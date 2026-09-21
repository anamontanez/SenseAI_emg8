# EMG8 Bracelet

8-channel surface EMG acquisition bracelet based on the **ESP32-S3-WROOM-1-N8**. Four ADS1015 ADCs sample raw EMG and envelope signals simultaneously using interrupt-driven mixed-rate single-shot mode, with binary SD logging, full-rate UDP streaming and UART CSV snapshots.

## Hardware

| Component | Part | Quantity |
|-----------|------|----------|
| MCU | ESP32-S3-WROOM-1-N8 | 1 |
| ADC | ADS1015 (12-bit, 3300 SPS) | 4 |
| IMU | ICM-42605 (6-axis) | 1 |
| Storage | MicroSD (SPI, FATFS) | 1 |
| LED | WS2812 RGB | 1 |
| Switch | Reed (normally open) | 1 |

## Pin Map

### I2C (1 MHz)

| Bus | SDA | SCL | Devices |
|-----|-----|-----|---------|
| I2C0 | GPIO6 | GPIO7 | ADC1 (0x48), ADC2 (0x49) |
| I2C1 | GPIO45 | GPIO47 | ADC3 (0x48), ADC4 (0x49) |

### ADC ALERT/RDY Interrupts

| ADC | Normal build GPIO | Attached bench GPIO |
|-----|-------------------|---------------------|
| ADC1 | GPIO40 | GPIO15 |
| ADC2 | GPIO41 | GPIO42 |
| ADC3 | GPIO42 | GPIO41 |
| ADC4 | GPIO15 | GPIO40 |

The measured bench routing differs from the original normal-build map.
Diagnostic and throughput bench builds use the measured column. The normal
map remains unchanged pending confirmation that this wiring matches the actual
bracelet; see [acquisition review](docs/acquisition-review.md).

### SD Card (SPI2)

| Signal | GPIO |
|--------|------|
| CS | GPIO10 |
| MOSI | GPIO11 |
| SCK | GPIO12 |
| MISO | GPIO13 |

### IMU — ICM-42605 (SPI3)

| Signal | GPIO |
|--------|------|
| MOSI | GPIO35 |
| SCK | GPIO36 |
| MISO | GPIO37 |
| CS | GPIO38 |
| FSYNC | GPIO39 |
| INT | GPIO16 |

### Peripherals

| Device | GPIO |
|--------|------|
| WS2812 RGB LED | GPIO2 |
| Reed Switch | GPIO9 |

## Channel Layout (per ADC)

The mapping differs between ADCs. These are the channel indices used by the
firmware and carried in each SD/UDP record:

| ADC | Raw EMG channels | Envelope channels |
|-----|------------------|-------------------|
| ADC1 | 0, 3 | 1, 2 |
| ADC2 | 1, 3 | 0, 2 |
| ADC3 | 0, 3 | 1, 2 |
| ADC4 | 1, 3 | 0, 2 |

With four ADCs this gives **eight raw channels and eight envelope channels**.
All mode inserts each envelope channel once per 20 raw cycles. Raw-only and
Envelope-only modes sample their respective channels at full speed.

Sampling uses **single-shot triggered round-robin**, not free-running continuous mode. In continuous mode the ADS1015 only applies a new MUX setting *after* the conversion already in progress finishes, so a config write lands either before or after that internal boundary depending on I2C timing — producing one stale conversion or none, unpredictably. No fixed "discard N" rule survives that race, and every miss shifts channel attribution by one until another miss shifts it back (observed on hardware as raw and envelope values swapping columns at random). Single-shot removes the race: nothing converts until the firmware asks, so each result provably belongs to the channel named in its own trigger. TI recommends single-shot whenever channels are swapped frequently.

Rates depend on the build, wiring and operating conditions — they are I2C-bound rather than purely conversion-bound. Use the per-channel counts in `#CNT` together with recording duration; UDP timestamps also expose sample intervals and reception rates. Counts alone do not measure Hz or prove lossless delivery.

## Firmware Architecture

The firmware uses a multi-task design on the ESP32-S3 dual-core processor:

```
Core 0                          Core 1
┌──────────────┐                ┌──────────────┐
│  app_main    │                │  sdWriteTask │  priority 5
│  (main loop) │                │  (batched    │
│              │                │   binary SD) │
├──────────────┤                ├──────────────┤
│  uartTask    │  priority 3    │  adc0 / adc1 │  priority MAX-2
│  (~50 Hz CSV)│                │  (per-bus    │
└──────────────┘                │   DRDY svc)  │
                                ├──────────────┤
                                │  imuTask     │  priority 4
                                └──────────────┘
```

- **ADC service tasks** (x2, core 1, one per I2C bus): each ALERT/RDY ISR timestamps a completion and posts its ADC index. The worker validates the ready event, reads the result, triggers the next single-shot conversion, then publishes the completed sample. It also owns start/stop and checks each ADC's recovery deadline even while its partner is active. Normal builds use separate `i2c_master` transfers; optional throughput builds combine read then trigger in a single legacy-driver command list.
- **UDP sender** (core 0, priority 5): drains bounded raw/envelope/IMU queues and batches version-1 datagrams. Failed sends retain their batch for a later retry. Radio shutdown waits for the sender to finish its current iteration before closing its resources.
- **SD writer** (core 1, priority 5): Drains the sample queues in batches (raw 500 × 8 B ≈ 4 KB). One file set (`R<nnn>.bin`, …) per recording start within the session directory.
- **UART CSV** (core 0, priority 3): Prints latest readings at ~50 Hz with auto-adjusted column headers per mode, reduced to ~1 Hz while the radio is active.
- **Main loop** (core 0): Monitors UART commands and reed switch for mode changes, start/stop, and pause/resume.

## Acquisition Modes

| Command | Mode | Channels | Description |
|---------|------|----------|-------------|
| `1` | All | All four per ADC | Raw EMG fast; envelope at 1/20, using the map above |
| `2` | Raw | Per-ADC raw pair above | Raw EMG only, no divider |
| `3` | Env | Per-ADC envelope pair above | Envelope only, no divider |
| `4` | Sensor | one | **Sensor test — currently NOT WORKING, see below** |
| `0` | Stop | — | Stop recording |

### Sensor Test Mode (`4`)

> **Sensor mode remains outside the current validation scope.** Earlier hardware tests reported no delivered samples and repeated stall recovery. The acquisition lifecycle has since changed, but Sensor mode has not been retested; use the verified All/Raw/Env modes until it is validated.

Intended behaviour, for whoever picks this up: only the ADC hosting the selected sensor runs, on a single channel, so the MUX never switches and that one sensor is sampled at the chip's full rate.

The 8 raw sEMG sensors are numbered **0–7**:

| Sensor | ADC | Channel | | Sensor | ADC | Channel |
|--------|-----|---------|-|--------|-----|---------|
| 0 | ADC1 | 0 | | 4 | ADC3 | 0 |
| 1 | ADC1 | 3 | | 5 | ADC3 | 3 |
| 2 | ADC2 | 1 | | 6 | ADC4 | 1 |
| 3 | ADC2 | 3 | | 7 | ADC4 | 3 |

Select with `S<n>` (`S0`–`S7`). Wire details:

- **Two raw bytes, no terminator** — `S` then the digit, like `V1`/`W1` (unlike `L`/`G`, which are newline-terminated). Send both in a single write: the digit must arrive within **100 ms** of the `S` or the command is rejected.
- A trailing `\n`/`\r` is harmless — it matches no command and is ignored.
- Accepted selections reply `#SENSOR:<n>,<adc>,<ch>` **once**, never repeated. `<adc>` is 1-based, `<ch>` is the raw ADS1015 channel index from the raw mapping above.
- A missing or out-of-range digit replies `#ERR:SENSOR`, so a malformed selection can't leave the host believing the wrong sensor is live.
- `#SENSOR:` is also emitted on entering the mode (right after `#MODE:4`), so the active sensor is always announced without inferring it from the header.
- `S<n>` may be sent **while a sensor test is already running** — the ADCs restart on the new sensor with no countdown, so you can sweep all eight electrodes in one continuous session. The CSV header is reprinted on each switch (see below). Sent in any other mode it just arms the selection for the next `4`.

The CSV stream carries a single EMG column in this mode, named `s<n>_adc<a>_<ch>`, e.g. `H,ts_us,s3_adc2_3,ax,...,label,rep`. Because that column name encodes the sensor, a live `S<n>` switch **reprints the `H` header** even though the mode hasn't changed — parse each `H` line rather than caching the first one. Samples still land in `R<nnn>.bin` tagged with their real ADC and channel indices, so recordings stay self-describing.

Modes can be switched at runtime via UART without rebooting. The reed switch toggles between pause and resume (defaults to All mode on first press).

## LED Status

| Color | Meaning |
|-------|---------|
| Blue | Booting / paused / stopped |
| Green | Recording |
| Red | Init error (ADC or SD failure) |

## UART Interface

The bracelet exposes a command-and-stream interface on `UART0`.

### Serial Settings

| Setting | Value |
|---------|-------|
| Baud rate | `460800` |
| Data bits | `8` |
| Parity | `None` |
| Stop bits | `1` |
| Flow control | `None` |
| Line ending for text commands | `\n` recommended |

The firmware emits a mix of:

- Metadata/status lines prefixed with `#`
- One CSV header line prefixed with `H`
- Repeated CSV data lines prefixed with `D`
- Raw binary payload bytes after a `G<path>` file-transfer command

### Host → Bracelet Commands

| Command | Example | Effect |
|---------|---------|--------|
| `1` | `1` | Start or switch to **All** mode |
| `2` | `2` | Start or switch to **Raw** mode |
| `3` | `3` | Start or switch to **Env** mode |
| `4` | `4` | Start or switch to **Sensor test** mode (one sensor at a time) |
| `S<n>` | `S3` | Select sEMG sensor `0`–`7` for sensor-test mode |
| `0` | `0` | Stop / pause acquisition |
| `R1000` / `Rmax` | `R1000\n` | Select average 1000 Hz ceiling or maximum speed while stopped |
| `R?` | `R?\n` | Query selected sampling rate |
| `?` | `?` | Query current status |
| `V1` | `V1` | Enable 5V rail |
| `V0` | `V0` | Disable 5V rail |
| `W1` | `W1` | Enable WiFi SoftAP + UDP streaming |
| `W0` | `W0` | Disable WiFi (prints `#NET` stats) |
| `U0` / `U1` | `U0` | Silence / restore UART output; command reception stays active |
| `Pgrasp` / `Prest` / `Pdemo` | `Pgrasp\n` | Set session phase and notify the companion |
| `P?` | `P?\n` | Query session phase |
| `L<id>,<rep>` | `L7,3` | Set current grasp label and repetition |
| `F` | `F` | List files on the SD card |
| `G<path>` | `Gs_AABBCCDDEEFF_1713012345/R000.bin` | Transfer one file as raw binary |

Command notes:

- `R1000`, `Rmax`, `R?`, `L<id>,<rep>` and `G<path>` are line commands. Send a terminating newline, for example `L7,3\n`.
- `1`, `2`, and `3` trigger the firmware countdown before acquisition starts.
- `G<path>` is rejected while recording is active and returns `#ERR:BUSY`.
- `F` and `G` require a mounted SD card. Otherwise the device returns `#ERR:NO_SD`.

### Sampling rate selection

Send `R1000\n` while stopped, then start normally with `1`, `2`, or `3`.
Send `Rmax\n` to restore the existing unrestricted scheduler. `max` is the
boot default; the setting lasts until reset. `R?\n` and the ordinary status
query report `#RATE:1000` or `#RATE:max`. Changing it during recording returns
`#ERR:BUSY`; stop first. The monitor app has not been modified.

The 1000 option paces ADC acquisition in groups of 20 fast-channel cycles
against 20 ms deadlines. In All mode this targets a 1000 Hz average ceiling
per raw channel and 50 Hz per envelope channel. Raw-only and Env-only modes
cap each active channel at 1000 Hz; Sensor mode caps its single channel.
It is not a uniform 1 ms sample clock: existing conversion jitter and envelope
insertion remain, and short windows can contain more than 1000 samples/s.
Use recorded timestamps. If hardware or I/O cannot sustain the target,
the rate is lower; no values are duplicated or interpolated.
After a long interruption the limiter discards accumulated catch-up credit.

The 32-byte v4 master header uses previously reserved byte 25 for the rate:
0 = max (including historical files), 1 = 1000 cap. Byte 26 announces the phase metadata extension (1); bytes 27-31 remain reserved.
UDP and sample record layouts are unchanged.

### Bracelet → Host Responses

| Prefix | Meaning |
|--------|---------|
| `#READY` | Firmware booted and is ready for commands |
| `#BOOT:reset=...,heap=...,minheap=...` | Reset cause and heap watermarks from the preceding boot |
| `#MAC:<hex>` | Device MAC used in session directory names |
| `#INIT:ADC=...,SD=...,IMU=...` | Peripheral init summary |
| `#MODE:<n>` | Current acquisition mode |
| `#CD:<n>` | Countdown tick before recording starts |
| `#CD:ABORT` | Countdown cancelled by sending `0` |
| `#REC` | Recording started or resumed |
| `#PAUSE` | Recording paused |
| `#STOP` | Recording stopped |
| `#CNT:<adc>,<c0>,<c1>,<c2>,<c3>,<i2c_err>,<retrig>` | Per-channel conversion counts for ADC `<adc>` during the recording that just stopped (4 lines, one per ADC), plus that ADC's failed-I2C-transaction count and stall recoveries. Fast channels of one ADC should match within ±1, envelope channels likewise, at the configured divider ratio. `<i2c_err>` and `<retrig>` should both be 0 or near-0 on healthy hardware — sustained nonzero values mean bus trouble. |
| `#LABEL:<id>,<rep>` | Label accepted |
| `#SENSOR:<n>,<adc>,<ch>` | Sensor-test selection accepted (and on entering mode `4`): sensor `<n>` is ADC `<adc>` (1-based) channel `<ch>`. Emitted once per event, never repeated |
| `#5V:0` / `#5V:1` | 5V rail state |
| `#WIFI:0` / `#WIFI:1` | WiFi radio + streaming state |
| `#NET:<ip>:<port>` | UDP client subscribed at this endpoint |
| `#NET:TX=<n>,ERR=<n>,DROP=<n>` | Streaming stats, printed on `W0` |
| `#STATUS:...` | Current status snapshot |
| `#HEALTH:<uptime_ms>,<free_heap>,<raw_q>,<env_q>,<imu_q>,<raw_drops>,<env_drops>,<imu_drops>` | Periodic 1 Hz runtime health snapshot |
| `#FLIST:<path>` | Start of SD file listing |
| `#F:<name>,<size>` | One file or directory entry |
| `#FEND` | End of SD file listing |
| `#FDATA:<path>,<bytes>` | File transfer header; raw bytes follow immediately |
| `#FDONE` | File transfer complete |
| `#ERR:<reason>` | Command rejected or failed |

### `#STATUS` Format

The current firmware replies to `?` with:

```text
#STATUS:<mode>,<recording>,<sd_ok>,<imu_ok>,<battery_mV>,<battery_pct>,<raw_drops>,<env_drops>,<imu_drops>
```

Field meanings:

| Field | Meaning |
|-------|---------|
| `mode` | `0=Idle`, `1=All`, `2=Raw`, `3=Env`, `4=Sensor test` |
| `recording` | `0` stopped/paused, `1` recording |
| `sd_ok` | `1` if SD storage is available |
| `imu_ok` | `1` if the IMU initialized correctly |
| `battery_mV` | Battery voltage in millivolts |
| `battery_pct` | Battery estimate in percent |
| `raw_drops` | Number of dropped raw EMG samples in the current run |
| `env_drops` | Number of dropped envelope samples in the current run |
| `imu_drops` | Number of dropped IMU samples in the current run |

### CSV Stream Format

When recording is active, the bracelet prints:

- One `H,...` header line at start of recording and again whenever the mode changes.
- Repeated `D,...` data lines at about 50 Hz, reduced to about 1 Hz while the radio is active.

Example header in **All** mode:

```text
H,ts_us,adc1_0,adc1_1,adc1_2,adc1_3,adc2_0,adc2_1,adc2_2,adc2_3,adc3_0,adc3_1,adc3_2,adc3_3,adc4_0,adc4_1,adc4_2,adc4_3,ax,ay,az,gx,gy,gz,label,rep
```

Example data line:

```text
D,123456,81,12,204,198,79,9,201,197,84,11,206,199,82,10,203,196,-0.031,0.004,0.998,0.2,-0.1,0.0,7,3
```

Notes for the Python datalogger:

- `D` lines are low-rate snapshots for monitoring, not the full EMG dataset.
- The high-rate dataset lives on the SD card in `R<nnn>.bin`, `E<nnn>.bin`, `I<nnn>.bin`, and `M<nnn>.bin`, where `<nnn>` is a zero-padded index that increments on every recording start within a session (so pause/resume never overwrites earlier data).
- After `#FDATA:<path>,<bytes>`, read exactly `<bytes>` raw bytes before parsing the trailing `#FDONE` line.
- During file transfer, treat the UART stream as binary, not line-oriented text.

### SD recording completion and errors

The writer opens a new file set before acquisition starts. Stop and pause
acknowledgements follow ADC stop, completion of any in-flight IMU measurement,
queue drain and file close. Once `#STOP` or `#PAUSE` arrives, files can be
downloaded immediately; no extra grace period is needed. Mode changes close
the previous file set before starting another. The companion stop notification
is sent at acquisition end, ahead of potentially slow SD flushing.

Writes must return both `FR_OK` and the full requested byte count. Write,
sync or close failures print `#ERR:SD_<operation>:<file>,<FatFs code>,<requested>,<written>`
and mark SD unavailable until reset. A code of zero with fewer bytes written
is still a failure (for example, a full card). Acquisition and UDP can continue.
The storage drop counters also include queued records discarded after failure
and records in a failed write batch whose persistence is uncertain; they are
not an exact count of missing bytes. Existing files are never reopened for overwrite.
Dirty files are synced at elapsed 500 ms intervals, and closed on stop.
Successful sync/close is not a guarantee against card-internal failure or power loss.

## WiFi / UDP Streaming

Off by default (radio adds 120–250 mA draw). Send `W1` over UART to enable, `W0` to disable.

- The bracelet hosts a WPA2 SoftAP: SSID `EMG8-<MAC>`, password `emg8sense`, bracelet IP `192.168.4.1`.
- Subscribe by sending **any** UDP datagram to `192.168.4.1:3333`; the firmware streams to the sender's address/port from then on. Re-send periodically if your viewer's port may change.
- The stream carries **everything at full rate** (raw + envelope + IMU; ~71 KB/s of record payload at 1000 Hz/raw in All mode). SD recording reliability requires separate validation with a working card.

**Packet format** (little-endian, ≤1404 bytes):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 | Magic `"E8"` |
| 2 | 1 | Version (1) |
| 3 | 1 | Type: 0 = raw `Sample[]`, 1 = envelope `Sample[]`, 2 = `ImuSample[]` |
| 4 | 4 | Per-type sequence number (gaps ⇒ lost packets) |
| 8 | 2 | Record count |
| 10 | 2 | Reserved |
| 12 | … | Records (8-byte `Sample` or 20-byte `ImuSample`, same layouts as SD) |

Partial batches normally flush after 30 ms. A rejected local send retains its batch and sequence for a later attempt; prolonged congestion can still overflow the bounded queues. `#NET` ERR counts failed send attempts and DROP counts queue overflow. Use reception counts and sequence gaps to assess delivered data. W0 intentionally discards pending data after the sender stops.

Minimal Python receiver:

```python
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"HI", ("192.168.4.1", 3333))          # subscribe
while True:
    pkt = s.recv(2048)
    magic, ver, ptype, seq, count = struct.unpack_from("<2sBBIH", pkt, 0)
    if magic != b"E8":
        continue
    if ptype in (0, 1):                          # raw / envelope
        for i in range(count):
            ts, adc, ch, val = struct.unpack_from("<IBBh", pkt, 12 + 8 * i)
```

## SD Binary Format

Each recording start within a session directory `s_<MAC>_<epoch>/` produces one file set: `M<nnn>.bin`, `R<nnn>.bin`, `E<nnn>.bin`, `I<nnn>.bin`. Only `M<nnn>.bin` has a header — `R`/`E`/`I` are pure record streams with no framing, so file size alone gives the record count.

**`M<nnn>.bin` — master file: 32-byte header, then a stream of 12-byte label events**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `"EMG8"` |
| 4 | 1 | Version (4) |
| 5 | 1 | Number of ADCs (4) |
| 6 | 1 | Channels per ADC (4) |
| 7 | 1 | Slow divider (20) |
| 8 | 4 | Epoch (seconds **since boot**, not wall-clock — the device has no RTC) |
| 12 | 2 | Battery voltage (mV) at recording start |
| 14 | 1 | Battery percentage at recording start |
| 15 | 1 | Battery state (see `BatteryManager::State`) |
| 16 | 2 | IMU output data rate (Hz) |
| 18 | 6 | Device MAC address |
| 24 | 1 | Mode (`1`=All, `2`=Raw, `3`=Env, `4`=Sensor test) |
| 25 | 1 | Rate selection: 0=max, 1=1000 Hz average cap |
| 26 | 1 | Metadata extension: 0=historical labels, 1=phase/event kind |
| 27 | 5 | Reserved |

Metadata event (12 bytes, recording-start snapshot and each label/phase command received while recording):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Timestamp (µs since recording start) |
| 4 | 2 | Grasp/movement ID |
| 6 | 2 | Repetition |
| 8 | 1 | Phase: 04=grasp, 05=rest, 06=demo (extension 1) |
| 9 | 1 | Event kind: 1=label, 2=phase, 3=start snapshot |
| 10 | 2 | Reserved |

**`R<nnn>.bin` / `E<nnn>.bin` — raw EMG / envelope: stream of 8-byte sample records, no header**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Timestamp (µs since recording start, captured in the ADC's DRDY ISR) |
| 4 | 1 | ADC index (0–3) |
| 5 | 1 | Channel index (0–3) |
| 6 | 2 | Value (signed 12-bit) |

**`I<nnn>.bin` — IMU: stream of 20-byte records, no header**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Timestamp (µs since recording start) |
| 4 | 2×3 | Accel X/Y/Z, milli-g (int16) |
| 10 | 2×3 | Gyro X/Y/Z, deci-dps (int16) |
| 16 | 2 | Temperature × 100 (int16) |
| 18 | 2 | Reserved |

These layouts are defined in [src/emg8_types.hpp](src/emg8_types.hpp) (`Sample`, `ImuSample`, `LabelEvent`) — the same structs are used for the UDP stream records (see above).

## Building

### Host-side serial capture

The repository includes [serial_capture.py](serial_capture.py) for repeatable
COM testing. It keeps DTR/RTS inactive so opening the CP210x port does not
accidentally reset the ESP32:

```bash
python serial_capture.py --seconds 50 --command 1 --stop-after 45 --metadata-only
```

Use `--command ?` for a status query, or omit `--command` to capture without
sending a command. The `--metadata-only` option suppresses high-rate `D` lines
while retaining headers, status, counters, and reset diagnostics.

### Requirements

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/) (6.x+)
- ESP-IDF framework (auto-installed by PlatformIO)

### Compile & Flash

```bash
pio run                                         # verified board: storage-bench
pio run -t upload --upload-port COM9             # flash that default configuration
pio device monitor -b 460800   # serial monitor (app UART runs at 460800, not the 115200 boot-log rate)
```

The custom [partitions.csv](partitions.csv) provides a 3 MB app partition on
the 8 MB flash. The default environment is now `esp32-s3-storage-bench`: real SD, combined
I2C transfers and the physically verified ready mapping 15/42/41/40.
The explicitly selected `esp32-s3-devkitc-1` environment retains the historical
40/41/42/15 wiring and separate I2C path; it does not match this attached board.
A status query prints `#CONFIG` with the actual I2C path and pin mapping.

### SD-free sampling benchmarks

| Environment | I2C path | Ready map | Detailed timing | SD |
|-------------|----------|-----------|-----------------|----|
| `esp32-s3-devkitc-1` | Separate, modern driver | Original | Off | Normal |
| `esp32-s3-bench` | Separate, modern driver | Measured bench | On | Disabled |
| `esp32-s3-legacy-bench` | Combined read then trigger | Measured bench | On | Disabled |
| `esp32-s3-throughput-bench` | Combined read then trigger | Measured bench | Off | Disabled |
| `esp32-s3-storage-bench` (default) | Combined read then trigger | Verified 15/42/41/40 | Boot routing probe | Enabled |

The combined path uses the ESP-IDF legacy I2C driver. It is now selected by
the default storage environment for this verified board. The other environments
remain explicit comparisons; match the physical ready wiring before use.
Build it with `pio run -e esp32-s3-throughput-bench`; wait for the whole build
to finish and verify the flashed image before measuring.

A 15-minute SD-free All-mode UDP run exceeded 1000 Hz per raw channel
(~1010 Hz), with envelopes at /20, IMU ~200 Hz and 99.9724% ADC delivery. This is average throughput:
mixed-rate scheduling produces nonuniform intervals. It does not establish
SD recording performance or analog signal quality with the floating inputs.
See [tools/README.md](tools/README.md) for captures and acceptance checks,
[WORKLOG.md](WORKLOG.md) for exact builds/results, and the
[acquisition review](docs/acquisition-review.md) for remaining work.

### Library Dependencies

The libraries below are **vendored directly into [lib/](lib/)** as plain files (not git submodules or PlatformIO registry packages), so the repository is self-contained and cloneable without access to the private Sense AI GitHub organization. Each still carries its own `README.md`/`CHANGELOG.md` documenting its own version history.

| Library | Path | Provides |
|---------|------|----------|
| sensors-library | [lib/sensors-library](lib/sensors-library) | ADS1015 driver (mixed-rate single-shot mode), ICM-42605 IMU, I2C/SPI wrappers, misc sensors |
| data-logging-library | [lib/data-logging-library](lib/data-logging-library) | SD card (FATFS) and flash storage |
| actuators-library | [lib/actuators-library](lib/actuators-library) | RGB LED (WS2812), reed switch |
| battery-library | [lib/battery-library](lib/battery-library) | Battery voltage/percentage/charge-state monitoring, 5V rail control |

## License

Sense-AI

## Companion synchronization and session phases

See [the firmware-defined contract](docs/companion-protocol.md) for the exact
monitor commands, companion UART bytes, SD metadata extension, and receiver
changes needed in Ana's repository. Default phase is demo; send Prest/Pgrasp
before starting a test, send transitions while recording, and Pdemo when
leaving the session. Phase persists across pauses and mode changes.

All companion UART writes now belong to a dedicated core-0 task (priority 6),
above UDP (5). ADC workers and SD writing remain on core 1. Sync frames use
an immutable recording epoch and are independent of CSV output and U0.
Use U1 for command acknowledgements; U0 still mutes all PC UART output.
One-way synchronization cannot establish precise cross-board alignment without
receiver fixes and measurements. The receiver/monitor repositories are unchanged.
