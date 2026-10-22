/*******************************************************************************
 *******************************************************************************
 * @file ICM42605.hpp
 * @brief Contains the declarations of the ICM42605 class methods.
 *
 * The ICM-42605 is a 6-axis MEMS MotionTracking device that combines a 3-axis
 * gyroscope and a 3-axis accelerometer. It supports I2C (up to 1 MHz) and SPI
 * (up to 24 MHz) communication.
 *
 * @version v0.1.0
 * @date 2025-07-11
 * @author daniel@sense-ai.co, Sense AI
 *******************************************************************************
 *******************************************************************************/

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "smart_sensor_sense.hpp"
#include "spi_gp_sense.hpp"

/**
 * @class ICM42605
 * @brief A class to interface with the ICM-42605 6-axis IMU sensor.
 *
 * Provides accelerometer and gyroscope readings via I2C or SPI. Inherits from
 * the Sensor base class.
 */
class ICM42605 : public Sensor {
public:
    /*----------------------------------------------------------------------*/
    /*                         Enumerations                                 */
    /*----------------------------------------------------------------------*/

    /** @brief Full-scale range selection for the accelerometer. */
    enum class AccelScale : uint8_t {
        kAFS_2G = 0x03,
        kAFS_4G = 0x02,
        kAFS_8G = 0x01,
        kAFS_16G = 0x00  ///< Default
    };

    /** @brief Full-scale range selection for the gyroscope. */
    enum class GyroScale : uint8_t {
        kGFS_2000DPS = 0x00,  ///< Default
        kGFS_1000DPS = 0x01,
        kGFS_500DPS = 0x02,
        kGFS_250DPS = 0x03,
        kGFS_125DPS = 0x04,
        kGFS_62_5DPS = 0x05,
        kGFS_31_25DPS = 0x06,
        kGFS_15_125DPS = 0x07
    };

    /** @brief Output data rate selection for the accelerometer. */
    enum class AccelODR : uint8_t {
        kAODR_8000Hz = 0x03,
        kAODR_4000Hz = 0x04,
        kAODR_2000Hz = 0x05,
        kAODR_1000Hz = 0x06,  ///< Default
        kAODR_500Hz = 0x0F,
        kAODR_200Hz = 0x07,
        kAODR_100Hz = 0x08,
        kAODR_50Hz = 0x09,
        kAODR_25Hz = 0x0A,
        kAODR_12_5Hz = 0x0B,
        kAODR_6_25Hz = 0x0C,
        kAODR_3_125Hz = 0x0D,
        kAODR_1_5625Hz = 0x0E
    };

    /** @brief Output data rate selection for the gyroscope. */
    enum class GyroODR : uint8_t {
        kGODR_8000Hz = 0x03,
        kGODR_4000Hz = 0x04,
        kGODR_2000Hz = 0x05,
        kGODR_1000Hz = 0x06,  ///< Default
        kGODR_500Hz = 0x0F,
        kGODR_200Hz = 0x07,
        kGODR_100Hz = 0x08,
        kGODR_50Hz = 0x09,
        kGODR_25Hz = 0x0A,
        kGODR_12_5Hz = 0x0B
    };

    /*----------------------------------------------------------------------*/
    /*                         Constructor                                  */
    /*----------------------------------------------------------------------*/

    /**
     * @brief Construct an ICM42605 over I2C.
     * @param bus     Reference to an initialised I2C bus object.
     * @param address 7-bit I2C slave address (default 0x69 when AD0 = 1).
     */
    ICM42605(I2C& bus, uint8_t address = kDefaultAddress);

    /**
     * @brief Construct an ICM42605 over SPI.
     * @param bus        Reference to an initialised SPI bus object.
     * @param csPin      GPIO pin used for chip-select.
     * @param spiClockHz SPI clock frequency in Hz (max 24 MHz, default 8 MHz).
     */
    ICM42605(SPI& bus, gpio_num_t csPin, int spiClockHz = 8000000);

    /**
     * @brief Construct an ICM42605 on a shared SPI device with software CS.
     *
     * Several chips share one device handle registered without a hardware CS
     * (spics_io_num = -1); this driver drives @p csPin itself around every
     * transfer. Needed when a host has fewer CS lines than chips (SPI3 on the
     * ESP32-S3 has three). @p csPin must already be an output held high.
     * @param bus          Reference to an initialised SPI bus object.
     * @param csPin        GPIO pin used for chip-select, driven in software.
     * @param sharedDevice Device handle added with no CS pin.
     */
    ICM42605(SPI& bus, gpio_num_t csPin, spi_device_handle_t sharedDevice);

    /*----------------------------------------------------------------------*/
    /*                         Sensor interface                             */
    /*----------------------------------------------------------------------*/

    /**
     * @brief Initialises the ICM42605 with default configuration.
     *
     * Default: ±16 g accel, ±2000 dps gyro, 1 kHz ODR for both.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t init(void) override;

    /**
     * @brief Initialises the ICM42605 with custom configuration.
     * @param aScale  Accelerometer full-scale range.
     * @param gScale  Gyroscope full-scale range.
     * @param aODR    Accelerometer output data rate.
     * @param gODR    Gyroscope output data rate.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t init(AccelScale aScale, GyroScale gScale, AccelODR aODR, GyroODR gODR);

    /**
     * @brief Reads all sensor axes (accel + gyro + temperature).
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t measure(void) override;

    /**
     * @brief Writes a CSV-formatted report into the buffer.
     *
     * Format: "ax,ay,az,gx,gy,gz,temp\n"
     * @param buff Buffer to write the report string.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t getReport(char* buff) const override;

    /**
     * @brief Returns the sensor ID (WHO_AM_I value).
     * @return uint8_t Expected 0x42 for ICM-42605.
     */
    uint8_t getID(void) const override;

    /*----------------------------------------------------------------------*/
    /*                         Calibration                                  */
    /*----------------------------------------------------------------------*/

    /**
     * @brief Computes accelerometer and gyroscope offset biases.
     *
     * The sensor must be flat and motionless during this call. Takes ~128
     * samples with 10 ms spacing.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t calibrate(void);

    /*----------------------------------------------------------------------*/
    /*                         Control                                      */
    /*----------------------------------------------------------------------*/

    /**
     * @brief Performs a soft-reset of the device.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t reset(void);

    /**
     * @brief Reads the INT_STATUS register.
     * @param status Pointer to store the status byte.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t getIntStatus(uint8_t* status) const;

    /*----------------------------------------------------------------------*/
    /*                         Getters                                      */
    /*----------------------------------------------------------------------*/

    float getAccelX(void) const;
    float getAccelY(void) const;
    float getAccelZ(void) const;
    float getGyroX(void) const;
    float getGyroY(void) const;
    float getGyroZ(void) const;
    float getTemperature(void) const;

    /**
     * @brief Returns a pointer to the 3-element accel array [x, y, z] in g.
     */
    const float* getAccel(void) const;

    /**
     * @brief Returns a pointer to the 3-element gyro array [x, y, z] in dps.
     */
    const float* getGyro(void) const;

    /**
     * @brief Returns the raw 14-byte sensor data from the last measure().
     * Order: temp(2), ax(2), ay(2), az(2), gx(2), gy(2), gz(2).
     */
    const uint8_t* getRawBuffer(void) const;

private:
    /*----------------------------------------------------------------------*/
    /*                         Constants                                    */
    /*----------------------------------------------------------------------*/

    static constexpr uint8_t kDefaultAddress = 0x69;
    static constexpr uint8_t kExpectedWhoAmI = 0x42;

    /** @brief ICM-42605 register addresses (Bank 0). */
    enum Reg : uint8_t {
        kWhoAmI = 0x75,
        kDeviceConfig = 0x11,
        kPwrMgmt0 = 0x4E,
        kAccelConfig0 = 0x50,
        kGyroConfig0 = 0x4F,
        kGyroConfig1 = 0x51,
        kTempData1 = 0x1D,
        kTempData0 = 0x1E,
        kAccelDataX1 = 0x1F,
        kAccelDataX0 = 0x20,
        kAccelDataY1 = 0x21,
        kAccelDataY0 = 0x22,
        kAccelDataZ1 = 0x23,
        kAccelDataZ0 = 0x24,
        kGyroDataX1 = 0x25,
        kGyroDataX0 = 0x26,
        kGyroDataY1 = 0x27,
        kGyroDataY0 = 0x28,
        kGyroDataZ1 = 0x29,
        kGyroDataZ0 = 0x2A,
        kIntStatus = 0x2D,
        kIntfConfig0 = 0x4C,
        kIntfConfig1 = 0x4D,
        kIntConfig = 0x14,
        kIntConfig0 = 0x63,
        kIntConfig1 = 0x64,
        kIntSource0 = 0x65,
        kFifoConfig = 0x16,
        kFifoConfig1 = 0x5F,
        kSignalPathReset = 0x4B,
        kDriveConfig = 0x13,
        kRegBankSel = 0x76
    };

    /*----------------------------------------------------------------------*/
    /*                         Member variables                             */
    /*----------------------------------------------------------------------*/

    bool useSpi_ = false;

    // I2C transport
    I2C* i2c_ = nullptr;
    uint8_t address_ = kDefaultAddress;

    // SPI transport
    SPI* spi_ = nullptr;
    gpio_num_t csPin_ = GPIO_NUM_NC;
    int spiClockHz_ = 8000000;
    mutable spi_device_handle_t spiDev_ = nullptr;
    bool softCs_ = false;  ///< true: shared device, csPin_ driven by this driver

    float aRes_ = 0.0f;  ///< Accel resolution (g per LSB)
    float gRes_ = 0.0f;  ///< Gyro resolution (dps per LSB)

    uint8_t rawBuffer_[14] = {};  ///< Raw sensor data (temp + accel + gyro)
    float accel_[3] = {};         ///< Calibrated accelerometer [x,y,z] in g
    float gyro_[3] = {};          ///< Calibrated gyroscope [x,y,z] in dps
    float temperature_ = 0.0f;

    float accelBias_[3] = {};  ///< Accel offset biases in g
    float gyroBias_[3] = {};   ///< Gyro offset biases in dps

    /*----------------------------------------------------------------------*/
    /*                         Private helpers                              */
    /*----------------------------------------------------------------------*/

    esp_err_t readRegister(uint8_t regAddr, uint8_t* value) const;
    esp_err_t readRegisters(uint8_t regAddr, uint8_t* data, size_t len) const;
    esp_err_t writeRegister(uint8_t regAddr, uint8_t value) const;
    /// SPI transfer with CS handling; rx == nullptr means write-only.
    esp_err_t spiTransfer(uint8_t* tx, uint8_t* rx, size_t len) const;

    static float computeAccelRes(AccelScale scale);
    static float computeGyroRes(GyroScale scale);
};
