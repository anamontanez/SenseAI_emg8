# Luna handoff: validate the participant-recording candidate

## Objective and scope

Validate candidate `2d58cb9` for the upcoming participant collection, in small
steps. Priorities: raw acquisition and SD integrity, then auxiliary sync/control,
then UDP. The next work is hardware verification and reporting, not optimization.

Work only in `D:\PhD\Code\SenseAI_emg8`, branch `feature/samping-speed`.
Do not edit `IA-Arm_Monitor`, `Electromyographic-interaction-variables`, or the
analysis project. Do not format/delete/overwrite SD files. Commit only on this
branch; do not push unless requested. Leave the pre-existing untracked
`.pio-core/`, `.pio-core2/`, and quiet-beacon sdkconfig alone.

Read this file first, then the identity/monitor contract in
[participant-readiness-2026-09-28.md](participant-readiness-2026-09-28.md).
Use [next-tests-2026-09-28.md](next-tests-2026-09-28.md) only for earlier hardware
evidence; the full historical WORKLOG need not be re-read to begin.

## Exact state at handoff

- Candidate source: `2d58cb9` (participant identity + start/parser safeguards).
  Previous deployed source: `a782687` (clear LED states), to the best available
  record. The candidate has **not** been flashed or hardware-tested.
- Build: `esp32-s3-storage-bench`, SD enabled, combined I2C, ready pins
  15/42/41/40. Do not use the historical `esp32-s3-devkitc-1` environment on
  this attached wiring. Build passed: 72,576 B static RAM, 942,830 B flash.
- All 47 offline tests pass. No need to rebuild or rerun them merely to flash
  the unchanged archived image. Changes or a regression justify new checks.
- Verified archived image directory (ignored, local only):
  `benchmarks/participant-review-2026-09-28/candidate-8748a463/`.
  It contains firmware BIN/ELF, bootloader, partition table, and sdkconfig.
- BIN SHA256:
  `8F42D1F24BE253DDBFCBF82B5AD8F9BDCC8F3876BC299906336BFF8415E8780D`.
- ELF SHA256 / required post-flash `#FIRMWARE:ELF_SHA256=` value:
  `8748a4630c008042d69f079054f2418a5abc1dd963878c638613f69020b3085e`.
- Last enumeration: COM5, COM11, COM12. Bracelet was on **COM5**, UART 460800.
  No COM port was opened during Astra's review. Exclusive availability has not
  been reconfirmed; the preceding question about board/port availability is
  unanswered. Confirm it before opening/flashing if the user has not supplied
  a fresh answer. Do not ask again for general testing authorization.
- Last stated hardware: ADCs, IMU and SD connected; ADC inputs open, no muscle
  sensors or auxiliary. Confirm any changes. Open-pin results are transport/
  storage evidence, never analog or end-to-end auxiliary validation.
- The new USB-UART converter sometimes needs a physical reset after connecting.
  If needed, arm capture first, then tell the user it is ready for ONE reset and
  wait for their action. Do not repeatedly miss the reset window or infer a
  firmware failure from silence alone.

## Changes being tested

`J<subject>,<session>\n` arms a coded participant/session identity while idle.
`#IDENTITY:ARMED,<subject>,<session>` acknowledges it. Each tagged start writes,
syncs and closes `Jnnn.json` before acquisition, then emits
`#SESSION:<subject>,<session>,<file-set>` before `#REC`.

After a successful start, the tag is consumed and another start is refused until
a new tag is armed. `J?\n` then says `#IDENTITY:REQUIRED,,`; this describes the
NEXT recording, not loss of the currently active tag. Cancelled/failed starts
retain the tag for retry. Invalid idle identity commands disarm old tags.
`J-\n` explicitly restores untagged/free recording; reboot resets to OFF.
Queries work during recording; changes/clears during recording/countdown are BUSY.

Codes accept ASCII letters/digits/underscore/hyphen/period, max 32 subject and
64 session characters. The JSON says `prepared`, not completed. It contains
codes, exact file set, firmware ELF hash, mode/rate/countdown. It is excluded
from #SDSUM's binary-only byte total. Tagged recordings have five files;
untagged recordings retain four. Binary formats and UDP remain unchanged.

Other changes: UART rejects entire overlong/binary lines; initial metadata
enqueue failure now aborts before #REC. No ADC scheduling, buffers, priorities,
Wi-Fi settings or UDP packet-pump changes were made.

## Test sequence: stop at the first unexpected failure

### 1. Establish state and flash the exact candidate

Confirm the monitor releases COM5 and is not recording. Use the existing
non-resetting capture tool for `?`; require stopped state before flashing.
If SD is unavailable, resolve/report that separately before an acquisition test.

Known Python works; Windows Store `py -3.12` failed with access denied:

```powershell
$py = 'C:\Users\escob\.platformio\penv\Scripts\python.exe'
& $py serial_capture.py --port COM5 --command '?' --seconds 3 --metadata-only
```

Verify archived BIN hash again and inspect source status. If firmware source has
changed since `2d58cb9`, identify the change instead of confusing the checkout
with this archived candidate. No need to recompile a docs-only handoff commit.
The existing app partition is at 0x10000 and bootloader/partition layout are
unchanged. Flash only the archived application, avoiding a rebuild of a different
image or erasing NVS/card contents:

```powershell
$image = 'D:\PhD\Code\SenseAI_emg8\benchmarks\participant-review-2026-09-28\candidate-8748a463\firmware.bin'
Get-FileHash -LiteralPath $image -Algorithm SHA256
& $py 'C:\Users\escob\.platformio\packages\tool-esptoolpy\esptool.py' --chip esp32s3 --port COM5 --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 8MB 0x10000 $image
```

Require successful upload/hash verification. Query `?`, saving the UART capture:
check exact firmware hash, CONFIG, `SDSTATE:READY,NONE`, stopped status, IMU
presence, and `RATE:1000`. `J?` should initially report OFF. Ask the operator
about visible LED states when needed; there is no camera evidence.

### 2. Two short tagged SD readbacks

Run sequentially, with fresh output paths. Confirm only the bench owns UART;
for UDP it must also be the sole subscriber. Commands from the repo root:

```powershell
& $py tools/bench_sd.py --port COM5 --condition off --rate 1000 --seconds 30 --identity 'BENCH_A,review_01' --output benchmarks/participant-review-2026-09-28/sd-A-30s
& $py tools/bench_sd.py --port COM5 --condition udp --rate 1000 --seconds 60 --identity 'BENCH_B,review_02' --wifi-profile EMG8-24EC4A368770 --output benchmarks/participant-review-2026-09-28/udp-B-60s
```

The saved Wi-Fi profile was working via the phone USB tether arrangement; verify
the laptop can join the bracelet AP. A WLAN permission/association failure is
not evidence of a firmware regression. Never create a second subscriber to snoop
while the participant monitor is receiving: the last endpoint takes the stream.

For each run require:

- Distinct file sets and correct JSON participant/session/hash/settings.
- Exact #SESSION association, #SD:OK, #REC, and final #SDSUM:...,OK + #STOP.
- All ADC conversion counts equal the SD counts, matching binary byte total,
  no storage drops, no ADC I2C errors/retriggers, valid monotonic timestamps.
- Old files keep their names and sizes. For the two new small recordings, re-read
  A after B and compare local hashes if practical, to check contents as well.
- Every received UDP sample exists on SD. Report missing UDP packets separately
  from SD integrity. Preserve NETDIAG, queue peaks and send/drop/pause counters.

The bench already checks identity and binary readback. Explicitly inspect
capture/summary.json for ADC errors/retriggers and the per-channel rates.
Allow several minutes for UART binary downloads; keep progress updates concise.

### 3. Short command/lifecycle acceptance

Use a small bounded serial capture/harness, saving TX/RX. Reuse the safe port
opening pattern (DTR/RTS false before open). Expected negative acknowledgements
must be handled explicitly: `bench_sd.Card.until()` raises on every #ERR and
therefore is unsuitable unchanged for these negative tests.

- After B, request a start without J. Expect IDENTITY_REQUIRED + STOP, no REC,
  and no new files. `J?` remains REQUIRED.
- Arm a tag, then send a malformed J. It must reject and disarm the old tag;
  another start must be refused. Also check one overlong line followed by a
  valid query: the parser must recover at newline without executing its digits.
- Arm `BENCH_C,cancel_retry`, start, and cancel with standalone `0` during the
  countdown. Expect CD:ABORT/STOP and ARMED still holding the same tag. Empty
  prepared files and an INCOMPLETE summary are expected for this aborted attempt.
- Retry without re-arming, record briefly, stop and verify the same identity in
  a new file set. During countdown/recording, J replacement and J- must report
  BUSY; J? remains a query. Use C1 temporarily only if helpful and restore C10.
- Once idle, explicitly `J-` for a tiny free-recording smoke test: no reused
  participant tag/sidecar. Restore C10, R1000, Pdemo, L0,0, W0, U1 and release COM.

Do not blindly use `bench_sd_lifecycle.py`: it currently omits sd_summary from
its generated capture summary, assumes four files and selects Rmax. Adapt a
small test or repair that host harness within this repo if useful. Do not
misdiagnose its missing-summary assertion as firmware data loss.

### 4. Representative collection run, after short tests pass

Coordinate with the user/monitor agent using the linked contract. The monitor
must support the identity handshake and SD result gates before a participant
run is considered validated. Run one normal-duration dummy protocol (~11.4 min)
with labels/phases and inspect both copies. If actual sensors/auxiliary are
absent, state that limitation; a longer bench run is not a substitute.

Use the monitor's own logs/API for this phase, never concurrent COM access or
another UDP subscriber. The monitor needs all four CNT lines and SDSUM/STOP,
plus a bounded UDP tail drain before closing its WAL. A 100 ms env/IMU flush
can lag the SD stop acknowledgement. Keep actual-arrival/rate checks; STREAM
only confirms local send submission.

At ~48 MB per full session, a UART readback can take ~18-27 minutes. Plan that
explicitly; use the short exact readbacks first. A card-reader copy after clean
stop/ejection is an alternative when the user is available.

## Existing evidence and deferred work

- Today's two pre-change SD+UDP runs had exact SD counts, zero SD drops, and
  every received UDP record on SD. Host losses: 6 raw + 1 envelope packets;
  repeat: 1 raw + 1 IMU. Firmware ERR/DROP/PAUSES/THROTTLES were all zero;
  sampled UDP raw queues peaked at 76/65 of 1500. More queue RAM does not
  address these particular losses. Do not resume speculative tuning.
- Raw packet: 174 records, 1404 UDP bytes, ~21.75 ms worth across eight channels.
  Env/IMU normally batch for ~100 ms. Preserve this tested format for now.
- Saved raw rates ~998.42-999.93 Hz at the 1000 ceiling, IMU ~173-178 Hz with
  combined load and no read errors. Report honestly; do not quietly retune the
  cap to claim a literal 1000 Hz minimum. The preferred ceiling/minimum policy
  remains unresolved. Raw/SD outrank the optional 200 Hz IMU target.
- Envelope staggering, rate-limiter scheduling, IMU FIFO/priority, beacon
  changes and retransmission protocols require separate A/B tests after this
  reliability validation. Do not implement them in this pass.
- >=75-minute rollover acceptance remains outstanding. Existing long-run
  verification accumulates records in memory and compares low-word bytes;
  repair that before using it across wraps. Independent short participant
  recordings reset their clocks; their durations do not add into one wrap.
- Boot-without-SD refusal is useful when the operator can remove/reinsert the
  card with power off. Do not pull a live card, fill it deliberately, or format
  it to create failures. Existing offline tests inject storage failures.
- Capacity: 300 x 11.4 min means ~14.5 GB plus retries and 57 device-hours.
  The current card is roughly 15 GiB and previously needed reformatting after
  inconsistent FAT geometry. A few short passes are not a lifetime guarantee.

## Stop conditions and deliverable

On an unexpected reset, storage failure, wrong identity/hash, changed old file,
missing SD record, or broken start/stop boundary: safely stop if running, save
evidence and isolate the first failure. Do not pile on further changes or reuse
failed participant data. A small reproducible bracelet/harness fix is in scope;
a scheduling/protocol redesign needs another review.

Append actual results, artifact paths, image hash and unresolved limits to
WORKLOG and this handoff (or a linked results file). Distinguish BUILD PASSED,
HARDWARE PASSED and MONITOR INTEGRATION PENDING. Give the user a concise
ready/not-ready assessment grounded in those checks, and leave the board
stopped with all handles/receivers released. Do not claim 300-user reliability
or faultless UDP from a short bench pass.
