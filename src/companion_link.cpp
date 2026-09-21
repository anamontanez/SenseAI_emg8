#include "companion_link.hpp"
#include "net_stream.hpp"
#include <atomic>
#include <cstdio>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr uart_port_t kPort = UART_NUM_1;
constexpr int64_t kPeriodUs = 30000000;
enum class Kind : uint8_t { Start, Stop, Phase };
struct Event {
    Kind kind;
    SessionPhase phase;
    bool notify;
    int64_t epochUs;
};
QueueHandle_t events = nullptr;
std::atomic<uint32_t> starts{0}, stops{0}, phases{0}, syncs{0};
std::atomic<uint32_t> queueErrors{0}, txErrors{0};

bool writeFrame(const uint8_t* bytes, size_t size) {
    // No TX ring buffer: the only writer waits for room in the hardware FIFO.
    // Short frames and no flow control bound normal wire time to <1 ms.
    if (uart_write_bytes(kPort, bytes, size) == static_cast<int>(size)) return true;
    ++txErrors;
    return false;  // do not retry a possibly partial frame without framing recovery
}

void writeByte(uint8_t value) { writeFrame(&value, 1); }

void sendSync(int64_t epoch) {
    // Drain preceding start/phase bytes before capturing the outgoing timestamp.
    if (uart_wait_tx_done(kPort, pdMS_TO_TICKS(10)) != ESP_OK) {
        ++txErrors;
        return;
    }
    uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - epoch);
    uint8_t frame[5] = {0x02, static_cast<uint8_t>(elapsed),
        static_cast<uint8_t>(elapsed >> 8), static_cast<uint8_t>(elapsed >> 16),
        static_cast<uint8_t>(elapsed >> 24)};
    if (writeFrame(frame, sizeof(frame))) ++syncs;
}

void linkTask(void*) {
    // State belongs exclusively to this task, including the immutable start epoch.
    bool active = false;
    int64_t epoch = 0, deadline = 0;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (active) {
            int64_t remaining = deadline - esp_timer_get_time();
            wait = remaining <= 0 ? 0 : pdMS_TO_TICKS((remaining + 999) / 1000);
            if (remaining > 0 && wait == 0) wait = 1;
        }
        Event event{};
        if (xQueueReceive(events, &event, wait) == pdTRUE) {
            switch (event.kind) {
            case Kind::Start:
                epoch = event.epochUs;
                active = true;
                writeByte(0x03); ++starts;
                writeByte(static_cast<uint8_t>(event.phase)); ++phases;
                sendSync(epoch);
                // Current receiver resets/flushes after START. One early retry
                // reduces that compatibility gap; receiver ownership still needs fixing.
                deadline = esp_timer_get_time() + 1000000;
                break;
            case Kind::Stop:
                active = false;
                if (event.notify) { writeByte(0x01); ++stops; }
                break;
            case Kind::Phase:
                writeByte(static_cast<uint8_t>(event.phase)); ++phases;
                break;
            }
        }
        // Process queued boundaries before a due periodic sync; never mix epochs.
        if (active && uxQueueMessagesWaiting(events) == 0 &&
            esp_timer_get_time() >= deadline) {
            sendSync(epoch);
            deadline = esp_timer_get_time() + kPeriodUs;
        }
    }
}

bool enqueue(Event event) {
    // Control task only: bounded backpressure, never called by ADC/SD workers.
    if (events && xQueueSend(events, &event, pdMS_TO_TICKS(10)) == pdTRUE) return true;
    ++queueErrors;
    return false;
}
} // namespace

const char* phaseName(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Grasp: return "grasp";
    case SessionPhase::Rest: return "rest";
    default: return "demo";
    }
}

void companionInit() {
    uart_config_t cfg{};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kPort, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, GPIO_NUM_4, GPIO_NUM_5,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    events = xQueueCreate(32, sizeof(Event));
    if (!events || xTaskCreatePinnedToCore(linkTask, "companion", 3072, nullptr,
                                          6, nullptr, 0) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}

bool companionStart(int64_t epochUs, SessionPhase phase) {
    return enqueue({Kind::Start, phase, false, epochUs});
}
bool companionStop(bool notify) {
    return enqueue({Kind::Stop, SessionPhase::Demo, notify, 0});
}
bool companionPhase(SessionPhase phase) {
    return enqueue({Kind::Phase, phase, false, 0});
}

void companionPrintStats() {
    if (hostUartQuiet()) return;
    printf("#LINK:START=%lu,STOP=%lu,PHASE=%lu,SYNC=%lu,QERR=%lu,TXERR=%lu\n",
           (unsigned long)starts.load(), (unsigned long)stops.load(),
           (unsigned long)phases.load(), (unsigned long)syncs.load(),
           (unsigned long)queueErrors.load(), (unsigned long)txErrors.load());
}
