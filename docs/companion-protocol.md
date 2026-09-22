# Companion synchronization and session commands

This file defines the bracelet side of the monitor/auxiliary-controller contract.
The monitor and auxiliary repositories are not modified by this change.

## PC UART0

460800 baud, 8N1. Send commands over UART; receive full acquisition data over
UDP. CSV `D,` lines are latest-value snapshots (~50 Hz with Wi-Fi off, ~1 Hz
with Wi-Fi enabled), not the acquisition sample rate. `U0` suppresses all
UART output, including acknowledgements, but leaves command reception active.
Use `U1` when acknowledgements are required; this does not disable UDP.

The session commands are newline terminated and case sensitive:

| Command | Meaning | Acknowledgement | Companion byte |
|---|---|---|---|
| `Pgrasp\n` | Active session, grasp begins | `#PHASE:grasp` | `04` |
| `Prest\n` | Active session, rest begins | `#PHASE:rest` | `05` |
| `Pdemo\n` | Free recording outside a test session | `#PHASE:demo` | `06` |
| `Psweep\n` | Request an auxiliary impedance sweep during recording; phase is unchanged | `#SWEEP:queued` | `05` |
| `P?\n` | Query current phase | `#PHASE:<name>` | none |
| `L<id>,<repetition>\n` | Set movement label and repetition | `#LABEL:<id>,<repetition>` | none |

Labels and phases are independent: rest must not erase the movement being
tested, and a label does not imply a grasp/rest transition. Set the label and
phase before starting with `1`–`4`; then send phase changes during recording.
Boot defaults to demo. Phase persists across pause, stop and mode changes;
send `Pdemo` when leaving a session, before any subsequent free recording.
Repeated phase commands resend the byte (there is no companion acknowledgement).
Host acknowledgement means accepted locally, not confirmed by the auxiliary board.
`Psweep` is accepted only while recording and deliberately does not create a
phase metadata event. The current auxiliary firmware ignores `05` while idle,
so a reference sweep before the bracelet starts requires a corresponding
auxiliary-firmware change.

## Companion UART1

Bracelet TX GPIO4 to auxiliary RX GPIO3, auxiliary TX GPIO6 to bracelet RX
GPIO5, common ground,
115200 baud, 8N1. One dedicated task on core 0 owns every write, with priority
4, below the UDP sender (5). UART1 has a 1024-byte RX ring and 512-byte driver
TX ring. ADC workers and SD writer remain on core 1.
No companion UART I/O runs inside an ADC callback or the SD writer.

Existing wire framing is retained: `01` = end test, `03` = recording start,
`02` followed by **exactly four little-endian bytes** = elapsed microseconds
since the bracelet's current recording epoch. Payload bytes may equal any
command byte; consume the entire payload before interpreting another command.
Elapsed time wraps at 2^32 microseconds (~71.6 minutes), as do sample timestamps.

Start sends `03`, the selected phase, then an initial synchronization frame.
A follow-up synchronization at one second helps recover from the current
receiver's start-time flush; subsequent frames are 30 seconds apart. This
follow-up is not a substitute for fixing receiver ownership. Pauses and mode
boundaries cancel the old synchronization schedule. Explicit end-test sends
`01`; it is queued before SD draining. A new start captures a new immutable
epoch in the ordered control queue. Queue/transmit errors are counted and
reported; they must not stall acquisition or SD recording.

## Historical receiver findings for Ana

Read-only review of `src/main_medicion.cpp` (SHA256
`b9f6b23f93df3637f7d54ea387aa59c529b535367a4a11d3c78642b84d43c367`)
found these issues at that historical revision:

- The RX task applies sync relative to `t_inicio_prueba_us`, but the main loop
  later resets that epoch and `g_clockOffsetUs`, then flushes UART after opening
  the SD file. A correctly received first sync can be overwritten or discarded.
  Establish the epoch on receiving `03` in the same owner as the sync parser;
  notify the measurement task without resetting/flushing that parser afterward.
- `volatile int64_t` is not cross-task synchronization on a 32-bit processor.
  Publish the epoch/offset together under a short lock or an atomic snapshot.
- The RX handler reads at most 32 bytes for each event and ignores UART overflow
  and frame errors. Drain available bytes and reset incomplete framing on errors.
- That revision ignored bytes `04`/`05`/`06`. Ana has since implemented
  them (reviewed at `973534b`); this item is superseded.
- One-way software timestamps include UART serialization (~434 microseconds
  for five bytes at 115200 baud) and RX scheduling latency. Offset replacement
  can make corrected time jump backward; it also misinterprets 32-bit wrap.
  Record local timestamps plus synchronization observations for reconstruction,
  or implement a monotonic clock correction with explicit wrap handling.
- A new bracelet recording/mode change resets its epoch. The receiver must
  process every `03` as an epoch boundary, including while already measuring.

No end-to-end synchronization accuracy is claimed without testing both boards
and a common timing reference. These findings do not establish the cause of
the bracelet's measured sampling slowdown.

## SD metadata extension

Sample/IMU records and UDP framing are unchanged. The 32-byte master header
remains v4; byte 26 = 1 announces this metadata extension (0 = historical).
The 12-byte label/event layout is unchanged. Formerly reserved byte 8 stores
phase (04 grasp, 05 rest, 06 demo); byte 9 stores event kind (1 label command,
2 phase command, 3 recording-start snapshot). Bytes 10-11 remain zero.
Every event contains current movement ID and repetition. The initial snapshot
has timestamp zero, so labels/phases set before recording are retained.

Older readers can still stride these records but will see repeated labels for
phase events; phase-aware readers must inspect the extension and event kind.
An exhausted metadata queue rejects the command with ERR:METADATA_QUEUE instead
of acknowledging an unsaved transition. Invalid labels return ERR:LABEL.
LINK counters from the status query are cumulative since boot; START/STOP/PHASE
count transmit attempts, SYNC counts frames accepted by the UART driver, and
QERR/TXERR count queue/transport failures. They do not confirm remote reception.

## Acquisition configuration and storage precedence

The attached board's isolated conversion probe confirms ADC1-4 ready pins
15/42/41/40. This checkout defaults to esp32-s3-storage-bench (real SD, combined
I2C transactions). Explicit esp32-s3-devkitc-1 selects the historical pin map
40/41/42/15 and is unsuitable for this attached board. Query CONFIG to identify
the flashed path and mapping.

SD raw buffering is 12000 records (~1.45 seconds at the measured max rate),
with smaller UDP queues to reserve RAM for storage. When SD's raw backlog
reaches 3000 records, the bounded UDP sender slows its passes from 5 to 10 ms.
At 9000 records (75% capacity) it defers transmission until below that threshold;
subscription and quiet-mode recovery remain responsive. Network queue overflow
is counted and may increase during prolonged deferral. Sampling and
SD enqueue do not wait for networking. This expresses the requested priority;
a card stall longer than finite buffer capacity can still lose samples.

SDIO status reports the longest write and sync group durations observed in the
current recording, including time preempted by other work. These counters are
diagnostics, not a guarantee of card durability or future maximum latency.

When storage queues are low, the writer blocks for one scheduler tick between
productive passes so it cannot monopolize core 1 with tiny writes and starve
the IMU. It drains continuously while backlog is high and while closing.

Host file-download implementations should provide sufficient serial RX
buffering. A multi-megabyte 460800-baud download on Windows lost 384 bytes with
the small default host buffer during this review. The test tool requests a
1 MiB RX buffer and verifies downloaded records/counts; the monitor is unchanged.
The existing G protocol has no CRC/retransmission, so this is not a guarantee
against all transport corruption.


### Final validation limits (2026-09-21)

The final image saved every acquired ADC sample in a 60-second SD+UDP max
run (~1021 Hz/raw channel, ~198 Hz IMU). A subsequent 35-second 1000 Hz
run reported 200 raw SD queue drops and ~146 Hz IMU, and its UART file
download timed out with missing #FDONE even with the 1 MiB RX buffer.
Storage under combined load and bulk UART downloads remain unresolved.
Do not treat successful short runs as a lossless-recording guarantee.
Final-image phase hardware retesting was not completed; the earlier
phase test and 24 host tests passed. Auxiliary receipt/clock accuracy
remain unverified. Detailed counts and artifacts are in WORKLOG.md.

## Auxiliary measurements forwarded to the monitor (2026-09-21)

Reviewed Ana's committed diff `585cc5d..973534b` in
`Electromyographic-interaction-variables/src/main_medicion.cpp`. Her UART2
now sends ASCII, LF-terminated messages at 115200 baud:

```text
imp:[1000.0,1234.56;2000.0,1200.12]
p1:1.25,p2:2.50,temp:30.75
```

Each impedance pair is frequency in Hz and magnitude in ohms. Pressure is
kPa and temperature is degrees Celsius. The bracelet forwards the complete
line with the same spelling, numeric text and point order to PC UART0 at
460800 baud. These are separate lines; H/D CSV columns and UDP packets do
not change. The monitor must recognize `imp:[` and `p1:` before its CSV
handling. Keep UART reception connected and use `U1`, including with UDP on.

While recording, pending auxiliary lines are emitted in batches at most
once per second, independent of the EMG CSV divider. Lines are sent once;
there is no repeated stale snapshot. Ana sends pressure/temperature every
2000 ms during grasp, and a sweep on start/rest; 1 Hz forwarding does not
create new measurements. No auxiliary timestamps, labels, sequence numbers,
or checksums exist in this wire format. Laptop arrival time is NOT the
sensor acquisition timestamp and cannot establish clock alignment.
This change relays live values only; it does not add auxiliary records
to bracelet SD files. Ana's controller retains its own SD logging.

Core-0 companion task (priority 4) retains sole ownership of UART1 TX/sync
and now drains RX nonblocking every <=10 ms under normal scheduling, with
a 512-byte driver TX ring, a 512-byte work budget per pass, and a 1024-byte
driver RX buffer. Commands
still wake it immediately; RX processing follows due commands/syncs.
A 4096-byte single-producer/single-consumer buffer publishes only complete
recognized lines (maximum 3078 characters excluding LF). The existing
core-0 UART preview task (priority 3) forwards a fixed snapshot of pending
lines, without copying a whole sweep or blocking the companion owner.
ADC/SD workers, queues and priorities remain unchanged.

Oversized/unrecognized lines are rejected; buffer exhaustion drops the
incoming line without blocking. UART overflow/framing errors flush pending
driver bytes and discard through the next LF. LF and CRLF are accepted.
Validation checks framing/tags, not numerical plausibility; without a
checksum undetected corruption remains possible. Finite buffering is not
a lossless-delivery guarantee.

`U0` and stopped recording suppress forwarding and discard completed
pending lines. Resume with `U1` while recording for new live measurements.
Do not expect stop-time or quiet-period measurements to be replayed. SD
binary downloads hold the stdout stream lock through FDATA/body/FDONE so
auxiliary/preview printf output cannot enter the binary payload.

Status `?` adds `#AUX:RX=...,TX=...,BAD=...,DROP=...,MUTED=...,UARTERR=...`:
counts since boot of accepted lines, complete host writes, rejected lines,
buffer/host-write losses, lines discarded while quiet/stopped, and observed
UART error events. TX means accepted by host output, not monitor acknowledgement.
