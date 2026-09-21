/*******************************************************************************
 * main4ADC.cpp — EMG8 Bracelet Firmware  (v4 — multi-file SD + label protocol)
 *
 * 4× ADS1015 mixed-rate single-shot round-robin with ALERT/RDY interrupts
 * (measured in All mode, see #CNT):
 *   ch 0, 2 = fast  (raw EMG   ≈ 554 Hz per channel)
 *   ch 1, 3 = slow  (envelope  ≈  28 Hz per channel)
 *
 * ICM-42605 6-axis IMU at 200 Hz via SPI
 *
 * Multi-file SD logging  ·  UART CSV @460800  ·  Reed switch + serial commands
 *
 * SD layout per session directory  s_<MAC>_<epoch>/  (one set per recording,
 * <nnn> increments on every recording start — pause/resume never truncates)
 *   M<nnn>.bin — master  (32-byte header + 12-byte label events)
 *   R<nnn>.bin — raw EMG  (8-byte Sample records, ch 0-1)
 *   E<nnn>.bin — envelope (8-byte Sample records, ch 2-3)
 *   I<nnn>.bin — IMU      (20-byte ImuSample records)
 *
 * UART baud 460800.  Protocol:
 *   PC→ESP: '0' stop · '1-4' mode · 'S<0-7>' sensor · '?' status · 'L<id>,<rep>\n' label
 *           'V0'/'V1' 5V · 'F' list files · 'G<path>\n' transfer file
 *   ESP→PC: #READY · #MODE:N · #CD:N · #REC · #STOP · #LABEL:id,rep
 *           #STATUS:mode,rec,sd,imu,mV,%,rawDrops,envDrops,imuDrops
 *           H,… · D,ts,… · #5V:0/1
 *
 * [SYNC] Ademas del protocolo de arriba, este lado (maestro) manda
 * periodicamente un pulso de sincronizacion de reloj al ESP de medicion
 * (impedancia/presion/temperatura), para que sus timestamps guardados en
 * SD queden en la misma base de tiempo que los Sample/ImuSample de este
 * lado (ambos expresados como "microsegundos desde que arranco ESTA
 * grabacion", la misma referencia que ya usa recordingTimestampUs()).
 *
 * UNDERDEVELOPMENT BY DANIEL ESCOBAR. daniel@sense-ai.co
 * Special thanks to Sense-AI! <3
 ******************************************************************************/
#include <cstring>
#include <string>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"     // IRAM_ATTR (button ISR)
#include "driver/uart.h"
#include "driver/gpio_filter.h"
#include "driver/gpio.h"  // gpio_config, gpio_install_isr_service, gpio_isr_handler_add (button ISR)
#include "esp_mac.h"
#include "ff.h"

#include "ADS1015.hpp"
#include "ICM42605.hpp"
#include "actuators_sense.hpp"
#include "switch_sense.hpp"
#include "sd_storage_sense.hpp"
#include "spi_gp_sense.hpp"
#include "BatteryManager.hpp"

/* ── Pin Map ───────────────────────────────────────────────────────────────── */

#ifdef EMG8_ADC_TIMING
#include "bench_clock_probe.hpp"
#endif

// I2C Bus 0 → ADC1 (ADDR_GND), ADC2 (ADDR_VCC)
static constexpr gpio_num_t kSDA0 = GPIO_NUM_6;
static constexpr gpio_num_t kSCL0 = GPIO_NUM_7;

// I2C Bus 1 → ADC3 (ADDR_GND), ADC4 (ADDR_VCC)
static constexpr gpio_num_t kSDA1 = GPIO_NUM_45;
static constexpr gpio_num_t kSCL1 = GPIO_NUM_47;

// ALERT / RDY per ADC. Bench routing measured with isolated conversions.
static constexpr gpio_num_t kRDY[] = {
#if defined(EMG8_ADC_TIMING) || defined(EMG8_BENCH_RDY_MAP)
    GPIO_NUM_15, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40
#else
    // Preserve the bracelet default until its wiring is confirmed.
    GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_15
#endif
};

// SD Card (SPI2)
static constexpr gpio_num_t kSD_CS   = GPIO_NUM_10;
static constexpr gpio_num_t kSD_MOSI = GPIO_NUM_11;
static constexpr gpio_num_t kSD_SCK  = GPIO_NUM_12;
static constexpr gpio_num_t kSD_MISO = GPIO_NUM_13;

// Peripherals
static constexpr gpio_num_t kLED  = GPIO_NUM_2;   // WS2812 RGB
static constexpr gpio_num_t kREED = GPIO_NUM_9;    // Reed switch

// Physical backup/OR button:
// short press starts the test, held 3+ sec stops it in a controlled
// way. Pull-up (active LOW), NormallyOpen.
//
// NOTE: shares GPIO8 with kBAT_ADC below -- the battery voltage
// divider is not wired up on this board revision, so the pin was
// reassigned to the button instead. Because of this, botonIsrInit()
// is called AFTER battery->init() in app_main() (see botonIsrInit()
// and the BotonEdge/botonEdgeQ interrupt setup near checkStartButton()).

static constexpr gpio_num_t kBOTON_INICIO = GPIO_NUM_8;

// IMU SPI (SPI3 / VSPI)
static constexpr gpio_num_t kIMU_MOSI = GPIO_NUM_35;
static constexpr gpio_num_t kIMU_SCK  = GPIO_NUM_36;
static constexpr gpio_num_t kIMU_MISO = GPIO_NUM_37;
static constexpr gpio_num_t kIMU_CS   = GPIO_NUM_38;
static constexpr gpio_num_t kIMU_INT  = GPIO_NUM_16;

// Battery / Power (5V charging board) - Battery and charge information is
// read on the other ESP32 (ESP32 #2), not used on this board.
/*static constexpr gpio_num_t kBAT_ADC  = GPIO_NUM_8;   // %Bat voltage divider
static constexpr gpio_num_t k5V_EN    = GPIO_NUM_14;  // 5V boost enable
static constexpr gpio_num_t kCHG      = GPIO_NUM_5;   // Charge status (active LOW)
static constexpr gpio_num_t kPGOOD    = GPIO_NUM_4;   // Power-good (active LOW)*/

/* ── Channel layout (NOT identical on every ADC) ───────────────────────────── */

/*
 * Medido en hardware el 2026-09-02, con el brazalete sobre la mesa y bateria
 * del sensor cargada: los ocho canales crudos entregan su pedestal de continua
 * (~605 cuentas) y los ocho de envolvente estan cerca de cero. Capturado por
 * UDP muestra a muestra, en modo 2 y modo 3 por separado para que cada patilla
 * corra a tasa completa; cada canal salio >=97.9 % en un solo nivel, y doce de
 * dieciseis al 100 %, asi que la asignacion es estable y no una carrera de mux.
 *
 *   ADC | crudo  | envolvente
 *   ----+--------+-----------
 *    1  | 0, 3   | 1, 2
 *    2  | 1, 3   | 0, 2
 *    3  | 0, 3   | 1, 2
 *    4  | 1, 3   | 0, 2
 *
 * Respecto a lo que suponia este firmware (crudo 0/2, envolvente 1/3): ch2 y
 * ch3 estan intercambiados en los cuatro ADC, y ch0/ch1 ademas en los ADC 2 y
 * 4. No es solo una etiqueta equivocada: con el mapa anterior las patillas
 * crudas de esos electrodos se muestreaban con el divisor /20 a ~23 Hz y las
 * de envolvente a ~460 Hz, o sea justo al reves de lo que hace falta.
 */
static constexpr uint8_t kRawCh[4][2] = {{0, 3}, {1, 3}, {0, 3}, {1, 3}};
static constexpr uint8_t kEnvCh[4][2] = {{1, 2}, {0, 2}, {1, 2}, {0, 2}};

static inline bool isRawCh(uint8_t adcId, uint8_t ch) {
    return ch == kRawCh[adcId & 3][0] || ch == kRawCh[adcId & 3][1];
}
// Single-shot triggered round-robin at 3300 SPS. Measured on hardware in All
// mode: ~554 Hz/ch raw and ~28 Hz/ch envelope per ADC, with symmetric
// per-channel counts and zero retriggers/I2C errors — this is the validated
// configuration, do not change it without re-checking #CNT.
static constexpr uint8_t kSLOW_DIV = 20;
static bool limitFastRate1000 = true;   // Boot at 1000 Hz; copied to workers at start.
static std::atomic<bool> recordingRate1000{false};  // SD header snapshot.
static constexpr auto    kADC_RATE = ADS1015::ConfigRate::Rate_3300Hz;

static constexpr uint16_t kIMU_ODR_HZ = 200;      // IMU polling rate

/* ── Types ─────────────────────────────────────────────────────────────────── */

#include "companion_link.hpp"
#include "emg8_types.hpp"   // Sample / ImuSample / LabelEvent (shared with net_stream)
#include <cstdarg>
#include "net_stream.hpp"

/* --- Modo UDP-solo -------------------------------------------------------
 * Todo lo que este firmware le dice al anfitrion sale por printf, en ~77
 * sitios. En vez de condicionar cada uno, se redirige printf aqui: un solo
 * punto, sin riesgo de olvidar una llamada, y los sitios siguen legibles.
 *
 * Los logs del driver no pasan por aqui; los apaga hostSetUartQuiet().
 * La recepcion del UART queda intacta a proposito (ver net_stream.hpp).
 */
static int hostPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static int hostPrintf(const char* fmt, ...) {
    if (hostUartQuiet()) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
#define printf hostPrintf

/** Comandos que llegan por UDP. Solo U0/U1: son escrituras atomicas de una
 *  bandera, sin efectos colaterales, y corren en la tarea de red. El resto
 *  del juego de comandos sigue entrando por UART, que nunca deja de leer. */
static void udpCommand(const char* data, int len) {
    if (len >= 2 && data[0] == 'U') {
        if (data[1] == '0') hostSetUartQuiet(true);
        else if (data[1] == '1') { hostSetUartQuiet(false); printf("#UART:1\n"); }
    }
}

enum class Mode : uint8_t { Idle = 0, All = 1, Raw = 2, Env = 3, Sensor = 4 };

// Sensor-test mode: one sEMG sensor at a time, for bench-checking each
// electrode before a real session. The 8 raw EMG sensors are numbered 0-7
// across the 4 ADCs; only the ADC hosting the selected sensor runs, with a
// single active channel, so the MUX never moves and that one sensor is
// sampled at the full hardware rate (~2400 Hz vs ~1100 Hz in All mode).
static constexpr uint8_t kNUM_SENSORS = 8;
static volatile uint8_t  testSensor   = 0;   // 0-7, selected with 'S<n>'

static inline uint8_t sensorAdc(uint8_t s)     { return (s % kNUM_SENSORS) / 2; }
// El sensor s vive en la patilla cruda del ADC que le toca, y cual es esa
// patilla depende del ADC (ver kRawCh).
static inline uint8_t sensorChannel(uint8_t s) {
    return kRawCh[sensorAdc(s) & 3][s % 2];
}

/* ── Globals ───────────────────────────────────────────────────────────────── */

static I2C i2c0(I2C_NUM_0, kSDA0, kSCL0, 1000000, false);
static I2C i2c1(I2C_NUM_1, kSDA1, kSCL1, 1000000, false);

static ADS1015* adc[4];
#ifndef EMG8_NO_SD
static SPI*     spiSD  = nullptr;
static SD*      sdCard = nullptr;
#endif
static SPI*     spiIMU = nullptr;
static RGB*     led    = nullptr;
static Switch*  reedSw = nullptr;
// Physical backup/OR button: interrupt-driven (see botonIsrInit() /
// botonEdgeQ below), not a Switch* like reedSw -- a polled read can miss
// a fast tap if the whole press+release happens between two loop
// iterations; the GPIO interrupt guarantees every edge is captured.
static BatteryManager* battery = nullptr;
static ICM42605*       imu     = nullptr;
static bool            imuOK   = false;
static bool            adcOK   = false;

static volatile Mode mode      = Mode::Idle;
static std::atomic<bool> recording{false};
static std::atomic<int64_t> recStart{0};     // µs epoch for timestamps
static std::atomic<bool> sdOK{false};           // Shared with the SD writer

// Separate queues for each stream → separate files
static QueueHandle_t rawQ   = nullptr;         // raw EMG (ch 0,1)
static QueueHandle_t envQ   = nullptr;         // envelope (ch 2,3)
static QueueHandle_t imuQ   = nullptr;
static QueueHandle_t labelQ = nullptr;         // LabelEvent from PC
static constexpr int kRAW_QLEN  = 12000;       // ~1.45 s at 8.3 kSa/s; SD buffering first
static constexpr int kENV_QLEN  = 1000;        // ≈ 2 s   @ ~460 Sa/s
static constexpr int kIMU_QLEN  = 400;         // ≈ 2 s   @ 200 Hz
static constexpr int kLABEL_QLEN = 32;

// Label state (set by PC via 'L' command)
static volatile uint16_t curGrasp = 0;         // current Ninapro grasp ID
static volatile uint16_t curRep   = 0;         // current repetition
static std::atomic<uint32_t> rawDrops{0};
static std::atomic<uint32_t> envDrops{0};
static std::atomic<uint32_t> imuDrops{0};

// Device MAC as hex string  "AABBCCDDEEFF\0"
static char macStr[13] = {};

// Session directory path (set once at SD init)
static std::string sessionDir;

static SessionPhase currentPhase = SessionPhase::Demo; // control-task owned

static uint32_t recordingTimestampUs() {
    return (uint32_t)(esp_timer_get_time() - recStart.load(std::memory_order_relaxed));
}

static void sendStartToSlave() {
    if (!companionStart(recStart.load(std::memory_order_relaxed), currentPhase))
        printf("#ERR:LINK_QUEUE\n");
}

static void stopCompanion(bool notify) {
    if (!companionStop(notify)) printf("#ERR:LINK_QUEUE\n");
}

// Metadata extension 1: reserved low byte = phase, next byte = event kind.
// Kind 1=label, 2=phase, 3=recording-start snapshot. Record size stays 12 bytes.
static bool saveMetadata(uint8_t kind, SessionPhase phase, uint16_t grasp,
                         uint16_t repetition, uint32_t timestamp) {
    if (!recording || !sdOK) return true;
    LabelEvent event{};
    event.ts = timestamp;
    event.grasp_id = grasp;
    event.repetition = repetition;
    event._reserved = static_cast<uint8_t>(phase) | (uint32_t(kind) << 8);
    if (labelQ && xQueueSend(labelQ, &event, 0) == pdTRUE) return true;
    printf("#ERR:METADATA_QUEUE\n");
    return false;
}

static std::atomic<uint32_t> sdWriteMaxUs{0}, sdSyncMaxUs{0};
static void trackIoTime(std::atomic<uint32_t>& maximum, int64_t start) {
    uint32_t duration = static_cast<uint32_t>(esp_timer_get_time() - start);
    uint32_t old = maximum.load(std::memory_order_relaxed);
    while (duration > old && !maximum.compare_exchange_weak(old, duration,
                                                            std::memory_order_relaxed)) {}
}
static NetStoragePressure storagePressure() {
    if (!sdOK || !rawQ) return NetStoragePressure::None;
    UBaseType_t pending = uxQueueMessagesWaiting(rawQ);
    if (pending >= 3 * kRAW_QLEN / 4) return NetStoragePressure::Defer;
    if (pending >= kRAW_QLEN / 4) return NetStoragePressure::Throttle;
    return NetStoragePressure::None;
}

static void resetDropCounters() {
    sdWriteMaxUs = 0; sdSyncMaxUs = 0;
    rawDrops.store(0, std::memory_order_relaxed);
    envDrops.store(0, std::memory_order_relaxed);
    imuDrops.store(0, std::memory_order_relaxed);
}

static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

/* Keep this line stable and machine-readable. It is the first application
 * line after a reset and is the quickest way to separate a firmware panic or
 * watchdog from a power interruption/brownout. */
static void printBootDiagnostics() {
    esp_reset_reason_t reason = esp_reset_reason();
    printf("#BOOT:reset=%s(%d),heap=%u,minheap=%u\n",
           resetReasonName(reason), (int)reason,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
}

static void updateStatusLed() {
    if (!led) return;

    if (!adcOK || !sdOK) {
        led->setColor(255, 0, 0);
    } else if (recording) {
        led->setColor(0, 255, 0);
    } else {
        led->setColor(0, 0, 255);
    }
    led->turnOn();
}

static void printBuildConfig() {
#ifdef EMG8_LEGACY_I2C_BENCH
    const char* i2c = "combined";
#else
    const char* i2c = "separate";
#endif
    printf("#CONFIG:I2C=%s,RDY=%d/%d/%d/%d,COMPANION_CORE=0\n",
           i2c, (int)kRDY[0], (int)kRDY[1], (int)kRDY[2], (int)kRDY[3]);
}

static void printStatusLine() {
    uint16_t mv = 0;
    uint8_t pct = 0;
    if (battery) {
        battery->measure();
        mv = battery->getVoltage();
        pct = battery->getPercentage();
    }

    printf("#STATUS:%d,%d,%d,%d,%u,%u,%lu,%lu,%lu\n",
           (int)mode,
           recording ? 1 : 0,
           sdOK ? 1 : 0,
           imuOK ? 1 : 0,
           mv,
           pct,
           (unsigned long)rawDrops.load(std::memory_order_relaxed),
           (unsigned long)envDrops.load(std::memory_order_relaxed),
           (unsigned long)imuDrops.load(std::memory_order_relaxed));
    printBuildConfig();
    printf("#PHASE:%s\n", phaseName(currentPhase));
    companionPrintStats();
    printf("#SDIO:WRITE_MAX_US=%lu,SYNC_MAX_US=%lu\n",
           (unsigned long)sdWriteMaxUs.load(), (unsigned long)sdSyncMaxUs.load());
    printf("#RATE:%s\n", limitFastRate1000 ? "1000" : "max");
    netPrintDiagnostics();
    constexpr uint32_t heapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    printf("#MEM:FREE=%lu,MIN=%lu,LARGEST=%lu\n",
           (unsigned long)heap_caps_get_free_size(heapCaps),
           (unsigned long)heap_caps_get_minimum_free_size(heapCaps),
           (unsigned long)heap_caps_get_largest_free_block(heapCaps));
}

/** Per-ADC/channel conversion counts for the recording that just ended —
 *  lets the host verify channel-rate symmetry (fast channels should match
 *  within ±1 per ADC, envelope likewise). */
static void printSampleCounts() {
    for (int a = 0; a < 4; a++) {
        printf("#CNT:%d,%lu,%lu,%lu,%lu,%lu,%lu\n", a + 1,
               (unsigned long)adc[a]->getSampleCount(0),
               (unsigned long)adc[a]->getSampleCount(1),
               (unsigned long)adc[a]->getSampleCount(2),
               (unsigned long)adc[a]->getSampleCount(3),
               (unsigned long)adc[a]->getI2cErrorCount(),
               (unsigned long)adc[a]->getRetriggerCount());
#ifdef EMG8_ADC_TIMING
        if (!hostUartQuiet()) adc[a]->printTiming(a + 1);
#endif
    }
}

/* One low-rate health line makes a long run diagnosable even when the reset
 * itself produces no useful text on the serial link. Queue occupancy is
 * sampled, not cleared, so this does not alter acquisition behaviour. */
static void printHealthLine() {
    printf("#HEALTH:%lu,%u,%u,%u,%u,%lu,%lu,%lu\n",
           (unsigned long)(esp_timer_get_time() / 1000),
           (unsigned)esp_get_free_heap_size(),
           (unsigned)uxQueueMessagesWaiting(rawQ),
           (unsigned)uxQueueMessagesWaiting(envQ),
           (unsigned)uxQueueMessagesWaiting(imuQ),
           (unsigned long)rawDrops.load(std::memory_order_relaxed),
           (unsigned long)envDrops.load(std::memory_order_relaxed),
           (unsigned long)imuDrops.load(std::memory_order_relaxed));
}

/* ── ADC conversion callback (called from each ADC's FreeRTOS task) ──────── */

static void onSample(uint8_t ch, int16_t val, uint32_t tsUs, void* arg) {
    uint8_t id = (uint8_t)(uintptr_t)arg;
    bool fast  = isRawCh(id, ch);

    // Drop channels the current mode doesn't need
    if (mode == Mode::Raw && !fast) return;
    if (mode == Mode::Env &&  fast) return;

    Sample s;
    // tsUs was captured in the DRDY ISR — unsigned 32-bit subtraction gives
    // the correct offset even across the µs-counter wrap (~71 min)
    s.ts  = tsUs - (uint32_t)recStart.load(std::memory_order_relaxed);
    s.adc = id;
    s.ch  = ch;
    s.val = val;

    // Without a usable card there is no storage consumer. Keep UDP independent.
    if (sdOK.load(std::memory_order_relaxed)) {
        if (fast) {
            if (xQueueSend(rawQ, &s, 0) != pdTRUE)
                rawDrops.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (xQueueSend(envQ, &s, 0) != pdTRUE)
                envDrops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (netStreamActive()) {
        fast ? netEnqueueRaw(s) : netEnqueueEnv(s);
    }
}

/* ── SD writer task — multi-file via raw FatFs, pinned to core 1, prio 5 ── */
/*
 * Files opened once per recording session (no open/close cycling):
 *   M.bin — master header + label events
 *   R.bin — raw EMG  (8-byte Sample; la patilla depende del ADC, ver kRawCh)
 *   E.bin — envelope (8-byte Sample; idem, ver kEnvCh)
 *   I.bin — IMU      (20-byte ImuSample)
 *
 * Uses raw f_open/f_write/f_sync to hold 4 FIL handles simultaneously
 * (the SD wrapper class only supports one open file at a time).
 */

static FIL filMaster, filRaw, filEnv, filImu;
static bool filesOpen = false;
static uint16_t recIndex = 0;   // per-recording file set index within the session

enum class SdCommand : uint8_t { Open, Close };
static QueueHandle_t sdCommands = nullptr;
static SemaphoreHandle_t sdCommandDone = nullptr;
static SemaphoreHandle_t imuRunMutex = nullptr;

// Only main submits commands; acknowledgement transfers file ownership.
// ADC and IMU producers are stopped before Close is submitted.
static void commandSdWriter(SdCommand command) {
    if (!sdCommands) return;
    xQueueSend(sdCommands, &command, portMAX_DELAY);
    xSemaphoreTake(sdCommandDone, portMAX_DELAY);
}

static void waitImuIdle() {
    if (!imuRunMutex) return;
    xSemaphoreTake(imuRunMutex, portMAX_DELAY);
    xSemaphoreGive(imuRunMutex);
}

static void sdReportError(const char* operation, const char* name, FRESULT error,
                          UINT requested = 0, UINT written = 0) {
    sdOK.store(false, std::memory_order_relaxed);
    printf("#ERR:SD_%s:%s,%d,%u,%u\n", operation, name, (int)error,
           (unsigned)requested, (unsigned)written);
    updateStatusLed();
}

static bool sdWriteChecked(FIL* file, const char* name, const void* data, UINT size) {
    UINT written = 0;
    FRESULT result = f_write(file, data, size, &written);
    if (result == FR_OK && written == size) return true;
    // FatFs may return FR_OK with a short write when the card is full.
    sdReportError("WRITE", name, result, size, written);
    return false;
}

static bool sdSyncChecked(FIL* file, const char* name) {
    FRESULT result = f_sync(file);
    if (result == FR_OK) return true;
    sdReportError("SYNC", name, result);
    return false;
}

static void sdCloseChecked(FIL* file, const char* name) {
    // f_close includes FatFs's final sync; do not ignore its result.
    FRESULT result = f_close(file);
    if (result != FR_OK) sdReportError("CLOSE", name, result);
}

static void sdCloseFiles();

static bool sdOpenFiles(const std::string& base) {
    // One file set per recording start (R000.bin, R001.bin, ...) so a
    // pause/resume or stop/start never truncates earlier data.
    char suffix[16];
    snprintf(suffix, sizeof(suffix), "%03u.bin", (unsigned)recIndex);
    std::string mPath = base + "/M" + suffix;
    std::string rPath = base + "/R" + suffix;
    std::string ePath = base + "/E" + suffix;
    std::string iPath = base + "/I" + suffix;

    FRESULT fr[4];
    fr[0] = f_open(&filMaster, mPath.c_str(), FA_CREATE_NEW | FA_WRITE);
    fr[1] = f_open(&filRaw,    rPath.c_str(), FA_CREATE_NEW | FA_WRITE);
    fr[2] = f_open(&filEnv,    ePath.c_str(), FA_CREATE_NEW | FA_WRITE);
    fr[3] = f_open(&filImu,    iPath.c_str(), FA_CREATE_NEW | FA_WRITE);
    if (fr[0] != FR_OK || fr[1] != FR_OK || fr[2] != FR_OK || fr[3] != FR_OK) {
        printf("#ERR:SD_OPEN:%d,%d,%d,%d\n", fr[0], fr[1], fr[2], fr[3]);
        if (fr[0] == FR_OK) sdCloseChecked(&filMaster, "M");
        if (fr[1] == FR_OK) sdCloseChecked(&filRaw, "R");
        if (fr[2] == FR_OK) sdCloseChecked(&filEnv, "E");
        if (fr[3] == FR_OK) sdCloseChecked(&filImu, "I");
        sdOK = false;
        updateStatusLed();
        return false;
    }
    recIndex++;

    // Write master header (32 bytes, v4)
    // [0-3] "EMG8"  [4] ver=4  [5] nADC  [6] nCh  [7] div
    // [8-11] epoch_s(u32)  [12-13] bat_mV  [14] bat_%  [15] bat_state
    // [16-17] imuODR(u16)  [18-23] MAC(6)  [24] mode  [25] rate cap  [26] metadata extension [27-31] reserved
    uint8_t hdr[32] = {};
    memcpy(hdr, "EMG8", 4);
    hdr[4] = 4;                  // version
    hdr[5] = 4;                  // number of ADCs
    hdr[6] = 4;                  // channels per ADC
    hdr[7] = kSLOW_DIV;
    uint32_t e32 = (uint32_t)(esp_timer_get_time() / 1000000);
    memcpy(hdr + 8, &e32, 4);
    if (battery) {
        battery->measure();
        uint16_t mv = battery->getVoltage();
        memcpy(hdr + 12, &mv, 2);
        hdr[14] = battery->getPercentage();
        hdr[15] = (uint8_t)battery->getState();
    }
    uint16_t odr16 = kIMU_ODR_HZ;
    memcpy(hdr + 16, &odr16, 2);
    // MAC bytes
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(hdr + 18, mac, 6);
    hdr[24] = (uint8_t)mode;
    hdr[26] = 1;  // metadata extension: phase/event kind in LabelEvent::_reserved
    hdr[25] = recordingRate1000.load(std::memory_order_relaxed) ? 1 : 0;  // v4 reserved byte: 0=max, 1=1000 Hz cap

    filesOpen = true;
    if (!sdWriteChecked(&filMaster, "M", hdr, sizeof(hdr)) ||
        !sdSyncChecked(&filMaster, "M")) {
        sdCloseFiles();
        return false;
    }
    return true;
}

static void sdCloseFiles() {
    if (!filesOpen) return;
    sdCloseChecked(&filRaw, "R");
    sdCloseChecked(&filEnv, "E");
    sdCloseChecked(&filImu, "I");
    sdCloseChecked(&filMaster, "M");
    filesOpen = false;
}

static void sdWriteTask(void*) {
    // Single owner; keep batch storage off the task's call stack.
    static Sample rawBuf[500], envBuf[100];
    static ImuSample imuBuf[50];
    static LabelEvent lblBuf[8];
    bool closing = false, dirty = false;
    TickType_t lastSync = xTaskGetTickCount();

    auto discardPending = [&]() {
        while (xQueueReceive(rawQ, rawBuf, 0) == pdTRUE)
            rawDrops.fetch_add(1, std::memory_order_relaxed);
        while (xQueueReceive(envQ, envBuf, 0) == pdTRUE)
            envDrops.fetch_add(1, std::memory_order_relaxed);
        while (xQueueReceive(imuQ, imuBuf, 0) == pdTRUE)
            imuDrops.fetch_add(1, std::memory_order_relaxed);
        while (xQueueReceive(labelQ, lblBuf, 0) == pdTRUE) {}
    };
    auto writeBatch = [&](QueueHandle_t queue, auto* buffer, int capacity,
                          FIL* file, const char* name, std::atomic<uint32_t>* drops) {
        int n = 0;
        while (n < capacity && xQueueReceive(queue, &buffer[n], 0) == pdTRUE) ++n;
        int64_t started = n ? esp_timer_get_time() : 0;
        bool ok = !n || sdWriteChecked(file, name, buffer, n * sizeof(buffer[0]));
        if (n) trackIoTime(sdWriteMaxUs, started);
        if (!ok) {
            // The failed batch is uncertain, even if some bytes were accepted.
            if (drops) drops->fetch_add(n, std::memory_order_relaxed);
            return -1;
        }
        return n;
    };

    while (true) {
        SdCommand command;
        if (xQueueReceive(sdCommands, &command, filesOpen ? 0 : portMAX_DELAY) == pdTRUE) {
            if (command == SdCommand::Open) {
                if (sdOK) sdOpenFiles(sessionDir);
                dirty = false;
                lastSync = xTaskGetTickCount();
                xSemaphoreGive(sdCommandDone);
            } else {
                closing = true;
            }
        }
        if (!filesOpen) {
            discardPending();
            if (closing) {
                closing = false;
                xSemaphoreGive(sdCommandDone);
            }
            continue;
        }

        int nR = writeBatch(rawQ, rawBuf, 500, &filRaw, "R", &rawDrops);
        int nE = nR < 0 ? 0 : writeBatch(envQ, envBuf, 100, &filEnv, "E", &envDrops);
        int nI = nR < 0 || nE < 0 ? 0 : writeBatch(imuQ, imuBuf, 50, &filImu, "I", &imuDrops);
        int nL = nR < 0 || nE < 0 || nI < 0 ? 0 :
            writeBatch(labelQ, lblBuf, 8, &filMaster, "M", nullptr);
        if (nR < 0 || nE < 0 || nI < 0 || nL < 0) {
            sdCloseFiles();
            discardPending();
            if (closing) {
                closing = false;
                xSemaphoreGive(sdCommandDone);
            }
            continue;
        }

        bool hadData = nR || nE || nI || nL;
        dirty |= hadData;
        if (closing && !hadData) {
            sdCloseFiles();
            closing = false;
            xSemaphoreGive(sdCommandDone);
            continue;
        }
        if (!closing && dirty &&
            (TickType_t)(xTaskGetTickCount() - lastSync) >= pdMS_TO_TICKS(500)) {
            // Evaluate all files even when one fails.
            int64_t syncStarted = esp_timer_get_time();
            bool ok = sdSyncChecked(&filRaw, "R");
            ok = sdSyncChecked(&filEnv, "E") && ok;
            ok = sdSyncChecked(&filImu, "I") && ok;
            ok = sdSyncChecked(&filMaster, "M") && ok;
            trackIoTime(sdSyncMaxUs, syncStarted);
            lastSync = xTaskGetTickCount();
            dirty = false;
            if (!ok) {
                sdCloseFiles();
                discardPending();
                continue;
            }
        }
        if (!hadData) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (!closing && uxQueueMessagesWaiting(rawQ) < 500 &&
                   uxQueueMessagesWaiting(envQ) < 100 &&
                   uxQueueMessagesWaiting(imuQ) < 50 &&
                   uxQueueMessagesWaiting(labelQ) < 8) {
            // Continuous tiny batches can keep this priority-5 task runnable
            // forever and starve the priority-4 IMU. With no significant
            // backlog, block for one tick; ADC workers still preempt us.
            vTaskDelay(1);
        }
    }
}

/* ── UART output task — pinned to core 0, ~50 Hz ──────────────────────────── */
/*  Protocol: line-based text, prefixed by a tag character.
 *
 *  Outgoing (ESP → PC):
 *    '#' lines = metadata / status
 *    'H' line  = CSV header   H,ts_us,adc1_0,...,ax,ay,az,gx,gy,gz,label,rep
 *    'D' line  = CSV data     D,12345,1234,-456,...,0.12,-0.03,0.98,...,5,2
 *
 *  Incoming (PC → ESP):
 *    '0'        stop
 *    '1'-'3'    mode
 *    '?'        query status
 *    'L<id>,<rep>\n'  set label
 *    'V0'/'V1'  5V off/on
 *    'F'        list SD files
 *    'G<path>\n' transfer SD file
 */

static void uartTask(void*) {
    // Divisor de la linea CSV cuando el UDP esta entregando: 50 iteraciones de
    // 20 ms = 1 Hz.
    constexpr int kCsvDividerOnNet = 50;
    int netQuietTick = 0;

    bool hdrDone = false;
    Mode lastHdrMode = Mode::Idle;
    uint8_t lastHdrSensor = 0xFF;

    while (true) {
        if (!recording) {
            hdrDone = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* En modo UDP-solo no basta con que printf no escriba: armar la linea
         * cuesta 16 lecturas de ADC y una docena de snprintf 50 veces por
         * segundo. Se salta entera. */
        if (hostUartQuiet()) {
            hdrDone = false;   // al volver, reimprimir la cabecera
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Reprint header when mode changes mid-recording
        if (mode != lastHdrMode) hdrDone = false;
        // ...and when the sensor under test changes, since 'S<n>' switches
        // sensors without changing mode: the single EMG column is named
        // after the sensor, so the old header would mislabel it.
        if (mode == Mode::Sensor && testSensor != lastHdrSensor) hdrDone = false;

        // Print header line once per recording start / mode change
        if (!hdrDone) {
            printf("H,ts_us");
            if (mode == Mode::Sensor) {
                // Single column: the sensor under test (s<n> = adc<a>_<ch>)
                uint8_t s = testSensor % kNUM_SENSORS;
                printf(",s%u_adc%u_%u", (unsigned)s,
                       (unsigned)(sensorAdc(s) + 1), (unsigned)sensorChannel(s));
            } else {
                for (int a = 0; a < 4; a++)
                    for (int c = 0; c < 4; c++) {
                        bool fast = isRawCh((uint8_t)a, (uint8_t)c);
                        if (mode == Mode::Raw && !fast) continue;
                        if (mode == Mode::Env &&  fast) continue;
                        // El sufijo dice que es la columna, no solo de donde
                        // sale: leer "adc2_1" sin saber el mapa confundia.
                        printf(",adc%d_%d%s", a + 1, c, fast ? "r" : "e");
                    }
            }
            if (imuOK)
                printf(",ax,ay,az,gx,gy,gz");
            printf(",label,rep\n");
            lastHdrMode = mode;
            lastHdrSensor = testSensor;
            hdrDone = true;
        }

        // While UDP is delivering the full-rate stream, the ~50 Hz CSV snapshot
        // is redundant: the host ignores its samples anyway (same channels, a
        // different clock — overlaying them is garbage). Drop to 1 Hz so the
        // link still shows liveness, labels and mode without spending UART
        // bandwidth or CPU on string formatting 50 times a second.
        if (netStreamActive() && ++netQuietTick < kCsvDividerOnNet) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        netQuietTick = 0;

        // One CSV line of latest readings
        char line[512];
        int  p = 0;

        // Timestamp (µs since recStart)
        uint32_t ts = recordingTimestampUs();
        p += snprintf(line + p, sizeof(line) - p, "D,%u", (unsigned)ts);

        if (mode == Mode::Sensor) {
            uint8_t s = testSensor % kNUM_SENSORS;
            p += snprintf(line + p, sizeof(line) - p, ",%d",
                          adc[sensorAdc(s)]->getLatestReading(sensorChannel(s)));
        } else {
            for (int a = 0; a < 4; a++)
                for (int c = 0; c < 4; c++) {
                    bool fast = isRawCh((uint8_t)a, (uint8_t)c);
                    if (mode == Mode::Raw && !fast) continue;
                    if (mode == Mode::Env &&  fast) continue;
                    p += snprintf(line + p, sizeof(line) - p, ",%d",
                                  adc[a]->getLatestReading(c));
                }
        }
        // Append IMU (latest measurement already done by imuTask)
        if (imuOK) {
            const float* ac = imu->getAccel();
            const float* gy = imu->getGyro();
            p += snprintf(line + p, sizeof(line) - p,
                          ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                          ac[0], ac[1], ac[2], gy[0], gy[1], gy[2]);
        }
        // Append current label
        p += snprintf(line + p, sizeof(line) - p, ",%u,%u",
                      (unsigned)curGrasp, (unsigned)curRep);
        line[p++] = '\n';
        line[p] = '\0';
        printf("%s", line);

        vTaskDelay(pdMS_TO_TICKS(20));  // ~50 Hz
    }
}

/* ── Per-bus ADC service tasks — event-driven via DRDY queues ──────────────── */
/*
 * One task per I2C bus (ADC1/2 on bus 0, ADC3/4 on bus 1) so transactions on
 * the two buses overlap instead of being serialized through one task. Each
 * ALERT ISR posts its ADC index to the bus queue; the task blocks on the
 * queue — no polling, no idle-sleep latency — and services exactly that ADC.
 * While one task blocks on an I2C transfer the other bus's task runs, and
 * when nothing converts both tasks sleep, leaving core 1 to SD/IMU/IDLE.
 *
 * The ADCs sample in single-shot mode, so the round-robin only advances when
 * we trigger it: a lost trigger write or a missed DRDY edge would otherwise
 * park that ADC forever. The queue wait therefore has a timeout, and on
 * expiry both ADCs on the bus get a stall check (a no-op unless one really
 * is overdue).
 */

static QueueHandle_t drdyQ[2] = {nullptr, nullptr};
static constexpr uint8_t kAdcStart = 4;
static constexpr uint8_t kAdcStop = 5;
static constexpr uint8_t kAdcRateWake = 6;
struct AdcBusControl {
    ADS1015::ChannelConfig configs[2][4];
    uint8_t counts[2] = {};
    bool rateLimited = false;
    SemaphoreHandle_t done = nullptr;
    esp_err_t result = ESP_OK;
};
static AdcBusControl adcControl[2];

// Only main submits commands. The completion semaphore transfers ownership of
// each configuration/result between main and the worker, without hot-path locks.
static esp_err_t commandAdcWorkers(uint8_t command) {
    for (int bus = 0; bus < 2; ++bus)
        xQueueSend(drdyQ[bus], &command, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    for (int bus = 0; bus < 2; ++bus) {
        xSemaphoreTake(adcControl[bus].done, portMAX_DELAY);
        if (adcControl[bus].result != ESP_OK) result = adcControl[bus].result;
    }
    return result;
}

static void adcRateWake(void* arg) {
    const int bus = (int)(intptr_t)arg;
    const uint8_t command = kAdcRateWake;
    // If full, the worker is already awake and will recheck its timer deadline.
    xQueueSend(drdyQ[bus], &command, 0);
}

static void adcBusTask(void* arg) {
    const int bus = (int)(intptr_t)arg;
    QueueHandle_t q = drdyQ[bus];
    esp_timer_handle_t rateTimer = nullptr;
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = adcRateWake;
    timerArgs.arg = arg;
    timerArgs.name = "adc_rate";
    ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &rateTimer));
    bool rateLimited = false;
    int64_t armedDeadline = 0;
    TickType_t lastRecoveryCheck = xTaskGetTickCount();
    uint8_t idx;
    while (true) {
        if (xQueueReceive(q, &idx, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (idx == kAdcStart || idx == kAdcStop) {
                auto& control = adcControl[bus];
                esp_timer_stop(rateTimer);
                armedDeadline = 0;
                rateLimited = idx == kAdcStart && control.rateLimited;
                control.result = ESP_OK;
                for (int slot = 0; slot < 2; ++slot) {
                    auto* device = adc[bus * 2 + slot];
                    if (device->isContinuousRunning()) {
                        esp_err_t err = device->stopContinuous();
                        if (err != ESP_OK) control.result = err;
                    }
                }
                // Both chips are stopped; queued edges belong to the old run.
                xQueueReset(q);
                if (idx == kAdcStart && control.result == ESP_OK) {
                    for (int slot = 0; slot < 2; ++slot) {
                        if (!control.counts[slot]) {
                            adc[bus * 2 + slot]->resetAcquisitionDiagnostics();
                            continue;
                        }
                        esp_err_t err = adc[bus * 2 + slot]->startMixedContinuousExternal(
                            control.configs[slot], control.counts[slot], kADC_RATE,
                            control.rateLimited);
                        if (err != ESP_OK) control.result = err;
                    }
                }
                xSemaphoreGive(control.done);
            } else if (idx < 4 && idx / 2 == bus) {
                adc[idx]->serviceConversion();
            } else if (idx == kAdcRateWake) {
                armedDeadline = 0;
                adc[bus * 2]->serviceConversion();
                adc[bus * 2 + 1]->serviceConversion();
            }
        }
        // A healthy partner keeps this queue active even if one ADC loses
        // its ready event. Check each chip's deadline independently of idle.
        const TickType_t now = xTaskGetTickCount();
        if (recording && (TickType_t)(now - lastRecoveryCheck) >= pdMS_TO_TICKS(1)) {
            lastRecoveryCheck = now;
            adc[bus * 2]->retriggerIfStalled();
            adc[bus * 2 + 1]->retriggerIfStalled();
        }
        if (rateLimited) {
            uint32_t delay = 0;
            for (int slot = 0; slot < 2; ++slot) {
                uint32_t candidate = adc[bus * 2 + slot]->rateWaitDelayUs();
                if (candidate && (!delay || candidate < delay)) delay = candidate;
            }
            if (delay) {
                const int64_t deadline = esp_timer_get_time() + delay;
                if (!armedDeadline || armedDeadline <= esp_timer_get_time() || deadline < armedDeadline) {
                    esp_timer_stop(rateTimer);
                    ESP_ERROR_CHECK(esp_timer_start_once(rateTimer, delay));
                    armedDeadline = deadline;
                }
            } else if (armedDeadline) {
                esp_timer_stop(rateTimer);
                armedDeadline = 0;
            }
        }
    }
}

/* ── IMU polling task — pinned to core 1, 200 Hz ──────────────────────────── */

static void imuTask(void*) {
    constexpr TickType_t period =
        pdMS_TO_TICKS(1000 / kIMU_ODR_HZ) > 0 ? pdMS_TO_TICKS(1000 / kIMU_ODR_HZ) : 1;
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        if (!recording || !imuOK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wake = xTaskGetTickCount();
            continue;
        }

        xSemaphoreTake(imuRunMutex, portMAX_DELAY);
        if (!recording.load(std::memory_order_relaxed)) {
            xSemaphoreGive(imuRunMutex);
            continue;
        }
        if (imu->measure() == ESP_OK) {
            const float* ac = imu->getAccel();
            const float* gy = imu->getGyro();
            float temp = imu->getTemperature();

            ImuSample s;
            s.ts = recordingTimestampUs();
            // Store as raw int16 scaled: accel in milli-g, gyro in deci-dps
            s.ax = (int16_t)(ac[0] * 1000.0f);
            s.ay = (int16_t)(ac[1] * 1000.0f);
            s.az = (int16_t)(ac[2] * 1000.0f);
            s.gx = (int16_t)(gy[0] * 10.0f);
            s.gy = (int16_t)(gy[1] * 10.0f);
            s.gz = (int16_t)(gy[2] * 10.0f);
            s.temp100 = (int16_t)(temp * 100.0f);
            if (sdOK.load(std::memory_order_relaxed) && xQueueSend(imuQ, &s, 0) != pdTRUE)
                imuDrops.fetch_add(1, std::memory_order_relaxed);
            if (netStreamActive()) netEnqueueImu(s);
        }

        xSemaphoreGive(imuRunMutex);
        vTaskDelayUntil(&wake, period);
    }
}

/* ── Countdown (visible on serial + LED, interruptible via '0') ────────────── */

static void handleSensorCommand();   // defined below; needed during countdown
static int  feedUartByte(uint8_t b); // defined below; same parser the loops use
static std::string sdListRoot();     // defined below; shared by 'F' and 'G'

static bool countdown(int seconds) {
    for (int i = seconds; i > 0; i--) {
        printf("#CD:%d\n", i);
        led->setColor(255, 180, 0);
        led->turnOn();
        // 10 × 100 ms = 1 second; check UART each tick
        for (int t = 0; t < 10; t++) {
            uint8_t rx;
            if (uart_read_bytes(UART_NUM_0, &rx, 1, pdMS_TO_TICKS(100)) > 0) {
                // Route through the same parser the command loops use, rather
                // than matching raw bytes. Matching raw bytes meant any '0'
                // *inside* a multi-byte command aborted the countdown: "S0"
                // aborted instead of selecting sensor 0, and "L0,1" -- which
                // hosts routinely send right after a
                // only a genuinely standalone '0' can reach the abort test.
                int c = feedUartByte(rx);
                if (c == '0') {
                    led->turnOff();
                    printf("#CD:ABORT\n");
                    return false;   // aborted
                }
                // Sensor selection is safe to honour mid-countdown: it only
                // arms the selection, the ADCs aren't running yet.
                if (c == 'S') handleSensorCommand();
            }
            if (t == 5) led->turnOff();   // blink: 500ms on, 500ms off
        }
    }
    printf("#CD:0\n");
    led->turnOn();
    return true;   // completed normally
}

/* ── Start / stop helpers ──────────────────────────────────────────────────── */

static bool startADCs() {
    for (auto& control : adcControl) control.rateLimited = limitFastRate1000;
    for (auto& control : adcControl)
        for (auto& count : control.counts) count = 0;

    if (mode == Mode::Sensor) {
        uint8_t sensor = testSensor % kNUM_SENSORS;
        uint8_t a = sensorAdc(sensor);
        auto& control = adcControl[a / 2];
        control.configs[a % 2][0] = {sensorChannel(sensor), 1, ADS1015::ConfigPGA::One};
        control.counts[a % 2] = 1;
    } else {
        bool raw = mode == Mode::All || mode == Mode::Raw;
        bool env = mode == Mode::All || mode == Mode::Env;
        for (int a = 0; a < 4; ++a) {
            auto& control = adcControl[a / 2];
            auto* cfg = control.configs[a % 2];
            uint8_t& n = control.counts[a % 2];
            if (raw) {
                cfg[n++] = {kRawCh[a][0], 1, ADS1015::ConfigPGA::One};
                cfg[n++] = {kRawCh[a][1], 1, ADS1015::ConfigPGA::One};
            }
            if (env) {
                uint8_t div = raw ? kSLOW_DIV : 1;
                cfg[n++] = {kEnvCh[a][0], div, ADS1015::ConfigPGA::One};
                cfg[n++] = {kEnvCh[a][1], div, ADS1015::ConfigPGA::One};
            }
        }
    }
    esp_err_t err = commandAdcWorkers(kAdcStart);
    if (err == ESP_OK) return true;
    commandAdcWorkers(kAdcStop);
    recording = false;
    stopCompanion(true);
    waitImuIdle();
    commandSdWriter(SdCommand::Close);
    adcOK = false;
    updateStatusLed();
    printf("#ERR:ADC_START:%d\n#STOP\n", (int)err);
    return false;
}

static void stopADCs() {
    esp_err_t err = commandAdcWorkers(kAdcStop);
    if (err != ESP_OK) {
        adcOK = false;
        printf("#ERR:ADC_STOP:%d\n", (int)err);
    }
}

// ─────────────────────────────────────────────
//  'S<n>' — pick which sEMG sensor (0-7) sensor-test mode reads. Safe to
//  send while a sensor test is already running: the ADCs are restarted on
//  the new sensor without a countdown, so you can sweep all eight
//  electrodes in one session. Ignored (but acknowledged) in other modes,
//  where it just arms the selection for the next '4'.
// ─────────────────────────────────────────────
static void printSensorLine() {
    uint8_t s = testSensor % kNUM_SENSORS;
    printf("#SENSOR:%u,%u,%u\n", (unsigned)s,
           (unsigned)(sensorAdc(s) + 1), (unsigned)sensorChannel(s));
}

static void selectTestSensor(uint8_t s) {
    testSensor = s % kNUM_SENSORS;
    printSensorLine();
    if (recording && mode == Mode::Sensor) {
        stopADCs();
        startADCs();
    }
}

// ─────────────────────────────────────────────
//  Reads the digit that must follow 'S'. Rejects loudly (#ERR:SENSOR)
//  rather than silently, so a malformed selection can't leave the host
//  believing a different sensor is being sampled than actually is.
// ─────────────────────────────────────────────
static void handleSensorCommand() {
    uint8_t sb;
    if (uart_read_bytes(UART_NUM_0, &sb, 1, pdMS_TO_TICKS(100)) <= 0) {
        printf("#ERR:SENSOR\n");   // 'S' arrived with no digit behind it
        return;
    }
    if (sb < '0' || sb > '7') {
        printf("#ERR:SENSOR\n");
        return;
    }
    selectTestSensor((uint8_t)(sb - '0'));
}

// ─────────────────────────────────────────────
//  Starts a recording (countdown + ADCs). Used from every place that
//  can start a test once the background tasks already exist: reed
//  switch, serial command 1-3, and the physical backup/OR button. The
//  caller must set `mode` before calling it. Returns false if the
//  countdown was aborted ('0' arrived during the wait).
// ─────────────────────────────────────────────
static bool startRecording() {
    printf("#MODE:%d\n", (int)mode);
    // Announce which sensor is live so the host doesn't have to infer it
    // from the CSV header (the selection persists across mode changes).
    if (mode == Mode::Sensor) printSensorLine();
    if (!countdown(3)) {
        updateStatusLed();
        printf("#STOP\n");
        return false;
    }
    // battery->enable5V();  // battery/5V management not used on this board (read on ESP32 #2 instead)
    recordingRate1000.store(limitFastRate1000, std::memory_order_relaxed);
    resetDropCounters();
    if (sdOK) commandSdWriter(SdCommand::Open);
    recStart.store(esp_timer_get_time(), std::memory_order_relaxed);
    recording = true;
    sendStartToSlave();
    saveMetadata(3, currentPhase, curGrasp, curRep, 0);
    if (!startADCs()) return false;
    updateStatusLed();
    printf("#REC\n");
    return true;
}

// ─────────────────────────────────────────────
//  Stops acquisition (ADCs + 5V) and updates the LED, without
//  notifying anything else. Used internally when only the mode
//  changes mid-recording (drain and close the old file set before restart).
// ─────────────────────────────────────────────
static void stopRecordingCore(bool notifySlave = false) {
    stopADCs();
    recording = false;
    waitImuIdle();
    // Notify the companion at acquisition end, before potentially slow SD I/O.
    stopCompanion(notifySlave);
    commandSdWriter(SdCommand::Close);
    printSampleCounts();
    // battery->disable5V();  // battery/5V management not used on this board (read on ESP32 #2 instead)
    updateStatusLed();
}

// ─────────────────────────────────────────────
//  Controlled end of test: stops the recording and notifies the
//  measurement ESP so it also closes its file. Used
//  both for the PC's '0' command and for the physical backup/OR
//  button (held 3s): if the PC/python crashes mid-test, the button
//  lets you close everything in a controlled way instead of
//  recording forever.
// ─────────────────────────────────────────────
static void stopTest() {
    stopRecordingCore(true);
    printf("#STOP\n");
}

// ─────────────────────────────────────────────
//  Physical backup/OR button (used to live on the measurement ESP,
//  moved to this bracelet). Interrupt-driven: a polled read can miss a
//  fast tap if the whole press+release cycle completes between two loop
//  iterations (confirmed happening via the earlier #BOTON_RAW/#STOPCAUSE
//  diagnostics -- fast taps were sometimes never seen at all, and the
//  50ms-blocking main loop occasionally caught a stale press right after
//  the 3s countdown, cancelling the just-started recording). A GPIO
//  interrupt on both edges guarantees every press/release is captured
//  with an accurate timestamp, regardless of what the main loop is doing
//  at that instant.
//    - Short press (released before kBotonHoldMs): starts the test if
//      not already recording -- useful for testing without the
//      python script or with grasps not yet defined.
//    - Held kBotonHoldMs (3s) or more while recording: stops the test
//      in a controlled way (stopTest, same effect as the PC's '0'
//      command) -- meant for when the PC/python crashes mid-test and
//      recording must not continue unchecked.
// ─────────────────────────────────────────────
static constexpr uint32_t kBotonHoldMs = 3000;
static constexpr int64_t  kBotonDebounceUs = 30000;
static int64_t botonUltimoEdgeAceptadoUs = 0;
static int64_t botonPresionadoDesdeUs = 0;
static bool    botonHoldDisparado     = false;
// Guard against the following bug: startRecording() calls countdown(3),
// which BLOCKS this whole loop for ~3 real seconds. If the user is still
// physically holding the button down when countdown() returns (very easy
// to do -- a "press and hold until it starts" is a natural gesture), the
// very next check would see presionado==true with botonPresionadoDesdeUs
// ==0 and treat it as a brand-new press, arming a fresh 3s hold-timer. If
// held a bit longer, that timer reaches kBotonHoldMs and immediately
// calls stopTest(), cancelling the recording that had just started. Fix:
// once a short press starts a recording, ignore the button entirely
// until it is physically released at least once, before re-arming the
// hold-timer.
static bool botonEsperandoSoltar = false;

// nivel: 0 = presionado (LOW, NormallyOpen w/ pull-up), 1 = suelto (HIGH)
struct BotonEdge { uint8_t nivel; int64_t tsUs; };
static QueueHandle_t botonEdgeQ = nullptr;

static void IRAM_ATTR botonIsr(void*) {
    BotonEdge e;
    e.nivel = gpio_get_level(kBOTON_INICIO);
    e.tsUs  = esp_timer_get_time();
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(botonEdgeQ, &e, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

// Configures GPIO8 for interrupt-on-any-edge and registers botonIsr().
// Called once from app_main(), replacing the earlier Switch-based
// polling for this specific button (reedSw keeps using Switch/
// isPressed() -- its gesture is a slow toggle, not at risk of a missed
// fast edge the way the start/stop button was).
static void botonIsrInit() {
    botonEdgeQ = xQueueCreate(16, sizeof(BotonEdge));

    gpio_config_t cfg = {};
    cfg.pin_bit_mask   = (1ULL << kBOTON_INICIO);
    cfg.mode           = GPIO_MODE_INPUT;
    cfg.pull_up_en     = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en   = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type      = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // The ADS1015 driver(s) already install the shared ISR service (see
    // the "gpio_install_isr_service already installed" lines in the
    // debug log) -- calling it again here is safe: ESP_ERR_INVALID_STATE
    // just means it's already up, anything else is a real error.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(kBOTON_INICIO, botonIsr, nullptr));
}

// Core gesture state machine, parameterized by (presionado, ahora)
// instead of reading the pin itself -- checkStartButton() below replays
// every edge the ISR captured (with its real timestamp) through this,
// plus one extra call with the live level/time to keep the hold-timer
// progressing between edges.
static void procesarEstadoBoton(bool presionado, int64_t ahora) {
    if (!presionado) {
        if (botonEsperandoSoltar) {
            // Diagnostic: marks the moment a real release is seen after
            // the short press that started the recording. Printed in
            // the same "us since recStart" units as the D, lines.
            printf("#BOTON_SOLTADO_REAL:%lu\n", (unsigned long)recordingTimestampUs());
        }
        botonEsperandoSoltar = false;
    } else if (botonEsperandoSoltar) {
        // Still holding from the press that just started a recording --
        // wait for a real release before tracking this as a new gesture.
        return;
    }

    if (presionado) {
        if (botonPresionadoDesdeUs == 0) {
            botonPresionadoDesdeUs = ahora;
            botonHoldDisparado = false;
        } else if (!botonHoldDisparado &&
                   (ahora - botonPresionadoDesdeUs) >= (int64_t)kBotonHoldMs * 1000) {
            botonHoldDisparado = true;
            if (recording) {
                printf("#STOPCAUSE:BOTON_HOLD\n");  // diagnostic: this stop came from the 3s button hold
                stopTest();
            }
        }
    } else {
        if (botonPresionadoDesdeUs != 0 && !botonHoldDisparado && !recording) {
            // Released before completing the hold -> short press
            mode = Mode::All;
            startRecording();
            botonEsperandoSoltar = true;  // ignore until physically released once
        }
        botonPresionadoDesdeUs = 0;
        botonHoldDisparado = false;
    }
}

static void checkStartButton() {
    // Replay every edge the ISR captured since the last call, in order,
    // with its real timestamp -- this is what guarantees a fast tap that
    // completed entirely between two loop iterations is never lost.
    BotonEdge e;
    while (xQueueReceive(botonEdgeQ, &e, 0) == pdTRUE) {
        if (e.tsUs - botonUltimoEdgeAceptadoUs < kBotonDebounceUs) {
            continue;
        }
        botonUltimoEdgeAceptadoUs = e.tsUs;
        printf("#BOTON_RAW:%d,%lld\n", e.nivel == 0 ? 1 : 0, (long long)e.tsUs);
        procesarEstadoBoton(e.nivel == 0, e.tsUs);
    }
    // Also evaluate the current live level/time, so a sustained hold
    // keeps progressing toward the 3s stop-gesture even when there's no
    // new edge to dequeue.
    procesarEstadoBoton(gpio_get_level(kBOTON_INICIO) == 0, esp_timer_get_time());
}

/* ── UART line buffer for multi-byte commands ──────────────────────────────── */

static char uartLineBuf[128];
static int  uartLinePos = 0;

static bool parseLabel(const char* text, uint16_t& grasp, uint16_t& repetition) {
    auto field = [&text](uint16_t& result) {
        if (*text < '0' || *text > '9') return false;
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + uint32_t(*text++ - '0');
            if (value > 65535) return false;
        }
        result = static_cast<uint16_t>(value);
        return true;
    };
    return field(grasp) && *text++ == ',' && field(repetition) && *text == '\0';
}

/** Process a complete line command (after '\n'). */
static void processUartLine(const char* line, int len) {
    if (len < 1) return;

    if (line[0] == 'R') {
        if (strcmp(line, "R?") == 0) {
            printf("#RATE:%s\n", limitFastRate1000 ? "1000" : "max");
        } else if (strcmp(line, "R1000") != 0 && strcmp(line, "Rmax") != 0) {
            printf("#ERR:RATE:USE_R1000_OR_Rmax\n");
        } else if (recording) {
            printf("#ERR:BUSY\n");
        } else {
            limitFastRate1000 = strcmp(line, "R1000") == 0;
            printf("#RATE:%s\n", limitFastRate1000 ? "1000" : "max");
        }
    } else if (line[0] == 'P') {
        if (strcmp(line, "P?") == 0) {
            printf("#PHASE:%s\n", phaseName(currentPhase));
            return;
        }
        SessionPhase next;
        if (strcmp(line, "Pgrasp") == 0) next = SessionPhase::Grasp;
        else if (strcmp(line, "Prest") == 0) next = SessionPhase::Rest;
        else if (strcmp(line, "Pdemo") == 0) next = SessionPhase::Demo;
        else { printf("#ERR:PHASE:USE_Pgrasp_Prest_OR_Pdemo\n"); return; }
        // Main is the only producer; the SD consumer can only free more space.
        if (recording && sdOK && (!labelQ || uxQueueSpacesAvailable(labelQ) == 0)) {
            printf("#ERR:METADATA_QUEUE\n"); return;
        }
        uint32_t timestamp = recordingTimestampUs();
        if (!companionPhase(next)) { printf("#ERR:LINK_QUEUE\n"); return; }
        if (!saveMetadata(2, next, curGrasp, curRep, timestamp)) return;
        currentPhase = next;
        printf("#PHASE:%s\n", phaseName(currentPhase));
    } else if (line[0] == 'L') {
        uint16_t gid = 0, rep = 0;
        if (!parseLabel(line + 1, gid, rep)) {
            printf("#ERR:LABEL\n"); return;
        }
        if (!saveMetadata(1, currentPhase, (uint16_t)gid, (uint16_t)rep,
                          recordingTimestampUs())) return;
        curGrasp = (uint16_t)gid;
        curRep = (uint16_t)rep;
        printf("#LABEL:%d,%d\n", gid, rep);
    } else if (line[0] == 'G') {
        // File transfer: G<path as emitted by the 'F' listing>
        // El cuerpo del archivo sale por uart_write_bytes, que no pasa por
        // hostPrintf: callados, el anfitrion recibiria los bytes sin la
        // cabecera #FDATA y no sabria que hacer con ellos. Se destapa solo.
        if (hostUartQuiet()) { hostSetUartQuiet(false); printf("#UART:1,transfer\n"); }
        if (!sdOK) { printf("#ERR:NO_SD\n"); return; }
        if (recording) { printf("#ERR:BUSY\n"); return; }
        std::string fpath(line + 1, len - 1);
        // Trim trailing whitespace
        while (!fpath.empty() && (fpath.back() == '\r' || fpath.back() == ' '))
            fpath.pop_back();

        // Resolve against the same base the 'F' listing walked, so a name
        // copied verbatim out of a '#F:' line round-trips. A bare name would
        // otherwise resolve against FatFs's *default* volume, which is not
        // necessarily the one the SD is mounted on (VOLUME_COUNT=2 here) —
        // listing succeeded while every open failed for exactly this reason.
        if (fpath.find(':') == std::string::npos) {
            std::string base = sdListRoot();
            if (!base.empty() && !fpath.empty() && fpath[0] == '/')
                fpath = base + fpath;
            else if (!base.empty())
                fpath = base + "/" + fpath;
        }

        // Only the main task handles UART commands, one transfer at a time.
        // FIL includes a 4096-byte sector cache with our FatFs configuration;
        // keeping it on the 3584-byte main task stack corrupts adjacent memory.
        static FIL tf;
        FRESULT fr = f_open(&tf, fpath.c_str(), FA_READ);
        if (fr != FR_OK) {
            // Report the FatFs code and the path actually attempted — a bare
            // "OPEN_FAIL" gave no way to tell a missing file from a bad path.
            printf("#ERR:OPEN_FAIL:%d,%s\n", (int)fr, fpath.c_str());
            return;
        }
        uint32_t sz = f_size(&tf);
        printf("#FDATA:%s,%u\n", fpath.c_str(), (unsigned)sz);

        static uint8_t xbuf[512];
        UINT br;
        while (f_read(&tf, xbuf, sizeof(xbuf), &br) == FR_OK && br > 0) {
            // Send raw binary (the Python side reads exactly sz bytes)
            uart_write_bytes(UART_NUM_0, xbuf, br);
        }
        f_close(&tf);
        printf("\n#FDONE\n");
    }
}

/** Feed one byte to the UART line accumulator. Returns single-char cmds. */
static int feedUartByte(uint8_t b) {
    // Multi-byte commands start with 'L', 'G', 'R', 'P' and end with '\n'
    if (uartLinePos > 0) {
        // We're accumulating a line
        if (b == '\n' || b == '\r') {
            uartLineBuf[uartLinePos] = '\0';
            processUartLine(uartLineBuf, uartLinePos);
            uartLinePos = 0;
            return 0;   // consumed
        }
        if (uartLinePos < (int)sizeof(uartLineBuf) - 1)
            uartLineBuf[uartLinePos++] = (char)b;
        return 0;   // consumed
    }

    // First byte of a potential command
    if (b == 'L' || b == 'G' || b == 'R' || b == 'P') {
        uartLineBuf[0] = (char)b;
        uartLinePos = 1;
        return 0;
    }

    // Single-byte commands
    return b;
}

/* ── SD directory listing helper ───────────────────────────────────────────── */

/** Base path the 'F' listing walks, and the base 'G' resolves names against.
 *  Keeping both on one definition is what makes a name copied out of a '#F:'
 *  line openable as-is. Includes the FatFs volume prefix (e.g. "0:"). */
static std::string sdListRoot() {
    size_t slash = sessionDir.find_last_of('/');
    return (slash == std::string::npos) ? sessionDir : sessionDir.substr(0, slash);
}

static void listSDDir(const char* path) {
    FF_DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK) {
        printf("#ERR:DIR\n");
        return;
    }
    printf("#FLIST:%s\n", path);
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fname[0] == '.') continue;
        bool isDir = (fno.fattrib & AM_DIR);
        printf("#F:%s%s,%u\n", fno.fname, isDir ? "/" : "",
               isDir ? 0u : (unsigned)fno.fsize);
        if (isDir) {
            // Recurse one level for session directories
            char sub[300];
            snprintf(sub, sizeof(sub), "%.*s/%.*s",
                     (int)(sizeof(sub)/2 - 2), path,
                     (int)(sizeof(sub)/2 - 2), fno.fname);
            FF_DIR sub_dir;
            FILINFO sub_fno;
            if (f_opendir(&sub_dir, sub) == FR_OK) {
                while (f_readdir(&sub_dir, &sub_fno) == FR_OK && sub_fno.fname[0] != '\0') {
                    if (sub_fno.fname[0] == '.') continue;
                    printf("#F:%s/%s,%u\n", fno.fname, sub_fno.fname,
                           (unsigned)sub_fno.fsize);
                }
                f_closedir(&sub_dir);
            }
        }
    }
    f_closedir(&dir);
    printf("#FEND\n");
}

/* ── app_main ──────────────────────────────────────────────────────────────── */


#if defined(EMG8_ADC_TIMING) || defined(EMG8_NET_DIAGNOSTICS)
// Bench-only routing test, before workers/ISR handlers start. Trigger one chip
// at a time and observe ALL ready inputs without relying on kRDY's ADC mapping.
static void probeReadyRouting() {
    auto levels = []() {
        uint8_t mask = 0;
        for (int pin = 0; pin < 4; ++pin)
            if (gpio_get_level(kRDY[pin])) mask |= 1U << pin;
        return mask;
    };
    auto writeWord = [](int a, uint16_t word) {
        uint8_t bytes[2] = {(uint8_t)(word >> 8), (uint8_t)word};
        I2C& bus = a < 2 ? i2c0 : i2c1;
        return bus.write(0x48 + (a & 1), 1, bytes, 2);
    };
    // Single-shot, AIN0, gain 1, 3300 SPS; OS=0 leaves an idle chip powered down.
    constexpr uint16_t idle = 0x43C3;  // comparator disabled
    constexpr uint16_t trigger = 0xC3C0;  // OS=1, conversion-ready enabled
    for (int a = 0; a < 4; ++a) {
        esp_err_t err = writeWord(a, idle);
        if (err != ESP_OK) { printf("#RDYPROBE:ERROR,%d,%d\n", a + 1, (int)err); return; }
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    for (int a = 0; a < 4; ++a) {
        I2C& bus = a < 2 ? i2c0 : i2c1;
        uint8_t addr = 0x48 + (a & 1);
        for (int rep = 0; rep < 3; ++rep) {
            uint8_t before = levels(), previous = before;
            uint32_t falling[4] = {};
            uint32_t start = (uint32_t)esp_timer_get_time();
            esp_err_t err = writeWord(a, trigger);
            uint32_t writeUs = (uint32_t)esp_timer_get_time() - start;
            while ((uint32_t)esp_timer_get_time() - start < 5000) {
                uint8_t now = levels();
                uint8_t edges = previous & ~now;
                for (int pin = 0; pin < 4; ++pin)
                    if ((edges & (1U << pin)) && !falling[pin])
                        falling[pin] = (uint32_t)esp_timer_get_time() - start;
                previous = now;
            }
            uint8_t cfg[2] = {}, result[2] = {};
            esp_err_t cfgErr = bus.read(addr, 1, cfg, 2);
            esp_err_t readErr = bus.read(addr, 0, result, 2);
            printf("#RDYPROBE:%d,%d,%d,%d,%d,%04X,%lu,%u,%u,%u,%lu,%lu,%lu,%lu\n",
                   a + 1, rep, (int)err, (int)cfgErr, (int)readErr,
                   (unsigned)((cfg[0] << 8) | cfg[1]), (unsigned long)writeUs,
                   before, previous, levels(), (unsigned long)falling[0],
                   (unsigned long)falling[1], (unsigned long)falling[2],
                   (unsigned long)falling[3]);
            writeWord(a, idle);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}
#endif

extern "C" void app_main() {

    /* ---- UART driver @ 460800 baud --------------------------------------- */
    const uart_config_t uart_cfg = {
        .baud_rate  = 460800,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags      = {},
    };
    uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);
    printBootDiagnostics();
    // Pull-up on RX: this line is never explicitly pinned (uses the
    // default UART0 pins), so when the PC/python is NOT connected (the
    // whole point of the physical backup/OR button) it's left floating.
    // A floating RX can pick up noise that occasionally decodes as a
    // valid byte -- including '0', which would silently call stopTest()
    // right after the button started a recording. The pull-up keeps the
    // line idle-HIGH (UART idle level) with nothing attached.
    gpio_set_pull_mode(GPIO_NUM_44, GPIO_PULLUP_ONLY);  // default ESP32-S3 U0RXD -- adjust if your board uses a different pin

    // UART1: toward the measurement ESP
    companionInit();
    netSetStoragePressureProbe(storagePressure);

    /* ---- Device MAC → hex string ----------------------------------------- */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    /* ---- RGB LED → blue while booting ------------------------------------ */
    led = new RGB(kLED);
    led->init();
    led->setColor(0, 0, 255);
    led->turnOn();

    /* ---- I2C ------------------------------------------------------------- */
    i2c0.init();
    i2c1.init();

    /* ---- ADCs ------------------------------------------------------------ */
    adc[0] = new ADS1015(i2c0, ADS1015::ADS111X_ADDR_GND);
    adc[1] = new ADS1015(i2c0, ADS1015::ADS111X_ADDR_VCC);
    adc[2] = new ADS1015(i2c1, ADS1015::ADS111X_ADDR_GND);
    adc[3] = new ADS1015(i2c1, ADS1015::ADS111X_ADDR_VCC);

    adcOK = true;
    for (int i = 0; i < 4; i++)
        if (adc[i]->checkForDevice() != ESP_OK) {
            printf("ADC%d not found\n", i + 1);
            adcOK = false;
        }

    // Configure ALERT/RDY pins + register callbacks + per-bus DRDY queues
    drdyQ[0] = xQueueCreate(64, sizeof(uint8_t));
    drdyQ[1] = xQueueCreate(64, sizeof(uint8_t));
    for (auto& control : adcControl) {
        control.done = xSemaphoreCreateBinary();
        if (!control.done) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    if (!drdyQ[0] || !drdyQ[1]) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    for (int i = 0; i < 4; i++) {
        adc[i]->configureAlertPin(kRDY[i]);
        // Suppress sub-two-clock input pulses before they become DRDY interrupts.
        // Filters live for the lifetime of these fixed acquisition GPIOs.
        gpio_pin_glitch_filter_config_t filterConfig = {};
        filterConfig.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
        filterConfig.gpio_num = kRDY[i];
        gpio_glitch_filter_handle_t filter = nullptr;
        ESP_ERROR_CHECK(gpio_new_pin_glitch_filter(&filterConfig, &filter));
        ESP_ERROR_CHECK(gpio_glitch_filter_enable(filter));
        adc[i]->onConversion(onSample, (void*)(uintptr_t)i);
        adc[i]->setEventQueue(drdyQ[i / 2], (uint8_t)i);
    }

#ifdef EMG8_ADC_TIMING
    probeBusClock(i2c0, kSCL0, 0);
    probeBusClock(i2c1, kSCL1, 1);
#endif
#if defined(EMG8_ADC_TIMING) || defined(EMG8_NET_DIAGNOSTICS)
    probeReadyRouting();
#endif

    /* ---- SD card --------------------------------------------------------- */
#ifndef EMG8_NO_SD
    spiSD = new SPI(SPI::SpiMode::kMaster, SPI2_HOST, kSD_MOSI, kSD_MISO, kSD_SCK);
    esp_err_t sdSpiErr = spiSD->init();
    if (sdSpiErr != ESP_OK) {
        printf("SD SPI bus init failed: %s\n", esp_err_to_name(sdSpiErr));
    } else {
        printf("SD SPI bus OK (MOSI=%d MISO=%d SCK=%d CS=%d)\n",
               kSD_MOSI, kSD_MISO, kSD_SCK, kSD_CS);
        sdCard = new SD(*spiSD, kSD_CS);
        esp_err_t sdErr = sdCard->init();
        if (sdErr != ESP_OK) {
            printf("SD card init failed: %s (0x%x)\n", esp_err_to_name(sdErr), sdErr);
        } else {
            FRESULT fr = sdCard->mountCard();
            if (fr != FR_OK) {
                printf("SD mount failed: FRESULT=%d\n", fr);
            } else {
                sdOK = true;
                printf("SD card mounted OK\n");
            }
        }
    }
    if (!sdOK)
        printf("SD card NOT available\n");

    // Uptime repeats after reboot. Never reuse an existing session directory.
    if (sdOK) {
        const std::string root = sdCard->getCurrentDir();
        const long long epoch = esp_timer_get_time() / 1000000;
        FRESULT mk = FR_EXIST;
        std::string candidate;
        for (unsigned attempt = 0; attempt < 1000 && mk == FR_EXIST; ++attempt) {
            char sdir[64];
            if (attempt == 0)
                snprintf(sdir, sizeof(sdir), "s_%s_%lld", macStr, epoch);
            else
                snprintf(sdir, sizeof(sdir), "s_%s_%lld_%u", macStr, epoch, attempt);
            candidate = root + "/" + sdir;
            mk = f_mkdir(candidate.c_str());
        }
        FRESULT cd = mk == FR_OK ? sdCard->goToDir(candidate) : mk;
        if (mk != FR_OK || cd != FR_OK) {
            sdOK = false;  // No fallback into the root or an old recording.
            printf("#ERR:SD_DIR:%d,%d,%s\n", (int)mk, (int)cd, candidate.c_str());
        } else {
            sessionDir = candidate;
            printf("#SDIR:%s\n", sessionDir.c_str());
        }
    }

#else
    printf("#BENCH:SD_DISABLED\n");
#endif

    /* ---- IMU (ICM-42605 over SPI3) --------------------------------------- */
    esp_log_level_set("ICM42605", ESP_LOG_DEBUG);
    spiIMU = new SPI(SPI::SpiMode::kMaster, SPI3_HOST, kIMU_MOSI, kIMU_MISO, kIMU_SCK);
    esp_err_t imuSpiErr = spiIMU->init();
    if (imuSpiErr != ESP_OK) {
        printf("IMU SPI bus init failed: %s\n", esp_err_to_name(imuSpiErr));
    } else {
        printf("IMU SPI bus OK (MOSI=%d MISO=%d SCK=%d CS=%d)\n",
               kIMU_MOSI, kIMU_MISO, kIMU_SCK, kIMU_CS);
        imu = new ICM42605(*spiIMU, kIMU_CS, 1000000);  // 1 MHz
        vTaskDelay(pdMS_TO_TICKS(50));  // Let SPI bus + IMU settle before init
        esp_err_t imuErr = imu->init(ICM42605::AccelScale::kAFS_4G,
                       ICM42605::GyroScale::kGFS_500DPS,
                       ICM42605::AccelODR::kAODR_200Hz,
                       ICM42605::GyroODR::kGODR_200Hz);
        if (imuErr == ESP_OK) {
            imuOK = true;
            printf("IMU OK (ICM-42605 SPI)\n");
        } else {
            printf("IMU init failed: %s (0x%x)\n", esp_err_to_name(imuErr), imuErr);
        }
    }

    /* ---- Reed switch ----------------------------------------------------- */
    reedSw = new Switch(kREED, Switch::SwitchMode::kNormallyOpen);
    reedSw->init();

    /* ---- Battery manager (5V charging board) ------------------------------ */
    // NOTE: battery/5V management is not used on this board -- it's read on
    // the other ESP32 (ESP32 #2) instead. `battery` stays nullptr; every
    // call site below is either commented out or already null-checked
    // (printStatusLine, sdOpenFiles, the 'V' UART command).
    /*battery = new BatteryManager(kBAT_ADC, kCHG, kPGOOD, k5V_EN);
    if (battery->init() != ESP_OK)
        printf("Battery manager init failed\n");
    else
        battery->measure();   // initial reading*/

    /* ---- Physical backup/OR button (moved from the measurement ESP) ------ */
    // Interrupt-driven (see botonIsrInit()/botonEdgeQ) instead of a
    // Switch/isPressed() poll -- a poll could miss a fast tap entirely.
    // Initialized AFTER the battery manager on purpose: see the note next
    // to kBOTON_INICIO's definition (shared GPIO8, digital config must win).
    botonIsrInit();

    /* ---- Sample queues --------------------------------------------------- */
    rawQ   = xQueueCreate(kRAW_QLEN,   sizeof(Sample));
    envQ   = xQueueCreate(kENV_QLEN,   sizeof(Sample));
    imuQ   = xQueueCreate(kIMU_QLEN,   sizeof(ImuSample));
    labelQ = xQueueCreate(kLABEL_QLEN, sizeof(LabelEvent));
    configASSERT(rawQ && envQ && imuQ && labelQ);
    imuRunMutex = xSemaphoreCreateMutex();
    configASSERT(imuRunMutex);
    if (sdOK) {
        sdCommands = xQueueCreate(1, sizeof(SdCommand));
        sdCommandDone = xSemaphoreCreateBinary();
        configASSERT(sdCommands && sdCommandDone);
        if (xTaskCreatePinnedToCore(sdWriteTask, "sd", 8192, nullptr, 5, nullptr, 1) != pdPASS)
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }


    /* ---- Status LED ------------------------------------------------------ */
    if (!adcOK || !sdOK) {
        printf("#INIT:ADC=%s,SD=%s,IMU=%s\n", adcOK ? "OK" : "FAIL",
               sdOK ? "OK" : "FAIL", imuOK ? "OK" : "FAIL");
    }
    updateStatusLed();

    /* ==== Wait for command ================================================ */
    /* Antes de #READY: el bucle de espera de modo tambien tiene que poder
     * recibir U1 por UDP, o el equipo queda mudo hasta que alguien grabe. */
    netSetCommandHandler(udpCommand);
    printf("#READY\n");
    printf("#MAC:%s\n", macStr);

    bool reedPrev = reedSw->isPressed();
    uint8_t rxByte;

    while (mode == Mode::Idle) {
        // Check UART. Timeout kept short (was 50ms) so this loop -- and the
        // button/reed checks below it -- run often enough to catch a fast
        // tap. At 50ms, a genuinely quick press-and-release could complete
        // entirely between two polls and never be seen at all.
        if (uart_read_bytes(UART_NUM_0, &rxByte, 1, pdMS_TO_TICKS(5)) > 0) {
            int cmd = feedUartByte(rxByte);
            if (cmd == '1') mode = Mode::All;
            else if (cmd == '2') mode = Mode::Raw;
            else if (cmd == '3') mode = Mode::Env;
            else if (cmd == '4') mode = Mode::Sensor;
            else if (cmd == 'S') handleSensorCommand();
            else if (cmd == '?') {
                printStatusLine();
            }
            else if (cmd == 'V') {
                // Read next byte for V0/V1
                uint8_t vb;
                if (uart_read_bytes(UART_NUM_0, &vb, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (vb == '1' && battery) { battery->enable5V(); printf("#5V:1\n"); }
                    else if (vb == '0' && battery) { battery->disable5V(); printf("#5V:0\n"); }
                }
            }
            else if (cmd == 'U') {
                // UART TX on/off (U0 = callar, U1 = volver). La recepcion
                // nunca se apaga, asi que U1 siempre llega, aunque sea a ciegas.
                uint8_t ub;
                if (uart_read_bytes(UART_NUM_0, &ub, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (ub == '0') { printf("#UART:0\n"); hostSetUartQuiet(true); }
                    else if (ub == '1') { hostSetUartQuiet(false); printf("#UART:1\n"); }
                }
            }
            else if (cmd == 'W') {
                // WiFi SoftAP + UDP streaming on/off (W1/W0)
                uint8_t wb;
                if (uart_read_bytes(UART_NUM_0, &wb, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (wb == '1') {
                        if (netStreamStart(macStr) == ESP_OK) printf("#WIFI:1\n");
                        else printf("#ERR:WIFI\n");
                    } else if (wb == '0') {
                        netStreamStop();
                        printf("#WIFI:0\n");
                    }
                }
            }
            else if (cmd == 'F') {
                if (sdOK) {
                    // Go up to root for listing
                    listSDDir(sdListRoot().c_str());
                }
            }
        }
        // Check reed switch (rising edge)
        bool reedNow = reedSw->isPressed();
        if (reedNow && !reedPrev)
            mode = Mode::All;
        reedPrev = reedNow;

        // Physical backup/OR button (tap only, hold doesn't matter here:
        // nothing is recording yet to stop). Drains the interrupt-
        // captured edge queue instead of polling isPressed(), so a fast
        // tap can't be missed even before the first recording starts.
        // Starts in mode All.
        BotonEdge be;
        while (xQueueReceive(botonEdgeQ, &be, 0) == pdTRUE) {
            if (be.nivel == 0) mode = Mode::All;   // 0 = presionado (LOW)
        }
    }

    /* ==== Start recording ================================================= */
    printf("#MODE:%d\n", (int)mode);
    if (!countdown(3)) {
        // Countdown aborted → go back to idle
        mode = Mode::Idle;
        printf("#STOP\n");
        // Fall through to main loop but not recording
    }

    if (mode != Mode::Idle) {
        // battery->enable5V();  // battery/5V management not used on this board (read on ESP32 #2 instead)
        recordingRate1000.store(limitFastRate1000, std::memory_order_relaxed);
        resetDropCounters();
        if (sdOK) commandSdWriter(SdCommand::Open);
        recStart.store(esp_timer_get_time(), std::memory_order_relaxed);
        recording = true;
        sendStartToSlave();
        saveMetadata(3, currentPhase, curGrasp, curRep, 0);
        updateStatusLed();
        printf("#REC\n");
    }

    xTaskCreatePinnedToCore(uartTask,     "uart", 4096, nullptr, 3, nullptr, 0);
    if (xTaskCreatePinnedToCore(adcBusTask,   "adc0", 4096, (void*)0, configMAX_PRIORITIES - 2, nullptr, 1) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    if (xTaskCreatePinnedToCore(adcBusTask,   "adc1", 4096, (void*)1, configMAX_PRIORITIES - 2, nullptr, 1) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
#ifndef EMG8_NO_IMU_TASK
    if (imuOK)
        xTaskCreatePinnedToCore(imuTask,  "imu",  4096, nullptr, 4, nullptr, 1);
#else
    // Compilar con -D EMG8_NO_IMU_TASK deja el IMU inicializado pero sin
    // muestrear. Sirve para separar el trafico de SPI3 (IMU) del de SPI2 (SD)
    // cuando se persigue un fallo en la ISR de SPI; no cambia nada del camino
    // de adquisicion de sEMG.
    printf("#DBG:IMU task disabled at build time\n");
#endif

    if (recording)
        startADCs();

    /* ==== Main loop: monitor reed + UART ================================== */
    constexpr uint32_t kDebounceMs = 300;
    uint32_t lastToggle = 0;
    uint32_t lastHealthMs = 0;

    while (true) {
        uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);

        if (nowMs - lastHealthMs >= 1000) {
            lastHealthMs = nowMs;
            printHealthLine();
        }

        // ---- Reed switch toggle (with debounce) ----
        bool reedNow = reedSw->isPressed();
        if (reedNow && !reedPrev && (nowMs - lastToggle > kDebounceMs)) {
            lastToggle = nowMs;
            if (recording) {
                stopRecordingCore();
                printf("#PAUSE\n");
            } else {
                startRecording();
            }
        }
        reedPrev = reedNow;

        // ---- Physical backup/OR button (moved from the measurement ESP) ----
        // Short press starts; held 3s stops in a controlled way.
        // See checkStartButton() for the gesture details.
        checkStartButton();

        // ---- UART commands ----
        // Timeout kept short (was 50ms, see note above the Idle-loop's
        // read) so checkStartButton() above gets called often enough to
        // catch a fast tap instead of missing it between polls.
        if (uart_read_bytes(UART_NUM_0, &rxByte, 1, pdMS_TO_TICKS(5)) > 0) {
            int cmd = feedUartByte(rxByte);
            if (cmd == 0) continue;   // consumed by line accumulator

            if (cmd == '0') {
                printf("#STOPCAUSE:UART0\n");  // diagnostic: this stop came from a '0' byte on UART0 (python, or noise if nothing is connected)
                stopTest();   // end of test -> the measurement ESP closes its file and goes back to waiting for the next start signal
            } else if (cmd == 'S') {
                handleSensorCommand();
            } else if (cmd >= '1' && cmd <= '4') {
                Mode newM = (Mode)(cmd - '0');
                if (newM != mode || !recording) {
                    if (recording) {
                        // Stop cleanly first (no trigger here: this is just
                        // a mode change, not the end of the test): the SD
                        // writer drains and closes the current file set, and
                        // an aborted countdown must not leave a
                        // half-recording state behind.
                        stopRecordingCore();
                    }
                    mode = newM;
                    startRecording();
                }
            } else if (cmd == '?') {
                printStatusLine();
            } else if (cmd == 'V') {
                uint8_t vb;
                if (uart_read_bytes(UART_NUM_0, &vb, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (vb == '1' && battery) { battery->enable5V(); printf("#5V:1\n"); }
                    else if (vb == '0' && battery) { battery->disable5V(); printf("#5V:0\n"); }
                }
            } else if (cmd == 'U') {
                // UART TX on/off (U0 = callar, U1 = volver). La recepcion
                // nunca se apaga, asi que U1 siempre llega, aunque sea a ciegas.
                uint8_t ub;
                if (uart_read_bytes(UART_NUM_0, &ub, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (ub == '0') { printf("#UART:0\n"); hostSetUartQuiet(true); }
                    else if (ub == '1') { hostSetUartQuiet(false); printf("#UART:1\n"); }
                }
            } else if (cmd == 'W') {
                uint8_t wb;
                if (uart_read_bytes(UART_NUM_0, &wb, 1, pdMS_TO_TICKS(100)) > 0) {
                    if (wb == '1') {
                        if (netStreamStart(macStr) == ESP_OK) printf("#WIFI:1\n");
                        else printf("#ERR:WIFI\n");
                    } else if (wb == '0') {
                        netStreamStop();
                        printf("#WIFI:0\n");
                    }
                }
            } else if (cmd == 'F') {
                if (sdOK) {
                    listSDDir(sdListRoot().c_str());
                }
            }
        }
    }
}
