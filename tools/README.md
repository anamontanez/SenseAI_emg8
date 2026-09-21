# Acquisition bench

Use `bench_acquisition.py` with the ADCs attached; analog sensor inputs may be
disconnected. It refuses to start if the device is recording or reports mounted
SD storage. It does not flash, reset, format, download, or modify the monitor app.
Do not use the old monitor `rate_matrix.py` to calculate current rates: its
channel-0-is-raw assumption predates this firmware's per-ADC channel map.

Example (from the firmware repository, using a Python environment with pyserial):

```text
python -B tools/test_bench_acquisition.py
python -B tools/bench_acquisition.py --condition udp --seconds 60 --wifi-profile EMG8-24EC4A368770 --output benchmarks/baseline-d4ca979/udp-01
```

The Windows Wi-Fi profile is optional and must already exist. The `8770` board
is the attached bench ESP32; the previously saved `8790` profile belongs to the
other bracelet. Ethernet should remain connected during Wi-Fi tests. Windows
may require elevated execution for `netsh wlan connect`.

Conditions are `off` (radio off), `nosub` (radio on without subscribing), `udp`,
and `quiet` (UDP with UART silenced during the measurement). Every run resets
the network session using W0/W1 as needed. UDP conditions reconnect the specified
Wi-Fi profile and require a subscription acknowledgment before acquisition.
The default mode is All; `--mode 2` and `--mode 3` exercise Raw and Env.

Each output directory must be new. Files are saved locally under the ignored
`benchmarks/` directory:

- `metadata.json`: configuration, channel map, UTC time, and timing convention.
- `serial.jsonl`: commands and non-CSV responses with host timestamps.
- `udp.bin`: complete datagrams, framed with `<QH` host nanoseconds and byte length.
- `summary.json`: every channel's counts, rates, timing quantiles, complete
  10-second reception windows, sequence losses, duplicate/reordered packets,
  I2C errors, retriggers, final status, and any failure.

`host_timed_acquired_hz` uses firmware CNT divided by the host's observed REC to
stop-command interval. It has small UART/start/stop timing uncertainty. UDP
`received_hz` uses device timestamps. It is reception throughput, not proof of
the acquisition rate if packets were lost. The delivery fraction compares all
received ADC records against all four ADC counters, including trailing batches
received after stop. The initial sequence number is a subscription boundary,
not evidence of packet loss. Out-of-order packets within the capture are kept;
duplicates are excluded from received counts.

The first firmware versions still enqueue SD samples when SD is unavailable.
Their raw/env/IMU storage-drop counters therefore rise even with perfect UDP
delivery. Preserve this evidence; do not interpret it as network loss. Later
SD-free firmware must eliminate that unused queue work.

Run each matrix condition eight times for 60 seconds, using a new output folder
per run, before making causal performance claims. A short or single run is only
a preliminary baseline. Keep the original build artifacts and full configuration
alongside the baseline, and identify the flashed build separately from the host
checkout: the existing protocol has no firmware build-ID query.

## Opt-in firmware diagnostics

Build `pio run -e esp32-s3-bench` for the attached SD-free bench. This environment
sets `EMG8_NO_SD` (skips SD initialization entirely) and `EMG8_ADC_TIMING`.
The normal environment keeps its existing SD behavior and omits timing code.
Unavailable SD storage is never enqueued to; the availability gate is shared
with the normal firmware.

After stop, #TIMING reports count, total/min/max microseconds, and eight histogram
bins with exclusive upper bounds 25, 50, 100, 200, 400, 800, 1600, infinity.
Metrics are config-write duration (trigger), ISR-to-service delay (wake),
conversion-register read duration (read), sample callback duration (publish),
config-write start to ready ISR (ready), and ready ISR to next-trigger call
(turnaround). These include preemption; ready includes the config transaction,
conversion, and interrupt latency. No timing text is emitted during acquisition.
Small timing instrumentation overhead is present and must be measured.

#ACQ reports each channel's count and first/last absolute 32-bit DRDY timestamps;
the harness calculates device acquisition rate independently of UDP delivery.
That span wraps after about 71 minutes, so keep diagnostic runs shorter than
one wrap. #ADC_EVENTS exposes event-queue overflow and stale pending events.
The harness stores these additions in summary.json; legacy firmware remains
supported. Diagnostics are suppressed if UART is still quiet at stop.

Diagnostic captures now require all four ADC event records, all 16 channel
records and all 24 complete timing histograms. Missing/truncated diagnostics
fail the capture and are listed in summary.json; they never imply zero events.
Original firmware without diagnostics remains supported. #ADC_EVENTS may append
early_ready and unasserted_ready counts after queue_drops and spurious.

The bench-only #SCLPROBE boot lines capture SCL with RMT at 20 MHz. Each symbol
is level0:duration0:level1:duration1, with durations in 50 ns ticks. RMT is
released before acquisition. Its own ISR affects the printed call duration;
use normal #TIMING records for software overhead.

Optional I2C driver comparison: build esp32-s3-legacy-bench. It inherits the
SD-disabled timing configuration and selects stack-backed legacy I2C transfers.
Normal esp32-s3-devkitc-1 and esp32-s3-bench continue to use i2c_master. Preserve
the exact binary and source diff for comparisons; see WORKLOG.md for measured
interrupt-placement variants. This experimental environment is not the normal
bracelet deployment target.

The esp32-s3-throughput-bench environment uses the same optional driver and
SD-free bench pin map but omits detailed per-conversion timing. It retains
physical ready validation/filtering and public stop counters. Use UDP timestamp
windows and counts for throughput; this build cannot report ADC_EVENTS/TIMING.
The optional driver now reads the completed conversion and writes the next
single-shot config within one command list, in that order. Its metric
exchange replaces read and covers both transfers. Trigger counts only
standalone initial/recovery writes. Ready starts at exchange entry (therefore
includes its read phase); turnaround ends at that entry. Do not compare these
last two directly with separate-transfer trigger boundaries. Wire speed,
channel order and gains are unchanged. Any exchange error suppresses publication
and waits a full recovery interval before re-arming the named next channel.

UDP sender ERR counts failed send attempts. With batch retries enabled, ERR
can increase while every acquired sample is eventually delivered. Use delivery
fraction, sequence gaps and queue DROP together; ERR alone is not lost data.
W0 intentionally discards pending data after the sender acknowledges shutdown.

## Companion and phase validation

Run `python tools/bench_companion.py --output benchmarks/<new-folder>` on an
idle board with mounted SD and exclusive COM9 access. It creates a recording,
exercises Pgrasp/Prest/Pdemo and labels (including while U0 mutes output),
measures radio-off/on CSV rates, verifies LINK counters, downloads only the
new files and checks exact SD metadata and acquired ADC counts. Existing
entries must retain their names and sizes. It restores demo/label0 and W0.
This validates bracelet scheduling/driver acceptance, not physical auxiliary
reception or cross-board synchronization accuracy.

`python -m unittest discover -s tools -p "test_*.py"` includes compiler-only
checks of the actual companion task, bounded label parser, and UDP task's
storage-priority branch. clang++ is required; no executable is emitted.
The UDP test checks that high SD backlog suppresses TX while subscriber
polling, quiet recovery, and shutdown acknowledgement remain functional.

When testing throughput, use the verified default storage environment for
this board and inspect CONFIG first. SDIO reports maximum write/sync-group
duration, including scheduling delay. A completed capture is only a storage
pass if every saved ADC count matches CNT and storage drops are zero.
A high acquisition rate or good UDP reception alone is insufficient.


Live network/heap investigation: pass --status-interval 2 to
bench_acquisition.py or bench_sd.py to record #NETDIAG/#MEM alongside
normal status. Zero (default) disables extra queries. A requested saved
Wi-Fi profile is retried up to five times after an AP restart; a stale
Windows scan may still require a fresh OS Wi-Fi scan before testing.
Timing counters and their limits: ../docs/streaming-diagnostics.md.
