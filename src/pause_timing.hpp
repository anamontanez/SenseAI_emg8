#pragma once
#include <cstdint>

// Single-owner elapsed-time accounting. Unsigned subtraction handles millis
// wrap; a single pause must be shorter than one full uint32_t clock period.
struct PauseTiming {
    bool active = false;
    uint32_t start = 0, count = 0, completedMs = 0, currentMs = 0, maxMs = 0;

    constexpr void update(bool paused, uint32_t now) {
        if (paused && !active) {
            active = true;
            start = now;
            ++count;
        }
        currentMs = active ? now - start : 0;
        if (currentMs > maxMs) maxMs = currentMs;
        if (active && !paused) {
            completedMs += currentMs;
            currentMs = 0;
            active = false;
        }
    }
};
