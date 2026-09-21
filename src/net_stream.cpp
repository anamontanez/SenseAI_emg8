#include <cerrno>
/*******************************************************************************
 * @file net_stream.cpp
 * @brief WiFi SoftAP + UDP full-rate sample streaming (see net_stream.hpp).
 ******************************************************************************/
#include "net_stream.hpp"

#include <atomic>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

namespace {

constexpr uint16_t kUdpPort   = 3333;
constexpr char     kApPass[]  = "emg8sense";  // WPA2, documented in README
constexpr uint8_t  kApChannel = 6;

constexpr uint8_t kTypeRaw = 0;
constexpr uint8_t kTypeEnv = 1;
constexpr uint8_t kTypeImu = 2;

constexpr int kHdrSize      = 12;
constexpr int kMaxPayload   = 1392;             // fits 1404-byte datagram < MTU
constexpr int kMaxRawRecs   = kMaxPayload / sizeof(Sample);     // 174
constexpr int kMaxImuRecs   = kMaxPayload / sizeof(ImuSample);  // 69
constexpr uint32_t kFlushMs = 30;               // partial-batch latency bound

// Queue sizes: sender drains every few ms, these only ride out WiFi hiccups
constexpr int kRawQLen = 1000;   // ~120 ms; reserve RAM for primary SD stream
constexpr int kEnvQLen = 256;
constexpr int kImuQLen = 100;

std::atomic<bool> active{false};
bool wifiInited   = false;   // one-time esp_netif/event/wifi init done
bool taskCreated  = false;
TaskHandle_t netTaskHandle = nullptr;
SemaphoreHandle_t stopped = nullptr;
std::atomic<bool> stopRequested{false};

QueueHandle_t rawQ = nullptr;
QueueHandle_t envQ = nullptr;
QueueHandle_t imuQ = nullptr;

int sock = -1;
sockaddr_in clientAddr = {};
bool clientKnown = false;
bool (*storagePressureProbe)() = nullptr;

std::atomic<uint32_t> txPackets{0};
std::atomic<uint32_t> dropCount{0};
uint32_t txErrors = 0;
#ifdef EMG8_NET_DIAGNOSTICS
struct SendErrorCount { int code = 0; uint32_t count = 0; };
SendErrorCount sendErrors[8];
void countSendError(int code) {
    for (unsigned i = 0; i < 7; ++i) {
        if (sendErrors[i].count == 0 || sendErrors[i].code == code) {
            sendErrors[i].code = code;
            ++sendErrors[i].count;
            return;
        }
    }
    sendErrors[7].code = -1;  // Other distinct codes, bounded storage.
    ++sendErrors[7].count;
}
#endif

/* Modo UDP-solo. Vive aqui porque este modulo es el que vuelve prescindible
 * al UART; nadie mas tiene por que saber como se apaga. */
std::atomic<bool> uartQuiet{false};
void (*cmdHandler)(const char*, int) = nullptr;
uint32_t lastClientMs = 0;

/* Si el anfitrion desaparece con el UART mudo el equipo queda incomunicado,
 * asi que el silencio caduca solo. */
constexpr uint32_t kQuietWatchdogMs = 20000;

/** Per-type packet assembly state. */
struct Batch {
    uint8_t  buf[kHdrSize + kMaxPayload];
    uint16_t count = 0;
    uint32_t seq = 0;
    uint32_t firstMs = 0;   // when the oldest queued record entered the batch
};
Batch batches[3];

void writeHeader(Batch& b, uint8_t type) {
    b.buf[0] = 'E';
    b.buf[1] = '8';
    b.buf[2] = 1;  // version
    b.buf[3] = type;
    memcpy(b.buf + 4, &b.seq, 4);
    memcpy(b.buf + 8, &b.count, 2);
    b.buf[10] = 0;
    b.buf[11] = 0;
}

bool sendBatch(Batch& b, uint8_t type, size_t recSize) {
    writeHeader(b, type);
    if (clientKnown) {
        int n = sendto(sock, b.buf, kHdrSize + b.count * recSize, 0,
                       (sockaddr*)&clientAddr, sizeof(clientAddr));
        if (n != kHdrSize + b.count * recSize) {
#ifdef EMG8_NET_DIAGNOSTICS
            countSendError(n < 0 ? errno : 0);  // Capture immediately, before logging.
#endif
            txErrors++;
            // Keep this batch and sequence for the next pump. Queue capacity
            // bounds the backlog; failed sends must not silently lose samples.
            return false;
        } else {
            txPackets.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Sequence advances even without a client so a late subscriber sees
    // an honest gap history rather than a fake zero-loss stream
    b.seq++;
    b.count = 0;
    return true;
}

/** Drain queue into the batch; send when full or older than kFlushMs. */
void pumpQueue(QueueHandle_t q, Batch& b, uint8_t type, size_t recSize,
               int maxRecs, uint32_t nowMs) {
    // A full batch may be waiting after a failed send. Retry it before
    // removing any more records from the bounded queue.
    if (b.count == maxRecs && !sendBatch(b, type, recSize)) return;
    while (b.count < maxRecs &&
           xQueueReceive(q, b.buf + kHdrSize + b.count * recSize, 0) == pdTRUE) {
        if (b.count == 0) b.firstMs = nowMs;
        b.count++;
        if (b.count == maxRecs && !sendBatch(b, type, recSize)) return;
    }
    if (b.count > 0 && (nowMs - b.firstMs) >= kFlushMs) {
        sendBatch(b, type, recSize);
    }
}

void pollSubscribe() {
    uint8_t rx[16];
    sockaddr_in src = {};
    socklen_t slen = sizeof(src);
    int n = recvfrom(sock, rx, sizeof(rx), MSG_DONTWAIT, (sockaddr*)&src, &slen);
    if (n >= 0) {
        bool changed = !clientKnown ||
                       src.sin_addr.s_addr != clientAddr.sin_addr.s_addr ||
                       src.sin_port != clientAddr.sin_port;
        clientAddr = src;
        clientKnown = true;
        lastClientMs = (uint32_t)(esp_timer_get_time() / 1000);
        if (changed && !hostUartQuiet()) {
            printf("#NET:%s:%u\n", inet_ntoa(src.sin_addr),
                   (unsigned)ntohs(src.sin_port));
        }
        /* Lo que no sea el "HI" de suscripcion es un comando. Esta es la via
         * de regreso que hace seguro apagar el UART. */
        if (n > 0 && cmdHandler && !(n == 2 && rx[0] == 'H' && rx[1] == 'I'))
            cmdHandler((const char*)rx, n);
    }
}

void netTask(void*) {
    while (true) {
        if (!active.load(std::memory_order_acquire) || sock < 0) {
            // Only acknowledge after the previous poll/pump iteration ended.
            // The control task may then close/reset the socket and batches.
            if (stopRequested.exchange(false, std::memory_order_acq_rel))
                xSemaphoreGive(stopped);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        pollSubscribe();

        /* Anfitrion callado y UART mudo: devolver la consola antes de que el
         * equipo quede mudo y sordo a la vez. */
        if (hostUartQuiet() && lastClientMs &&
            (uint32_t)(esp_timer_get_time() / 1000) - lastClientMs > kQuietWatchdogMs) {
            hostSetUartQuiet(false);
            printf("#UART:1,watchdog\n");
        }

        // Keep subscription/quiet recovery responsive, but stop adding Wi-Fi
        // work when primary storage needs to catch up. Queue overflow remains
        // explicitly counted by the existing enqueue paths.
        if (storagePressureProbe && storagePressureProbe()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
        pumpQueue(rawQ, batches[kTypeRaw], kTypeRaw, sizeof(Sample), kMaxRawRecs, nowMs);
        pumpQueue(envQ, batches[kTypeEnv], kTypeEnv, sizeof(Sample), kMaxRawRecs, nowMs);
        pumpQueue(imuQ, batches[kTypeImu], kTypeImu, sizeof(ImuSample), kMaxImuRecs, nowMs);

        // Raw fills a packet every ~19 ms in All mode; 5 ms keeps up with
        // margin while letting the task sleep most of the time
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t wifiInitOnce(const char* macStr) {
    if (wifiInited) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&initCfg);
    if (err != ESP_OK) return err;

    wifi_config_t apCfg = {};
    snprintf((char*)apCfg.ap.ssid, sizeof(apCfg.ap.ssid), "EMG8-%s", macStr);
    apCfg.ap.ssid_len = strlen((char*)apCfg.ap.ssid);
    strlcpy((char*)apCfg.ap.password, kApPass, sizeof(apCfg.ap.password));
    apCfg.ap.channel = kApChannel;
    apCfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apCfg.ap.max_connection = 2;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &apCfg);
    if (err != ESP_OK) return err;

    wifiInited = true;
    return ESP_OK;
}

}  // namespace

void hostSetUartQuiet(bool quiet) {
    uartQuiet.store(quiet, std::memory_order_relaxed);
    /* Los logs del propio ESP-IDF no pasan por printf, hay que callarlos
     * aparte o el UART sigue hablando. */
    esp_log_level_set("*", quiet ? ESP_LOG_NONE : ESP_LOG_INFO);
}

bool hostUartQuiet(void) { return uartQuiet.load(std::memory_order_relaxed); }

void netSetCommandHandler(void (*handler)(const char*, int)) { cmdHandler = handler; }

esp_err_t netStreamStart(const char* macStr) {
    if (active.load()) return ESP_OK;

    esp_err_t err = wifiInitOnce(macStr);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    if (rawQ == nullptr) rawQ = xQueueCreate(kRawQLen, sizeof(Sample));
    if (envQ == nullptr) envQ = xQueueCreate(kEnvQLen, sizeof(Sample));
    if (imuQ == nullptr) imuQ = xQueueCreate(kImuQLen, sizeof(ImuSample));
    if (stopped == nullptr) stopped = xSemaphoreCreateBinary();
    if (rawQ == nullptr || envQ == nullptr || imuQ == nullptr || stopped == nullptr) {
        esp_wifi_stop();
        return ESP_ERR_NO_MEM;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        esp_wifi_stop();
        return ESP_FAIL;
    }
    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(kUdpPort);
    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        close(sock);
        sock = -1;
        esp_wifi_stop();
        return ESP_FAIL;
    }

    txPackets.store(0);
    dropCount.store(0);
    txErrors = 0;
#ifdef EMG8_NET_DIAGNOSTICS
    for (auto& error : sendErrors) error = {};
#endif
    clientKnown = false;

    if (!taskCreated) {
        if (xTaskCreatePinnedToCore(netTask, "net", 4096, nullptr, 5, &netTaskHandle, 0) !=
            pdPASS) {
            close(sock);
            sock = -1;
            esp_wifi_stop();
            return ESP_ERR_NO_MEM;
        }
        taskCreated = true;
    }

    active.store(true, std::memory_order_release);
    xTaskNotifyGive(netTaskHandle);
    return ESP_OK;
}

void netStreamStop() {
    if (!active.load()) return;
    xSemaphoreTake(stopped, 0);  // discard any obsolete acknowledgment
    stopRequested.store(true, std::memory_order_release);
    active.store(false, std::memory_order_release);
    xTaskNotifyGive(netTaskHandle);
    xSemaphoreTake(stopped, portMAX_DELAY);

    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
    clientKnown = false;
    for (auto& b : batches) b.count = 0;
    // Discard anything still queued so a later session starts clean
    Sample s;
    ImuSample im;
    while (rawQ && xQueueReceive(rawQ, &s, 0) == pdTRUE) {}
    while (envQ && xQueueReceive(envQ, &s, 0) == pdTRUE) {}
    while (imuQ && xQueueReceive(imuQ, &im, 0) == pdTRUE) {}

    esp_wifi_stop();

#ifdef EMG8_NET_DIAGNOSTICS
    hostSetUartQuiet(false);
    for (const auto& error : sendErrors)
        if (error.count)
            printf("#NET_ERRNO:%d,%lu\n", error.code, (unsigned long)error.count);
#endif
    hostSetUartQuiet(false);   // sin radio no queda por donde hablar: el UART vuelve
    lastClientMs = 0;
    printf("#NET:TX=%lu,ERR=%lu,DROP=%lu\n",
           (unsigned long)txPackets.load(),
           (unsigned long)txErrors,
           (unsigned long)dropCount.load());
}

bool netStreamActive() {
    return active.load(std::memory_order_acquire);
}

void netEnqueueRaw(const Sample& s) {
    if (rawQ == nullptr || xQueueSend(rawQ, &s, 0) != pdTRUE)
        dropCount.fetch_add(1, std::memory_order_relaxed);
}

void netEnqueueEnv(const Sample& s) {
    if (envQ == nullptr || xQueueSend(envQ, &s, 0) != pdTRUE)
        dropCount.fetch_add(1, std::memory_order_relaxed);
}

void netEnqueueImu(const ImuSample& s) {
    if (imuQ == nullptr || xQueueSend(imuQ, &s, 0) != pdTRUE)
        dropCount.fetch_add(1, std::memory_order_relaxed);
}

uint32_t netPacketsSent() {
    return txPackets.load(std::memory_order_relaxed);
}

uint32_t netDropCount() {
    return dropCount.load(std::memory_order_relaxed);
}

void netSetStoragePressureProbe(bool (*probe)()) { storagePressureProbe = probe; }
