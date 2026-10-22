/*
 * main_test_board.cpp -- Prueba de hardware de la board sEMG (v0.2)
 * Solo ESP-IDF puro (sin librerias "sense").
 *
 *   1) LED RGB WS2812 (IO45): rojo -> verde -> azul   (RMT)
 *   2) SD  (SPI2: CS=10, SCK=12, MISO=13, MOSI=11): monta, escribe y lee un archivo
 *   3) IMU ICM-42605 (SPI3, CS_IMU=38): WHO_AM_I + lectura continua
 *   4) Boton (GPIO8, pull-up, activo en LOW)
 *
 * LED final: verde = SD e IMU OK, rojo = alguno fallo.
 * Pines MOSI/SCK/MISO del IMU (35/36/37) tomados de main4ADC.cpp; ajusta si cambian.
 *
 * src/CMakeLists.txt:
 *   idf_component_register(SRCS "main.cpp"
 *       REQUIRES driver esp_driver_gpio esp_driver_spi esp_driver_rmt fatfs sdmmc esp_timer)
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/rmt_tx.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

/* ── Pines ───────────────────────────────────────────────────────────────── */
#define SD_CS_PIN     GPIO_NUM_10
#define SD_SCK_PIN    GPIO_NUM_12
#define SD_MISO_PIN   GPIO_NUM_13
#define SD_MOSI_PIN   GPIO_NUM_11
#define SD_SPI_HOST   SPI2_HOST

#define LED_GPIO      GPIO_NUM_45
#define BOTON_GPIO    GPIO_NUM_8

#define IMU_MOSI_PIN  GPIO_NUM_35
#define IMU_SCK_PIN   GPIO_NUM_36
#define IMU_MISO_PIN  GPIO_NUM_37
#define IMU_CS_PIN    GPIO_NUM_38   // CS_IMU
#define IMU_SPI_HOST  SPI3_HOST

/* ── ICM-42605: registros (banco 0) ──────────────────────────────────────── */
#define ICM_DEVICE_CONFIG   0x11
#define ICM_TEMP_DATA1      0x1D   // luego accel (6 B) y gyro (6 B)
#define ICM_PWR_MGMT0       0x4E
#define ICM_GYRO_CONFIG0    0x4F
#define ICM_ACCEL_CONFIG0   0x50
#define ICM_WHO_AM_I        0x75
#define ICM_REG_BANK_SEL    0x76
#define ICM42605_ID         0x42
#define ICM42607_ID         0x60   // por si la board trae otra variante

// FS accel +-4 g -> 8192 LSB/g ; FS gyro +-500 dps -> 65.5 LSB/dps
#define ACCEL_LSB_PER_G     8192.0f
#define GYRO_LSB_PER_DPS    65.5f

static bool ledOK = true;   // solo verificable visualmente
static bool sdOK  = false;
static bool imuOK = false;

/* ── 1) LED WS2812 ───────────────────────────────────────────────────────── */
static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

static void led_init(void) {
    rmt_tx_channel_config_t tc = {};
    tc.clk_src = RMT_CLK_SRC_DEFAULT;
    tc.gpio_num = LED_GPIO;
    tc.mem_block_symbols = 64;
    tc.resolution_hz = 10000000;   // 10 MHz -> 0.1 us por tick
    tc.trans_queue_depth = 4;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tc, &led_chan));

    rmt_bytes_encoder_config_t ec = {};
    ec.bit0.duration0 = 3;  ec.bit0.level0 = 1;   // 0.3 us alto
    ec.bit0.duration1 = 9;  ec.bit0.level1 = 0;   // 0.9 us bajo
    ec.bit1.duration0 = 9;  ec.bit1.level0 = 1;   // 0.9 us alto
    ec.bit1.duration1 = 3;  ec.bit1.level1 = 0;   // 0.3 us bajo
    ec.flags.msb_first = 1;
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&ec, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = {};
    tx.loop_count = 0;
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, grb, sizeof(grb), &tx));
    rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
}

static void testLed(void) {
    printf("\n[LED] GPIO%d: rojo, verde, azul\n", (int)LED_GPIO);
    led_init();
    const uint8_t c[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };
    const char*  n[3]     = { "rojo", "verde", "azul" };
    for (int i = 0; i < 3; i++) {
        printf("[LED] %s\n", n[i]);
        led_set(c[i][0], c[i][1], c[i][2]);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    printf("[LED] Verifica visualmente que viste los 3 colores.\n");
}

/* ── 2) SD ───────────────────────────────────────────────────────────────── */
static void testSD(void) {
    printf("\n[SD] CS=%d SCK=%d MISO=%d MOSI=%d\n",
           (int)SD_CS_PIN, (int)SD_SCK_PIN, (int)SD_MISO_PIN, (int)SD_MOSI_PIN);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 400;   // conservador para la prueba

    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI_PIN;
    bus.miso_io_num = SD_MISO_PIN;
    bus.sclk_io_num = SD_SCK_PIN;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) { printf("[SD] FALLO spi_bus_initialize: %s\n", esp_err_to_name(ret)); return; }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_PIN;
    slot.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 3;
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = NULL;
    int intentos = 0;
    do {
        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
        if (ret != ESP_OK) { intentos++; vTaskDelay(pdMS_TO_TICKS(200)); }
    } while (ret != ESP_OK && intentos < 3);
    if (ret != ESP_OK) { printf("[SD] FALLO montaje: %s\n", esp_err_to_name(ret)); return; }
    printf("[SD] Montada OK: %s\n", card->cid.name);

    // Nombre en formato 8.3 (max 8 caracteres + extension): FatFs de ESP-IDF
    // viene sin nombres largos (LFN) por defecto, y "test_board.txt" fallaba.
    const char *path = "/sdcard/test.txt";
    const char *texto = "Prueba SD board sEMG v0.2\n";
    FILE *f = fopen(path, "w");
    if (!f) { printf("[SD] FALLO fopen escritura (errno=%d)\n", errno); return; }
    fputs(texto, f);
    fclose(f);

    char buf[64] = {0};
    f = fopen(path, "r");
    if (!f) { printf("[SD] FALLO fopen lectura\n"); return; }
    fgets(buf, sizeof(buf), f);
    fclose(f);

    if (strcmp(buf, texto) != 0) { printf("[SD] FALLO verificacion: leido='%s'\n", buf); return; }
    sdOK = true;
    printf("[SD] OK: escribio y leyo %s\n", path);
}

/* ── 3) IMU ──────────────────────────────────────────────────────────────── */
static spi_device_handle_t imu_dev = NULL;

static esp_err_t imu_read(uint8_t reg, uint8_t *data, size_t len) {
    uint8_t tx[1 + 16] = {0};
    uint8_t rx[1 + 16] = {0};
    if (len > 16) return ESP_ERR_INVALID_SIZE;
    tx[0] = reg | 0x80;
    spi_transaction_t t = {};
    t.length = 8 * (1 + len);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t e = spi_device_transmit(imu_dev, &t);
    if (e == ESP_OK) memcpy(data, rx + 1, len);
    return e;
}

static esp_err_t imu_write(uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = tx;
    return spi_device_transmit(imu_dev, &t);
}

static void testIMUInit(void) {
    printf("\n[IMU] CS=%d MOSI=%d MISO=%d SCK=%d\n",
           (int)IMU_CS_PIN, (int)IMU_MOSI_PIN, (int)IMU_MISO_PIN, (int)IMU_SCK_PIN);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = IMU_MOSI_PIN;
    bus.miso_io_num = IMU_MISO_PIN;
    bus.sclk_io_num = IMU_SCK_PIN;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32;
    esp_err_t e = spi_bus_initialize(IMU_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { printf("[IMU] FALLO spi_bus_initialize: %s\n", esp_err_to_name(e)); return; }

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 1000000;   // 1 MHz
    dev.mode = 0;
    dev.spics_io_num = IMU_CS_PIN;
    dev.queue_size = 2;
    e = spi_bus_add_device(IMU_SPI_HOST, &dev, &imu_dev);
    if (e != ESP_OK) { printf("[IMU] FALLO add_device: %s\n", esp_err_to_name(e)); return; }

    vTaskDelay(pdMS_TO_TICKS(50));
    imu_write(ICM_REG_BANK_SEL, 0x00);
    imu_write(ICM_DEVICE_CONFIG, 0x01);   // soft reset
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t id = 0;
    e = imu_read(ICM_WHO_AM_I, &id, 1);
    printf("[IMU] WHO_AM_I = 0x%02X (esperado 0x%02X)\n", id, ICM42605_ID);
    if (e != ESP_OK || (id != ICM42605_ID && id != ICM42607_ID)) {
        printf("[IMU] FALLO: no responde o ID inesperado (revisa CS/pines/soldadura)\n");
        return;
    }

    imu_write(ICM_ACCEL_CONFIG0, (2 << 5) | 0x07);   // +-4 g, 200 Hz
    imu_write(ICM_GYRO_CONFIG0,  (2 << 5) | 0x07);   // +-500 dps, 200 Hz
    imu_write(ICM_PWR_MGMT0, 0x0F);                  // accel + gyro en low-noise
    vTaskDelay(pdMS_TO_TICKS(50));

    imuOK = true;
    printf("[IMU] Init OK\n");
}

static void imuLeerMuestra(void) {
    if (!imuOK) return;
    uint8_t d[14];
    if (imu_read(ICM_TEMP_DATA1, d, 14) != ESP_OK) { printf("[IMU] error de lectura\n"); return; }
    int16_t t  = (int16_t)((d[0]  << 8) | d[1]);
    int16_t ax = (int16_t)((d[2]  << 8) | d[3]);
    int16_t ay = (int16_t)((d[4]  << 8) | d[5]);
    int16_t az = (int16_t)((d[6]  << 8) | d[7]);
    int16_t gx = (int16_t)((d[8]  << 8) | d[9]);
    int16_t gy = (int16_t)((d[10] << 8) | d[11]);
    int16_t gz = (int16_t)((d[12] << 8) | d[13]);
    printf("[IMU] acc[g]=% .3f % .3f % .3f | gyro[dps]=% .2f % .2f % .2f | T=%.1fC\n",
           ax / ACCEL_LSB_PER_G, ay / ACCEL_LSB_PER_G, az / ACCEL_LSB_PER_G,
           gx / GYRO_LSB_PER_DPS, gy / GYRO_LSB_PER_DPS, gz / GYRO_LSB_PER_DPS,
           t / 132.48f + 25.0f);
}

/* ── app_main ────────────────────────────────────────────────────────────── */
static void led_estado_final(void) {
    if (sdOK && imuOK) led_set(0, 255, 0);
    else               led_set(255, 0, 0);
}

#ifdef __cplusplus
extern "C" {
#endif
void app_main(void);
#ifdef __cplusplus
}
#endif

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\n===== TEST BOARD sEMG v0.2: LED / SD / IMU / BOTON =====\n");

    testLed();
    testSD();
    testIMUInit();

    printf("\n----- RESUMEN -----\n");
    printf("LED : %s\n", ledOK ? "(visual)" : "FALLO");
    printf("SD  : %s\n", sdOK  ? "OK" : "FALLO");
    printf("IMU : %s\n", imuOK ? "OK" : "FALLO");
    led_estado_final();

    gpio_config_t b = {};
    b.pin_bit_mask = (1ULL << BOTON_GPIO);
    b.mode = GPIO_MODE_INPUT;
    b.pull_up_en = GPIO_PULLUP_ENABLE;
    b.pull_down_en = GPIO_PULLDOWN_DISABLE;
    b.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&b);
    printf("\nLoop: IMU a 5 Hz. Presiona el boton (GPIO%d) para probarlo.\n", (int)BOTON_GPIO);

    bool prev = true;   // reposo = HIGH
    int tick = 0;
    while (true) {
        bool now = gpio_get_level(BOTON_GPIO);
        if (prev && !now) {
            printf("[BTN] presionado\n");
            led_set(255, 180, 0);
        } else if (!prev && now) {
            printf("[BTN] soltado\n");
            led_estado_final();
        }
        prev = now;

        if (++tick >= 20) {   // 20 * 10 ms = 200 ms
            tick = 0;
            imuLeerMuestra();
        }
        // 10 ms = 1 tick con tick de 100 Hz (con 5 ms daba 0 ticks y no cedia la CPU)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}