# Review for the next participant collection

Reviewed 28 September 2026 against bracelet baseline `a782687`, Saturday's
26 September participant tables, and today's saved SD/UDP captures. Only Ana's
bracelet fork is changed. Auxiliary firmware and monitor sources were read only.

## Decision and findings

Keep the validated storage-bench acquisition, SD queues, priorities, 1000 cap,
channel mapping, Wi-Fi settings and UDP v1 layout for the next collection.
Add participant traceability and reject ambiguous commands. A successful build
does not establish hardware acceptance.

| Priority | Finding | Action |
|---|---|---|
| P0 | The SD master contains labels, repetitions, phases and clock anchors, but no participant or host-session ID. Directory time is uptime, not calendar time. | New opt-in `J` identity command and checked/synced `Jnnn.json` before acquisition. Monitor handshake below. |
| P0 | Saturday's host sent only the initial movement label in most sessions when prepare time was zero. | Monitor fix is in its worklog. Verify each trial's `L`/`P` acknowledgement and SD events with the actual runner. Firmware cannot recover commands never received. |
| P0 | Today's SD copies matched acquisition counts and byte totals; the card previously had inconsistent FAT geometry and needed reformatting. | Preserve the SD safeguards. Use verified storage and read back representative recordings; short passes do not establish day-long card reliability. |
| P1 | Initial metadata enqueue failures could still reach `#REC`. | Start now aborts, closes files, retains the tag for retry, and latches a metadata fault. |
| P1 | Overlong commands were silently truncated; embedded NUL could conceal suffix bytes from string parsers. | Reject the whole line through CR/LF. Invalid idle `J` commands disarm old tags. |
| P1 | The local monitor parser reviewed here does not yet interpret the new SD result, receipt timestamp and stream sidebands. | Implement the monitor gates below. Legacy acknowledgements and auxiliary payloads stay unchanged. |

## UDP evidence and packet sizing

Today's two 60-second captures saved 479,609 / 479,692 raw records and matched
all reported ADC counts on SD. Every received UDP record existed on SD. The
host missed 6 raw + 1 envelope packets in one run, and 1 raw + 1 IMU in the
repeat. Both runs reported zero send errors, zero UDP queue drops, **zero
storage-pressure pauses and zero throttles**. Sampled UDP raw queue peaks
were only 76 and 65 entries out of 1,500.

The firmware's SD-pressure deferral therefore does not explain these particular
gaps. The evidence cannot distinguish radio loss after `sendto`, over-air loss,
or host network delivery. More queue RAM cannot recover packets already accepted
by the local network stack.

- Raw: 174 records x 8 bytes + 12-byte header = **1,404 UDP payload bytes**;
  1,432 bytes including normal IPv4/UDP headers, below a 1,500-byte MTU.
  At eight 1 kHz channels, a lost full packet removes about 21.75 ms of
  aggregate acquisition time. Nearly all received raw packets were full.
- Envelope: usually 40-48 records (332-396 bytes), around every 100-120 ms.
  IMU packet counts vary with its actual sampling rate. Their 100 ms partial
  flush is intentional batching, distinct from acquisition gaps.
- Nominal record payload is 71.2 kB/s at 1000 raw / 50 envelope / 200 IMU Hz.
  Larger packets offer little efficiency benefit and may cross the MTU;
  smaller packets increase packet rate. Neither is justified by these captures.
- Keep sequence gaps and original timestamps in the host data. Display
  interpolation must not be counted as measured samples.
- The stream has one subscriber: another host endpoint takes it over. Do not
  run a bench UDP receiver alongside the participant monitor.

Recomputed packet histograms, SD intervals and final diagnostics are saved in
`benchmarks/robustness-2026-09-28/review-metrics-2026-09-28.json` (local artifact).

## Sampling improvements requiring an A/B test

Saturday's raw intervals were ~0.913 ms, with a ~2.44-2.50 ms interval every
20 samples. Both envelopes share a due cycle, and the rate limiter waits at
that 20-cycle boundary. Staggering only the envelopes cannot give uniform 1 ms
sampling: at ~455 us/conversion, even one extra conversion gives ~1.37 ms.
Limiter placement also needs consideration, retaining genuine DRDY timestamps.

Today's SD raw rates were 998.42-999.93 Hz; per-channel 99th-percentile intervals
were 1.940-1.952 ms. Do not apply Saturday's exact distribution to today's
image or call the cap a guaranteed minimum of 1000 Hz. Open pins establish
timing only. A future change must compare raw/env counts, interval distributions,
SD completeness and channel attribution, followed by real-sensor spectra.

IMU at ~173-178 Hz in today's combined runs exceeds the agreed practical
100 Hz floor. FIFO/priority changes and beacon-noise mitigation need separate
hardware tests. Keep these out of the immediate participant build.

## Identity contract for the monitor agent

UART0, 460800 baud, newline-terminated. No auxiliary-controller effect, no UDP
change and no change to binary SD formats:

```text
PC -> JS023,sS023_n1_20260929_100000\n
ESP -> #IDENTITY:ARMED,S023,sS023_n1_20260929_100000
PC -> 1
... countdown ...
ESP -> #SD:OK,0:/s_<MAC>_<uptime>_<suffix>/000.bin,0
ESP -> #SESSION:S023,sS023_n1_20260929_100000,0:/s_<MAC>_<uptime>_<suffix>/000.bin
ESP -> #TSWRAP:0,0
ESP -> #REC
```

Subject length: 1-32 ASCII characters. Session length: 1-64. Allowed in both:
letters, digits, underscore, hyphen, period. Use participant codes, not names
or demographic details. Empty fields, extra commas, controls, Unicode and
excess length are rejected. Identifiers are JSON values, never path components.
The unique host session ID may include its date/time; firmware has no RTC.

- `J?\n` reports `#IDENTITY:ARMED,<subject>,<session>`,
  `#IDENTITY:REQUIRED,,`, or `#IDENTITY:OFF,,`. `?` includes the same line.
  This describes the next start; REQUIRED during recording is expected after
  consumption. The current recording's identity is the preceding `#SESSION`.
- A successful start consumes the tag. Another new recording, including
  mode-change/reed/button resume, requires a new explicit `J`. Otherwise
  `#ERR:IDENTITY_REQUIRED` and `#STOP` arrive before files or auxiliary start.
  Session-code uniqueness is the monitor's responsibility.
- Failed preflight, cancelled countdown and failed acquisition start retain
  the tag for retry. Partial files remain intact. Invalid idle `J` commands
  disarm any old selection and keep identity required.
- Changes/clears during countdown or recording return `#ERR:BUSY`; the current
  recording's identity is immutable. `J-\n` explicitly restores untagged/free
  operation. Do not send it automatically to bypass a participant start failure.
- Selection is volatile. Reboot starts OFF for legacy standalone compatibility;
  the monitor must arm and confirm every participant recording after reconnect.
  Old firmware will not acknowledge `#IDENTITY`; block the tagged workflow on
  a timeout rather than continuing without it.
- `#FIRMWARE:ELF_SHA256=<64 lowercase hex digits>` accompanies `?` and build
  configuration output. Persist it in the host session.

Each tagged file set gains `Jnnn.json`, opened with `FA_CREATE_NEW`, checked
for full write, synced and closed before countdown. Fields: schema
`emg8.identity.v1`, `state: prepared`, subject, session, exact file_set, firmware
ELF SHA256, mode, rate and countdown_seconds. The JSON is a **preflight identity
record**, not proof the recording started or finished. An aborted attempt may
leave it beside empty binaries. `#SESSION` binds a successful start;
`#SDSUM:...,OK` establishes the firmware's successful final binary close.

`#SDSUM` bytes continue to count only M/R/E/I binaries; JSON is excluded.
`F` lists the JSON and `G<listed-path>\n` downloads it. Untagged recordings
retain four files. No extra file handle stays open during acquisition.

## Monitor acceptance gates

1. Start host logging before commands. Arm identity, wait for its exact
   acknowledgement, and set the initial label and phase explicitly.
2. Require matching `#SESSION`, `#SD:OK`, `#REC`, and actual full-rate UDP on
   all eight raw channels before cues. `#STREAM` reports local submission,
   not delivery. Preserve the actual-arrival/rate gate and the validated
   lead-in for Ana's start-up sweep.
3. Stop trial cues on SD failure, unexpected stop/reset, metadata errors or
   stale link. A UDP gap flags the PC copy; it does not prove SD data loss.
4. At stop, collect `#SDSUM` (OK/INCOMPLETE), all four `#CNT`, and `#STOP`.
   Check acquisition error/retrigger counts and storage drops separately.
5. Drain the UDP tail before closing the host data file. `#STOP` guarantees
   SD drain/close, not UDP delivery. Partial env/IMU batches may remain for
   100 ms and the transports can arrive out of order. Use a bounded drain,
   flag late/missing packets and reconcile from SD; delay alone is not a
   delivery guarantee.
6. Persist participant/session-to-file mapping, firmware hash and QC result.
   `sd=1` alone is not saved-session proof. Parse `#EVENT`, `#AUXTS`, `#TSWRAP`
   as additions to unchanged legacy acknowledgements/payloads.

The monitor agent owns these changes; its repo was not edited here.

## Capacity and hardware acceptance

At Saturday's ~4.2-4.26 MB/min, 300 x 11.4-minute sessions produce ~14.5 GB
(13.5 GiB), before retries/calibration/headroom, and require **57 device-hours**.
The current card exposes roughly 15 GiB. Distribute storage/work by station
and day, and archive verified files before capacity becomes tight. Firmware
never formats, deletes or overwrites old recordings.

Before participants:

1. Record two dummy subjects; read back both JSON and binary sets, verify
   identity/counts/bytes and preservation of old files. Verify unarmed repeats
   are refused and cancelled starts can retry.
2. Run a full normal-duration monitor protocol with real sensors and auxiliary.
   Inspect labels/phases, SD result, UDP quality and physical LED visibility.
   Build/open-pin tests cannot establish analog or auxiliary correctness.
3. Confirm boot-without-SD refuses recording. Live removal/full-card injection
   belongs on disposable media, never participant data. Keep host and SD copies.
4. A >=75-minute continuous rollover run remains outstanding. Separate
   11-minute recordings reset their clock and do not accumulate into one wrap.
   The long-run host verifier still needs bounded-memory/epoch-aware comparison.

Prepared bench command (fresh output path, correct port and saved Wi-Fi profile):

```text
python tools/bench_sd.py --port COM5 --condition udp --rate 1000 --seconds 60 --identity BENCH_A,review_01 --wifi-profile EMG8-24EC4A368770 --output benchmarks/identity-review-01
```

The bench verifies the sidecar, firmware hash and UART association as well as
the existing SD/UDP comparison. It creates test files and preserves old files.
Hardware results for this change must be recorded separately from today's baseline.

## Build and offline validation

The 42-test baseline suite passed before edits. The updated suite plus identity
command checks cover 47 passing tests: subject/session bounds, stale-tag
disarming, busy/countdown rejection, one-start consumption, failed-start retry,
UART overflow/NUL rejection, injected identity-file open/write/sync/close
failures, and missing start-metadata refusal, alongside existing regressions.

`esp32-s3-storage-bench` built successfully: 72,576 bytes static RAM (264 more
than the LED baseline), 942,830 bytes flash. BIN SHA256:
`8F42D1F24BE253DDBFCBF82B5AD8F9BDCC8F3876BC299906336BFF8415E8780D`.
ELF SHA256 / expected `#FIRMWARE` value:
`8748a4630c008042d69f079054f2418a5abc1dd963878c638613f69020b3085e`.
No acquisition-loop, packet-pump, task-priority or queue-size changes.
This candidate has not been flashed or tested on hardware. No serial port was
opened during the review; COM5/COM11/COM12 were only enumerated.
