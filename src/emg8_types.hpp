/*******************************************************************************
 * @file emg8_types.hpp
 * @brief Shared sample/record types used by acquisition, SD logging and
 *        network streaming. Layouts are part of the on-disk (.bin) and
 *        on-wire (UDP) formats — do not change without bumping versions.
 ******************************************************************************/
#pragma once

#include <cstdint>

/** @brief One ADC conversion (also the SD R/E.bin and UDP record format). */
struct __attribute__((packed)) Sample {
    uint32_t ts;    // µs since recording start
    uint8_t  adc;   // 0–3
    uint8_t  ch;    // 0–3
    int16_t  val;   // signed 12-bit
};
static_assert(sizeof(Sample) == 8, "Sample must be 8 bytes");

/** @brief IMU sample for SD logging (20 bytes, raw int16 for compactness). */
struct __attribute__((packed)) ImuSample {
    uint32_t ts;       // µs since recording start
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    int16_t  temp100;  // temperature × 100
    uint16_t _pad;     // pad to 20 bytes
};
static_assert(sizeof(ImuSample) == 20, "ImuSample must be 20 bytes");

/** @brief Label/phase event written to master file (see docs/companion-protocol.md). */
struct __attribute__((packed)) LabelEvent {
    uint32_t ts;           // µs since recStart
    uint16_t grasp_id;     // Ninapro movement number (0 = rest)
    uint16_t repetition;   // current repetition
    uint32_t _reserved;    // extension 1: phase u8, event kind u8, zero u16
};
static_assert(sizeof(LabelEvent) == 12, "LabelEvent must be 12 bytes");
