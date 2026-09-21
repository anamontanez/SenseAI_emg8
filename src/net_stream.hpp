/*******************************************************************************
 * @file net_stream.hpp
 * @brief WiFi SoftAP + UDP full-rate sample streaming.
 *
 * The bracelet hosts its own network (SSID EMG8-<MAC>, WPA2). A client
 * subscribes by sending any UDP datagram to <bracelet-ip>:3333; the firmware
 * then streams binary packets to that endpoint until another client
 * subscribes or the radio is turned off.
 *
 * Packet layout (little-endian):
 *   offset 0  char[2]  magic "E8"
 *   offset 2  uint8    version (1)
 *   offset 3  uint8    type: 0 = raw Sample[], 1 = env Sample[], 2 = ImuSample[]
 *   offset 4  uint32   per-type sequence number (gaps ⇒ lost packets)
 *   offset 8  uint16   record count
 *   offset 10 uint16   reserved
 *   offset 12 records  count × (8 B Sample or 20 B ImuSample)
 *
 * A partial batch is normally flushed after 30 ms. Failed sends retain
 * the batch for a later pump; queue capacity still bounds the backlog.
 * Radio is off until netStreamStart() (UART 'W1').
 ******************************************************************************/
#pragma once

#include "emg8_types.hpp"
#include "esp_err.h"

/**
 * @brief Bring up SoftAP + UDP socket + sender task. Idempotent.
 * Start/stop calls are serialized by the host control task.
 */
esp_err_t netStreamStart(const char* macStr);

/**
 * @brief Stop streaming and turn the radio off. Prints #NET stats.
 * Waits for the sender to leave its current iteration before resource cleanup.
 * Do not call from the network command callback (it runs in that sender).
 */
void netStreamStop();

/** @brief True between netStreamStart() and netStreamStop(). */
bool netStreamActive();

/**
 * @brief UDP-only mode: silence everything the device writes to UART0.
 *
 * Receive stays enabled on purpose. `U1` has to work blind, so muting the
 * transmit side must never cost us the way back in.
 */
void hostSetUartQuiet(bool quiet);
bool hostUartQuiet(void);

/**
 * @brief Route datagrams arriving on the stream socket into a command parser.
 *
 * `pollSubscribe()` already reads and discards them to learn the client
 * address; handing the payload over is what gives UDP a return path.
 */
void netSetCommandHandler(void (*handler)(const char* data, int len));

/** @brief Non-blocking enqueue; silently counts drops when the net queue is full. */
void netEnqueueRaw(const Sample& s);
void netEnqueueEnv(const Sample& s);
void netEnqueueImu(const ImuSample& s);

uint32_t netPacketsSent();
uint32_t netDropCount();

/** Install before starting Wi-Fi. Sender yields while SD backlog is high. */
void netSetStoragePressureProbe(bool (*probe)());
