/*******************************************************************************
 *******************************************************************************
 * @file ICM42605.cpp
 * @brief Contains the definitions of the ICM42605 class methods.
 *
 * Ported from the Arduino/ESP-IDF reference implementation to use the
 * sensors-library I2C and SPI abstractions.
 *
 * @version v0.2.0
 * @date 2025-07-11
 * @author daniel@sense-ai.co, Sense AI
 *******************************************************************************
 *******************************************************************************/

#include <cstdio>
#include <cstring>

#include "ICM42605.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ICM42605";

/*=============================================================================
 *  Construction
 *===========================================================================*/

ICM42605::ICM42605(I2C& bus, uint8_t address)
    : useSpi_(false), i2c_(&bus), address_(address) {
}

ICM42605::ICM42605(SPI& bus, gpio_num_t csPin, int spiClockHz)
    : useSpi_(true), spi_(&bus), csPin_(csPin), spiClockHz_(spiClockHz) {
}

ICM42605::ICM42605(SPI& bus, gpio_num_t csPin, spi_device_handle_t sharedDevice)
    : useSpi_(true), spi_(&bus), csPin_(csPin), spiDev_(sharedDevice), softCs_(true) {
}

/*=============================================================================
 *  Sensor interface
 *===========================================================================*/

esp_err_t ICM42605::init(void) {
    return init(AccelScale::kAFS_16G, GyroScale::kGFS_2000DPS, AccelODR::kAODR_1000Hz,
                GyroODR::kGODR_1000Hz);
}

esp_err_t ICM42605::init(AccelScale aScale, GyroScale gScale, AccelODR aODR,
                         GyroODR gODR) {
    aRes_ = computeAccelRes(aScale);
    gRes_ = computeGyroRes(gScale);

    // Attach SPI device if using SPI transport
    if (useSpi_ && spiDev_ == nullptr) {
        ESP_LOGI(TAG, "Adding SPI device: CS=%d, clock=%d Hz, Mode3", csPin_, spiClockHz_);
        esp_err_t err = spi_->addDevice(csPin_, spiClockHz_,
                                        SPI::ClockMode::kMode3, &spiDev_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI addDevice failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "SPI device added OK, waiting 10ms for CS settle");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Try a raw SPI read before reset to check bus connectivity
    if (useSpi_) {
        uint8_t tx[2] = { 0x75 | 0x80, 0x00 };  // WHO_AM_I reg read
        uint8_t rx[2] = {};
        esp_err_t dbgErr = spiTransfer(tx, rx, 2);
        ESP_LOGI(TAG, "Pre-reset WHO_AM_I probe: tx=[%02X %02X] rx=[%02X %02X] err=%s",
                 tx[0], tx[1], rx[0], rx[1], esp_err_to_name(dbgErr));
    }

    // Soft-reset
    ESP_LOGI(TAG, "Performing soft reset");
    esp_err_t err = reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Reset done, waited 20ms");

    // Verify WHO_AM_I
    uint8_t whoAmI = 0;
    err = readRegister(Reg::kWhoAmI, &whoAmI);
    ESP_LOGI(TAG, "WHO_AM_I read: 0x%02X (err=%s)", whoAmI, esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I: %s", esp_err_to_name(err));
        return err;
    }
    if (whoAmI != kExpectedWhoAmI) {
        // Try reading a few more times in case the chip needs more wake-up time
        for (int retry = 0; retry < 3; retry++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            err = readRegister(Reg::kWhoAmI, &whoAmI);
            ESP_LOGI(TAG, "WHO_AM_I retry %d: 0x%02X (err=%s)", retry, whoAmI, esp_err_to_name(err));
            if (whoAmI == kExpectedWhoAmI) break;
        }
        if (whoAmI != kExpectedWhoAmI) {
            ESP_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X", whoAmI,
                     kExpectedWhoAmI);
            return ESP_ERR_NOT_FOUND;
        }
    }

    // Enable gyro + accel in Low-Noise mode
    err = writeRegister(Reg::kPwrMgmt0, 0x0F);
    if (err != ESP_OK) {
        return err;
    }
    // Datasheet recommends 200 us wait after changing sensor modes
    vTaskDelay(pdMS_TO_TICKS(1));

    // Gyro full-scale + ODR
    uint8_t gyroReg = static_cast<uint8_t>(gODR) | (static_cast<uint8_t>(gScale) << 5);
    err = writeRegister(Reg::kGyroConfig0, gyroReg);
    if (err != ESP_OK) {
        return err;
    }

    // Accel full-scale + ODR
    uint8_t accelReg = static_cast<uint8_t>(aODR) | (static_cast<uint8_t>(aScale) << 5);
    err = writeRegister(Reg::kAccelConfig0, accelReg);
    if (err != ESP_OK) {
        return err;
    }

    // Gyro filter: temp LPF ≈5 Hz, 1st-order gyro filter
    err = writeRegister(Reg::kGyroConfig1, 0xD0);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Initialised (WHO_AM_I=0x%02X)", whoAmI);
    return ESP_OK;
}

esp_err_t ICM42605::measure(void) {
    // Burst-read 14 bytes starting from TEMP_DATA1 (0x1D)
    esp_err_t err = readRegisters(Reg::kTempData1, rawBuffer_, 14);
    if (err != ESP_OK) {
        return err;
    }

    // Parse raw 16-bit values (big-endian)
    int16_t rawTemp = (static_cast<int16_t>(rawBuffer_[0]) << 8) | rawBuffer_[1];
    int16_t rawAccX = (static_cast<int16_t>(rawBuffer_[2]) << 8) | rawBuffer_[3];
    int16_t rawAccY = (static_cast<int16_t>(rawBuffer_[4]) << 8) | rawBuffer_[5];
    int16_t rawAccZ = (static_cast<int16_t>(rawBuffer_[6]) << 8) | rawBuffer_[7];
    int16_t rawGyrX = (static_cast<int16_t>(rawBuffer_[8]) << 8) | rawBuffer_[9];
    int16_t rawGyrY = (static_cast<int16_t>(rawBuffer_[10]) << 8) | rawBuffer_[11];
    int16_t rawGyrZ = (static_cast<int16_t>(rawBuffer_[12]) << 8) | rawBuffer_[13];

    // Convert to physical units
    temperature_ = (static_cast<float>(rawTemp) / 132.48f) + 25.0f;

    accel_[0] = static_cast<float>(rawAccX) * aRes_ - accelBias_[0];
    accel_[1] = static_cast<float>(rawAccY) * aRes_ - accelBias_[1];
    accel_[2] = static_cast<float>(rawAccZ) * aRes_ - accelBias_[2];

    gyro_[0] = static_cast<float>(rawGyrX) * gRes_ - gyroBias_[0];
    gyro_[1] = static_cast<float>(rawGyrY) * gRes_ - gyroBias_[1];
    gyro_[2] = static_cast<float>(rawGyrZ) * gRes_ - gyroBias_[2];

    return ESP_OK;
}

esp_err_t ICM42605::getReport(char* buff) const {
    int n = sprintf(buff, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f\n", accel_[0], accel_[1],
                    accel_[2], gyro_[0], gyro_[1], gyro_[2], temperature_);
    return (n > 0) ? ESP_OK : ESP_FAIL;
}

uint8_t ICM42605::getID(void) const {
    return kExpectedWhoAmI;
}

/*=============================================================================
 *  Calibration
 *===========================================================================*/

esp_err_t ICM42605::calibrate(void) {
    static constexpr int kSamples = 128;
    int32_t sumAcc[3] = {};
    int32_t sumGyr[3] = {};

    ESP_LOGI(TAG, "Calibrating — keep sensor flat and motionless");

    for (int i = 0; i < kSamples; ++i) {
        esp_err_t err = measure();
        if (err != ESP_OK) {
            return err;
        }
        // Accumulate raw-converted values (before bias subtraction, but bias
        // is still zero at this point or from a previous calibration).
        // We read from the already-parsed accel_/gyro_ arrays but add back
        // any existing bias to get the un-biased measurement.
        int16_t rawAccX = (static_cast<int16_t>(rawBuffer_[2]) << 8) | rawBuffer_[3];
        int16_t rawAccY = (static_cast<int16_t>(rawBuffer_[4]) << 8) | rawBuffer_[5];
        int16_t rawAccZ = (static_cast<int16_t>(rawBuffer_[6]) << 8) | rawBuffer_[7];
        int16_t rawGyrX = (static_cast<int16_t>(rawBuffer_[8]) << 8) | rawBuffer_[9];
        int16_t rawGyrY = (static_cast<int16_t>(rawBuffer_[10]) << 8) | rawBuffer_[11];
        int16_t rawGyrZ = (static_cast<int16_t>(rawBuffer_[12]) << 8) | rawBuffer_[13];

        sumAcc[0] += rawAccX;
        sumAcc[1] += rawAccY;
        sumAcc[2] += rawAccZ;
        sumGyr[0] += rawGyrX;
        sumGyr[1] += rawGyrY;
        sumGyr[2] += rawGyrZ;

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    accelBias_[0] = static_cast<float>(sumAcc[0]) * aRes_ / kSamples;
    accelBias_[1] = static_cast<float>(sumAcc[1]) * aRes_ / kSamples;
    accelBias_[2] = static_cast<float>(sumAcc[2]) * aRes_ / kSamples;
    gyroBias_[0] = static_cast<float>(sumGyr[0]) * gRes_ / kSamples;
    gyroBias_[1] = static_cast<float>(sumGyr[1]) * gRes_ / kSamples;
    gyroBias_[2] = static_cast<float>(sumGyr[2]) * gRes_ / kSamples;

    // Remove gravity from whichever axis is closest to ±1 g
    for (int i = 0; i < 3; ++i) {
        if (accelBias_[i] > 0.8f)
            accelBias_[i] -= 1.0f;
        else if (accelBias_[i] < -0.8f)
            accelBias_[i] += 1.0f;
    }

    ESP_LOGI(TAG, "Bias: aX=%.4f aY=%.4f aZ=%.4f gX=%.4f gY=%.4f gZ=%.4f", accelBias_[0],
             accelBias_[1], accelBias_[2], gyroBias_[0], gyroBias_[1], gyroBias_[2]);

    return ESP_OK;
}

/*=============================================================================
 *  Control
 *===========================================================================*/

esp_err_t ICM42605::reset(void) {
    esp_err_t err = writeRegister(Reg::kDeviceConfig, 0x01);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // Conservative: Arduino lib waits ~11 ms
    return ESP_OK;
}

esp_err_t ICM42605::getIntStatus(uint8_t* status) const {
    return readRegister(Reg::kIntStatus, status);
}

/*=============================================================================
 *  Getters
 *===========================================================================*/

float ICM42605::getAccelX(void) const {
    return accel_[0];
}
float ICM42605::getAccelY(void) const {
    return accel_[1];
}
float ICM42605::getAccelZ(void) const {
    return accel_[2];
}
float ICM42605::getGyroX(void) const {
    return gyro_[0];
}
float ICM42605::getGyroY(void) const {
    return gyro_[1];
}
float ICM42605::getGyroZ(void) const {
    return gyro_[2];
}
float ICM42605::getTemperature(void) const {
    return temperature_;
}

const float* ICM42605::getAccel(void) const {
    return accel_;
}
const float* ICM42605::getGyro(void) const {
    return gyro_;
}
const uint8_t* ICM42605::getRawBuffer(void) const {
    return rawBuffer_;
}

/*=============================================================================
 *  Private helpers
 *===========================================================================*/

esp_err_t ICM42605::readRegister(uint8_t regAddr, uint8_t* value) const {
    if (useSpi_) {
        uint8_t tx[2] = { static_cast<uint8_t>(regAddr | 0x80), 0x00 };
        uint8_t rx[2] = {};
        esp_err_t err = spiTransfer(tx, rx, 2);
        *value = rx[1];
        return err;
    }
    return i2c_->read(address_, regAddr, value, 1);
}

esp_err_t ICM42605::readRegisters(uint8_t regAddr, uint8_t* data, size_t len) const {
    if (useSpi_) {
        // Max burst is 14 bytes (sensor data) + 1 address byte
        uint8_t tx[16] = {};
        uint8_t rx[16] = {};
        if (len > 15) len = 15;
        tx[0] = regAddr | 0x80;  // read bit
        esp_err_t err = spiTransfer(tx, rx, 1 + len);
        memcpy(data, rx + 1, len);
        return err;
    }
    return i2c_->read(address_, regAddr, data, len);
}

esp_err_t ICM42605::writeRegister(uint8_t regAddr, uint8_t value) const {
    if (useSpi_) {
        uint8_t tx[2] = { static_cast<uint8_t>(regAddr & 0x7F), value };
        return spiTransfer(tx, nullptr, 2);
    }
    return i2c_->write(address_, regAddr, &value, 1);
}

esp_err_t ICM42605::spiTransfer(uint8_t* tx, uint8_t* rx, size_t len) const {
    // Con CS por software, el CS rodea toda la transaccion; el bus lo adquiere
    // el wrapper SPI dentro de transfer/write.
    if (softCs_) gpio_set_level(csPin_, 0);
    esp_err_t err = rx ? spi_->transfer(&spiDev_, tx, rx, len)
                       : spi_->write(&spiDev_, tx, len);
    if (softCs_) gpio_set_level(csPin_, 1);
    return err;
}

float ICM42605::computeAccelRes(AccelScale scale) {
    switch (scale) {
        case AccelScale::kAFS_2G:
            return 2.0f / 32768.0f;
        case AccelScale::kAFS_4G:
            return 4.0f / 32768.0f;
        case AccelScale::kAFS_8G:
            return 8.0f / 32768.0f;
        case AccelScale::kAFS_16G:
            return 16.0f / 32768.0f;
    }
    return 16.0f / 32768.0f;
}

float ICM42605::computeGyroRes(GyroScale scale) {
    switch (scale) {
        case GyroScale::kGFS_15_125DPS:
            return 15.125f / 32768.0f;
        case GyroScale::kGFS_31_25DPS:
            return 31.25f / 32768.0f;
        case GyroScale::kGFS_62_5DPS:
            return 62.5f / 32768.0f;
        case GyroScale::kGFS_125DPS:
            return 125.0f / 32768.0f;
        case GyroScale::kGFS_250DPS:
            return 250.0f / 32768.0f;
        case GyroScale::kGFS_500DPS:
            return 500.0f / 32768.0f;
        case GyroScale::kGFS_1000DPS:
            return 1000.0f / 32768.0f;
        case GyroScale::kGFS_2000DPS:
            return 2000.0f / 32768.0f;
    }
    return 2000.0f / 32768.0f;
}
