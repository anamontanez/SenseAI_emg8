# September participant-data review and robustness update

Evidence reviewed: `EMG8_article_analysis/firmware-upgrades.md`, README/WORKLOG,
and `data/s26/{sessions,timing_deep,timing,hold_overlap,wifi_lines}.csv`.
The tables contain six complete subject-coded sessions plus two interrupted
attempts. This describes the files, without resolving the operator's five-user
count. No participant data or analysis files were modified.

## What the evidence establishes

- Complete-session raw rates are usually 1000 Hz, envelope 50 Hz. One complete
  session has about 0.466% missing raw samples at the laptop. Host completeness
  cannot establish SD completeness, nor must UDP counts equal SD counts.
- Raw intervals cluster near 0.913 ms and 2.44-2.50 ms every 20 samples.
  Mean rate is not uniform sampling. IMU delivery is about 148-179 Hz in the
  completed sessions. Neither issue is repaired by changing an advertised rate.
- The old SD code never deletes or overwrites recordings, but permanently
  disables storage after an error, allows link-only recording, and creates no
  writer after a boot mount failure. The dim fault LED made this operational
  failure easy to miss. The participant logs do not identify which exact SD
  operation failed that day.
- The spectral pattern is consistent with SoftAP beacon interference. Its
  analog cause and the benefit of a different beacon setting require an A/B
  recording, not just a firmware build.

## Implemented on the bracelet

### B1: storage before recording

Normal builds refuse to start without a new, successfully opened and synced
master file plus all three data files. A failed mount at boot is recoverable:
the SD owner always exists, and a new start makes one recovery attempt. A stale
mount that fails opening gets one reinitialization and one new file-set attempt.
Partial files remain on the card. Each recovery creates a new unique directory;
all recording files still use `FA_CREATE_NEW`. There is no format/delete path.

The SD wrapper now initializes its handles, releases failed mount registrations,
and removes old SPI devices before reinitialization. Repeated attempts therefore
do not intentionally leak logical disks or device registrations. SDK init and
card command timeouts can exceed 500 ms; no 500 ms recovery bound or one-second
physical-removal detection bound is claimed without a hardware test.

The main task stops a live recording after SD I/O failure, sample-queue loss,
or metadata-queue loss. It notifies the auxiliary, drains what can be saved,
reports the result, and leaves the fault visible. Bright red (255,0,0), 250 ms
on / 250 ms off, overrides idle/connection/countdown states and is refreshed
while waiting for SD commands. A new successful start preflight clears it.
ADC failure is bright purple. The dim green/blue idle animation is retained.

Every start path now uses the same checks. The first boot start formerly had
separate logic and could announce `#REC` before verifying ADC start. U0 no
longer suppresses control/status/fault/save-result lines, only preview CSV and
driver logs. This is an intentional change for reliable failure reporting.

### B2/B3: timestamped receipt and command events

The current monitor unpacks exactly two label fields, uses the entire phase
suffix as the phase name, and matches auxiliary lines from their first character.
The proposed appended timestamp / `@...` prefix would break those parsers.
This update therefore adds sideband lines and retains legacy acknowledgements
and payloads byte-for-byte. No monitor changes are included.

| New UART line | Meaning |
|---|---|
| `#EVENT:LABEL,<grasp>,<rep>,<us>` | Accepted label and its bracelet command-processing time |
| `#EVENT:PHASE,<name>,<us>` | Accepted phase and its bracelet command-processing time |
| `#AUXTS:<us>` immediately followed by the original auxiliary line | Bracelet receipt time of that complete line |
| `#SD:OK,<file-set>,0` | Storage was prepared for this started recording |
| `#SD:FAIL,<reason>,<us>` | Storage/integrity failure; may repeat after final close |
| `#SDSUM:<file-set>,<raw>,<env>,<imu>,<bytes>,OK\|INCOMPLETE` | Final storage result, before `#STOP`/`#PAUSE` |
| `#SDSTATE:READY\|WRITING\|FAILED,<reason>` | Current storage state, not proof of a complete saved session |
| `#TSWRAP:<upper-u32>,<us>` | Recording timestamp epoch at start and each wrap |
| `#STREAM:UDP,<us>` | First successful local raw/envelope datagram send of this recording |
| `#IMURATE:TARGET=200,COUNT=<n>,ERRORS=<n>,MILLIHZ=<n>` | Actual successful IMU reads divided by elapsed recording time, not measured sensor ODR |

All new `<us>` fields are u64 elapsed microseconds since recording start.
Pre-start accepted label/phase events use zero; their state is captured by the
start snapshot in the master file. Existing phase/label SD events use the same
low timestamp word as the UART event. Queries still return legacy state lines
without pretending to be new phase changes.

Auxiliary receipt stamps are attached inside the bounded ring at line completion.
Forwarding remains at the existing one-second heartbeat on the low-priority
UART task. Timestamps include up to a normal 10 ms drain interval, serialization,
and all preceding auxiliary measurement work. They do **not** timestamp sensor
acquisition or the start of a multi-second sweep. No ≤1 ms alignment claim is
justified. Auxiliary data remain live UART relay; their SD copy stays on Ana's
board. Sensor validity and sweep-abort behavior need her firmware changes.

### B4: long recording clocks

Sample/IMU/UDP records retain their binary layouts and u32 timestamp fields.
Master header version remains 4; metadata extension byte 26 is now **2** and
byte 27 is **1**. Kind-4, 12-byte master records contain `low_time:u32,
high_time:u32, phase:u8, kind:u8, reserved:u16`. They appear at start, wrap,
and stop. Other kinds are unchanged. Kind 4 is never a movement label.

Reference decoder: `tools/sd_clock.py`. Unwrap each ADC/channel independently
and IMU separately, restarting per file set. Ordinary small backwards changes
are errors, not wraps; a large low-word fall is a wrap. This assumes an active
stream has no gap spanning a complete 71.6-minute clock cycle. The SD-failure
stop policy prevents recording through a long known storage outage. Master
anchors explicitly identify the high word and support sparse event decoding.
Offline tests reconstruct two hours and markers around rollover. A real
75-minute SD+UDP recording remains required.

### B9/B10: stream and periodic status

Starting a recording discards prior UDP queued/batched data through the sender
task before starting the new acquisition epoch. Packet sequences continue.
The sender reports its first successful raw/envelope `sendto`, then the main
task emits `#STREAM`. No notification is generated without a subscriber, after
a rejected send, or for IMU alone. This is **local submission**, not delivery
acknowledgement or proof of 1000 Hz reception. Keep the host's actual-arrival
and rate check before participant cues. UART delivery of this line may lag
the first UDP arrival; no ordering between the two transports is guaranteed.

The existing compact `#STATUS` plus SD state and actual IMU-read rate is emitted
every ten seconds while recording, even in U0. Battery remains unavailable on
this bracelet revision (existing zero fields); no battery-life claim is added.
IMU record padding is now initialized instead of transmitting stack contents.

## Deliberately not claimed complete

| Item | State / next validation |
|---|---|
| B5 beacon noise | Configurable build setting; `esp32-s3-quiet-beacon-bench` selects 1000 TU. Default keeps the validated 100 TU. Compare association, UDP continuity, and analog rest spectra before adopting it. |
| B6 raw regularity | Unchanged. Interleaving the two envelopes would reduce consecutive extra conversions, but the measured ~455 µs per conversion leaves a three-conversion interval near 1.37 ms. It cannot by itself promise <1.2 ms. Review scheduling and measured bus/trigger cost on the board. |
| B7 IMU timing | Actual rate/read errors now reported; acquisition remains polling. FIFO/data-ready with verified sensor-clock mapping requires driver and board validation. No synthetic timestamps, duplicate samples, or claimed 200 Hz fix. |
| B8 sweep during grasp | Cannot be guaranteed by the bracelet once Ana's board has started excitation. It needs the auxiliary's GRASP-abort/power-down path and the host lead-in. |
| A1-A5 | Auxiliary repository kept read-only as requested. |

## Hardware acceptance before participant deployment

Build-only work does not establish runtime throughput or SD recovery on this
card. With a backed-up test card: boot without card and confirm bright red and
start rejection; insert card and retry without reset; record/stop twice and
verify new file sets with no old file changed. In a disposable test recording,
remove/fill the card and verify stop, `INCOMPLETE`, visible red, then recovery
on a fresh start. Measure actual detection/recovery times rather than assuming
the proposed thresholds. Simulated short-write/sync/close failures are covered
offline, but deliberate card removal can damage the test filesystem.

Then compare every acquired ADC count to SD and every received UDP record to SD,
check auxiliary legacy parsing plus receipt stamps, exercise U0 and rapid
start/stop, and run ≥75 minutes to cross timestamp rollover. The existing
`bench_sd.py` and `bench_companion.py` understand the new metadata extension.
