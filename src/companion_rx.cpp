#include "companion_rx.hpp"
#include "companion_rx_buffer.hpp"
#include "net_stream.hpp"
#include <cstdio>
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace {
CompanionRxBuffer buffer;
std::atomic<uint32_t> received{0}, forwarded{0}, rejected{0}, dropped{0}, muted{0};
std::atomic<uint32_t> wireErrors{0};
}

// UART event queue supplied by the existing companion owner.
QueueHandle_t companionRxEvents = nullptr;

void companionReceive() {
    uart_event_t event{};
    // Bound event work as well as byte work under a noisy/disconnected wire.
    for (int n = 0; n < 16 && xQueueReceive(companionRxEvents, &event, 0) == pdTRUE; ++n) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR ||
            event.type == UART_BREAK) {
            ++wireErrors;
            uart_flush_input(UART_NUM_1);
            buffer.losePartial();
        }
    }
    uint8_t bytes[128];
    for (int batch = 0; batch < 4; ++batch) {
        int count = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), 0);
        if (count <= 0) break;
        for (int i = 0; i < count; ++i) {
            switch (buffer.feed(bytes[i])) {
            case CompanionRxBuffer::Result::Accepted: ++received; break;
            case CompanionRxBuffer::Result::Rejected: ++rejected; break;
            case CompanionRxBuffer::Result::Full: ++dropped; break;
            default: break;
            }
        }
    }
}

void companionForward(bool live) {
    // Called only by uartTask, which also prints H/D lines. Never hold the
    // companion task waiting on the laptop. No allocations or large stack copy.
    static int64_t next = 0;
    const int64_t now = esp_timer_get_time();
    const bool enabled = live && !hostUartQuiet();
    if (enabled && now < next) return;
    next = enabled ? now + 1000000 : 0;
    const uint32_t limit = buffer.snapshot(); // new arrivals wait for next batch
    CompanionRxBuffer::View view{};
    while (buffer.peek(limit, view)) {
        if (enabled && !hostUartQuiet()) {
            // One stdio call holds its stream lock for the entire line, even
            // when the ring wraps. No raw UART writes mixed with other prints.
            int count = printf("%.*s%.*s\n", int(view.firstSize), view.first,
                               int(view.secondSize), view.second);
            if (count == int(view.firstSize + view.secondSize + 1)) ++forwarded;
            else ++dropped;
        } else ++muted;
        buffer.consume(view);
    }
}

void companionRxPrintStats() {
    if (hostUartQuiet()) return;
    printf("#AUX:RX=%lu,TX=%lu,BAD=%lu,DROP=%lu,MUTED=%lu,UARTERR=%lu\n",
           (unsigned long)received.load(), (unsigned long)forwarded.load(),
           (unsigned long)rejected.load(), (unsigned long)dropped.load(),
           (unsigned long)muted.load(), (unsigned long)wireErrors.load());
}
