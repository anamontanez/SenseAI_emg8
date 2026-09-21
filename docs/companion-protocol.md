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
| `P?\n` | Query current phase | `#PHASE:<name>` | none |
| `L<id>,<repetition>\n` | Set movement label and repetition | `#LABEL:<id>,<repetition>` | none |

Labels and phases are independent: rest must not erase the movement being
tested, and a label does not imply a grasp/rest transition. Set the label and
phase before starting with `1`–`4`; then send phase changes during recording.
Boot defaults to demo. Phase persists across pause, stop and mode changes;
send `Pdemo` when leaving a session, before any subsequent free recording.
Repeated phase commands resend the byte (there is no companion acknowledgement).
Host acknowledgement means accepted locally, not confirmed by the auxiliary board.

## Companion UART1

Bracelet TX GPIO4 to auxiliary RX, bracelet RX GPIO5, common ground,
115200 baud, 8N1. One dedicated task on core 0 owns every write, with priority
6, above the UDP sender (5). ADC workers and SD writer remain on core 1.
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

## Receiver findings for Ana

Read-only review of `src/main_medicion.cpp` found these outstanding issues:

- The RX task applies sync relative to `t_inicio_prueba_us`, but the main loop
  later resets that epoch and `g_clockOffsetUs`, then flushes UART after opening
  the SD file. A correctly received first sync can be overwritten or discarded.
  Establish the epoch on receiving `03` in the same owner as the sync parser;
  notify the measurement task without resetting/flushing that parser afterward.
- `volatile int64_t` is not cross-task synchronization on a 32-bit processor.
  Publish the epoch/offset together under a short lock or an atomic snapshot.
- The RX handler reads at most 32 bytes for each event and ignores UART overflow
  and frame errors. Drain available bytes and reset incomplete framing on errors.
- Bytes `04`/`05`/`06` are currently ignored; Ana must implement their handling.
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
reaches 3000 records, the UDP sender pauses packet transmission until storage
catches up; subscription and quiet-mode recovery remain responsive. Network
queue overflow is counted and may increase during these pauses. Sampling and
SD enqueue do not wait for networking. This expresses the requested priority;
a card stall longer than finite buffer capacity can still lose samples.

SDIO status reports the longest write and sync group durations observed in the
current recording, including time preempted by other work. These counters are
diagnostics, not a guarantee of card durability or future maximum latency.
