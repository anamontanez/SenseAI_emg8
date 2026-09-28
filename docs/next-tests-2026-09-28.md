# Next deployment: evidence review and bounded test plan

Prepared 2026-09-28 against `feature/samping-speed` at `229a9b0`.
This is a plan for the next implementation/deployment turn, not a claim that
the tests below have run. Only the bracelet repository is writable in scope;
the monitor and Ana's auxiliary firmware belong to their respective agents.

## What we can conclude now

The September 27 build passed 34 host checks. Its reported static RAM usage
was 72,996 bytes (22.3%); flash usage was 947,266 bytes (30.1%). Those numbers
do not include runtime queue/radio allocations. Earlier combined runs reached
roughly 18-22 KB minimum internal heap; do not increase buffers on the basis
of the static RAM percentage.

The last connected bench board was ESP32-S3 MAC `24EC4A368770`, CP210x serial
`78C2C3DF451BF111BDA9BAC40F0F12F8`, on **COM5**, not COM9. Detect the port again
at the next deployment. The attached hardware has ADCs, IMU and SD, but no
sEMG sensors or auxiliary controller.

The upload transcript and subsequent boot demonstrate the new firmware was
running. The final application-flash hash acknowledgement was not retained in
the conversation output; save a complete upload log and image identity next
time. The currently retained artifacts, hashed during this review, are:

- BIN SHA256: `ce58494f9211671e99c5df33374a973b67dd374656d085f895c66e0383b80303`
- ELF SHA256: `244823a1fe5b6bfeb7835500b7f165b8d526278c99a56f3014be64cffa9b50eb`

Observed UART evidence:

```text
#SD:CRC_AFTER_READY
#ERR:SD_MOUNT:13
#INIT:ADC=OK,SD=FAIL,IMU=OK
#READY
#SD:FAIL,INIT,0
... after command 1 ...
#MODE:1
#SD:CRC_AFTER_READY
#ERR:SD_MOUNT:13
#SD:FAIL,INIT,0
#ERR:SD_REQUIRED
#STOP
```

This verifies boot fault reporting and one attempted recovery/start refusal.
It does **not** verify successful SD writing, recovery to a usable card, sustained
1000 Hz acquisition, UDP delivery, physical fault-LED visibility, or time sync
with the absent auxiliary. The heartbeat evidence after refusal covers only a
few seconds. No full recording or long stability soak ran on this build.

Error 13 is `FR_NO_FILESYSTEM`: FatFs did not recognize a supported volume.
It does not establish that the card is defective. In the installed ESP-IDF
5.4.0 FatFs source, `FF_FS_EXFAT=0` and `FF_LBA64=0` (the GPT scan is behind
the latter). Unsupported formatting/partition layout, damaged filesystem
metadata, or incorrect sector data remain hypotheses. The current card's
actual layout has not been read. Do not format it as a diagnostic shortcut.

The new fault LED is bright red in source, but its visibility needs an operator
check. Repeated `POWERON` boots during the previous capture are not evidence
of spontaneous power failure: those scripts opened an already-configured
serial port and then changed DTR/RTS, and some explicitly reset the board.
Use the safe serial pattern in `tools/bench_acquisition.py`: set DTR/RTS inactive
**before** opening, continuously drain UART, and save raw output. Firmware UART is
460800; ROM/early boot uses 115200, explaining the initial garbled bytes.

## First patch: close the restart hole and improve the evidence

### 1. Reject an empty acquisition configuration (required)

Source-confirmed failure path in `src/main.cpp`:

1. The first selected recording fails SD preflight or is cancelled.
2. The statement after worker creation resets `mode` to `Mode::Idle`.
3. A subsequent reed-switch start calls `startRecording()` without selecting
   a mode. The UART and physical-button paths do select a mode.
4. With Idle, `startADCs()` fills neither raw nor envelope configurations.
   Both workers skip every zero-count ADC and still acknowledge `ESP_OK`.
5. Once SD is usable, firmware can announce `#REC` with no EMG conversions;
   the IMU may still record, and final storage status can say `OK`.

Preserve the selected valid mode after a failed first start, retain All as
the reed's default when no mode is selected, and defensively reject an invalid
or empty acquisition configuration before auxiliary START / `#REC`. Do not
change ADC scheduling or timing. Add a regression covering the actual initial
failure/cancel -> reed retry path and the zero-configuration worker behaviour;
the present start test replaces `startADCs()` with a success stub and misses it.

Acceptance: failed/cancelled starts never acknowledge recording; retry uses
the selected mode (All by default), starts its intended ADCs, and produces
nonzero records on every enabled channel. Raw/Env mode choices survive retry.

### 2. Add bounded, read-only mount diagnostics (required if error 13 persists)

On a failed mount while stopped, report the symbolic result, card sector size
and capacity, then inspect sector 0 and plausible partition boot sectors with
bounds checks. Repeat each relevant read to distinguish stable bytes from
inconsistent transport. Report signatures, partition type/start and relevant
FAT geometry; avoid dumping user files or adding any filesystem writes.
Use a small temporary DMA-capable buffer released before acquisition. No
formatting, repair, erasure, or automatic partition conversion.

- Stable unsupported exFAT/GPT layout: identify it, then arrange a backed-up
  compatible test card. Do not expand filesystem support in this first patch.
- Valid compatible metadata but mount failure: compare the new SD wrapper
  with `c45a7bc` using a read-only diagnostic build of the old mount path.
  Do not run participant acquisition on the old silent-failure behaviour.
- Inconsistent reads / explicit transport errors: investigate contact, wiring,
  clock and supply, one change at a time. Neither power nor SD is preselected
  as the cause.

Adding an additive `#BUILD` line with the running app's embedded ELF identity
at boot and `?` is a useful small companion change. Record the actual build
environment and image hashes even if this is deferred.

### 3. Make the short-run harness verify the new save contract (required)

In `tools/bench_acquisition.py`, replace the fixed one-second stop wait with
a bounded wait for `#STOP`, all four `#CNT` lines, and the recording's
`#SDSUM` for SD runs. Continue draining UART/UDP throughout. Treat unexpected
early `#STOP`, reset, storage fault, or `INCOMPLETE` as explicit failure.
Use a generous measured close timeout (initially 30 seconds), never an
unbounded wait. Preserve incomplete evidence and release the serial port.

In `tools/bench_sd.py`, verify the summary's file set, counts, byte total and
`OK` against the four downloaded files as well as the ADC counters. Its
current verifier does not check `#SDSUM`. Test delayed close (>1 s), early stop,
missing summary and incomplete summary, so failure cannot look like success.

## Deployment and short acceptance runs

Build and flash **esp32-s3-storage-bench**. The generic devkit environment has
the historical ready-pin mapping; the quiet-beacon environment is experimental.
Keep default R1000, countdown 10 s, existing task priorities, SD/UDP buffers,
100 TU beacon and tested radio transmit setting. Use C1 only for labelled
rapid-cycle bench tests and restore C10 afterward.

Save each run under a new ignored `benchmarks/robustness-2026-09-28/...`
directory: source revision/diff, build configuration, BIN/ELF hashes, complete
build/upload logs, continuously captured UART, host-timestamped UDP datagrams,
before/after card listing, new SD files and machine-readable verdicts.
Read/download commands must never overwrite existing files. Compare old file
names and sizes before/after; hash existing files too if claiming their contents
are unchanged. Close COM on exit. Do not infer board health from a capture
that discarded bytes or caused a reset on open.

1. **Storage refusal and retry:** confirm the existing mount error, bright red
   visibly distinct from idle, start refusal, and repeated bounded attempts
   without progressively lost heap/disk slots. Then use a recognized card and
   verify recovery without reboot. Card handling is operator-assisted.
2. **60 s All/1000, Wi-Fi off:** require SD counts equal acquired per-channel
   counts; about 1000 Hz raw and 50 Hz envelope per channel over full windows;
   no storage/metadata drops, I2C errors, unexpected retriggers or resets;
   successful final summary. Measure IMU rate, do not substitute target 200 Hz.
3. **60 s All/1000, UDP on:** repeat the storage checks; compare every received
   record with SD. Report packet gaps, delivery fraction, arrival-gap duration,
   device-time gaps, SD write/sync latency and minimum heap independently.
   Every received record should exist on SD; UDP need not contain every SD
   record. A sequence gap alone does not localize loss to radio versus host.
4. **Repeated boundaries:** three short start/stop cycles plus countdown cancel
   and retry, including the reed path. Each start gets a new file set; no old
   sample timestamps cross epochs. Exercise U0, label/phase commands and their
   timestamps. With no auxiliary attached this validates only bracelet-side
   handling, not downstream reception or synchronization accuracy.
5. **10 min combined soak:** only after the short tests pass. Distinguish delayed
   packet batches from missing samples using device timestamps, sequences,
   ADC counts and SD contents. Keep the new save/fault events in the capture.

The historical 34 host checks are useful logic tests, including real source
fragments with fake I/O; they are not a substitute for these hardware runs.
If a run fails, isolate the observed stage before changing any performance
parameter. A single short successful run cannot certify faultless operation.

## Later tests and deferred work

- **Fault during recording:** first use test-only write/sync/close fault
  injection. It exercises the safety response but cannot emulate a physical
  card removal. Removal/full-card tests require a backed-up disposable test
  card and an operator; do not risk participant files.
- **75-minute wrap test:** first fix the harness. The current UDP summary sorts
  `(ts - first) & 0xffffffff`, which aliases samples after a full clock cycle.
  It also keeps all timestamps in memory; SD verification keeps full streams
  in Counters. Use extended time per channel/IMU with sequence-aware reorder
  handling, streaming statistics and a bounded/disk-backed comparison. Then
  require continuous SD data and correctly decoded kind-4 anchors across
  71.6 minutes. `tools/sd_clock.py` is a reference for ordered SD streams, not
  a complete reordered-UDP solution. Store artificial wrap tests alongside it.
- **IMU:** retain the current 200 Hz configuration while measuring effective
  rate. Consider a deliberate 100 Hz setting after the SD baseline if useful;
  lowering the target does not itself guarantee regular sampling or solve
  missed readings. FIFO/data-ready is a separate driver/hardware effort.
- **Raw regularity:** mean 1000 Hz is not uniform 1 ms sampling. Historical
  ~2.45 ms intervals every 20 samples remain. Spreading envelope conversions
  is a separate measured A/B change, with the same file-level checks; do not
  promise sub-1.2 ms intervals from scheduling alone.
- **Beacon interference:** keep the default. Open ADC pins cannot establish
  sEMG spectral improvement. Compare real sensor rest recordings, SD rates and
  UDP continuity before adopting a longer beacon interval.
- **Auxiliary validity/sweep timing:** requires Ana's firmware and connected
  hardware. Receipt stamps do not establish sensor acquisition time or 1 ms
  cross-board alignment.

## Handoff for the monitor owner (no monitor edits here)

The current monitor parser has no dedicated handling for the new sidebands.
Use `docs/companion-protocol.md` and `docs/robustness-2026-09.md` as the contract:

- Persist `#SD`, `#SDSTATE`, `#SDSUM`, `#EVENT`, `#AUXTS`, `#TSWRAP`, `#IMURATE`
  and `#STREAM`; retain the existing legacy phase/label/auxiliary payloads.
- Pair `#AUXTS` with the following auxiliary line and scope timestamps to the
  recording; label it as bracelet receipt time.
- Accept SD metadata extension 2; kind 4 is a time anchor, never a grasp label.
- Show saved status only after that file set's `#SDSUM:...,OK`; show a fault on
  refusal or incomplete save. Confirm acquired counts vs saved counts first;
  UDP count differences are a separate link-completeness result.
- `#STREAM:UDP` means the first successful local send, **not confirmed delivery**.
  Keep the monitor's received-rate/full-stream gate before participant cues.
- U0 now suppresses preview CSV/ordinary output; fault/control/save events and
  the auxiliary relay remain available. The monitor contract's older claim
  that U0 silences all UART output needs updating by its owner.

## Deployment results — 2026-09-28

Implemented the failed-start/reed-retry guard, a shared validated ADC channel
plan, cleanup when ADC startup fails after the companion has started, read-only
card-sector diagnostics, bounded stop/SDSUM checks in the acquisition harness,
and file-level validation of the firmware SDSUM. Added host regressions for the
actual ADC map, Idle rejection, and retry cleanup. The card diagnostic reads
LBA 0 and plausible partition boot sectors twice through a DMA-capable buffer;
it does not mount, format, write, or dump file contents.

All 41 host checks pass, including a simulated writer close delayed beyond one
second and refusal for absent/incomplete save summaries. The
`esp32-s3-storage-bench` firmware built and was
flashed to MAC `24:EC:4A:36:87:70` on COM5. Static RAM is 72,996 bytes (22.3%);
flash is 948,834 bytes (30.2%). BIN SHA256:
`F11273B2D0F13FA11A5AB34A476E0C36694DDE6E88579DF444DF752F7B5A68E9`; ELF
SHA256: `789F7F1BC063E03937E0A536EE6FB7D563BE2EFE77170A0280860FB9B36B05E3`.

The board's first post-flash status was idle, ADC/IMU healthy, SD unavailable.
Five UART start retries each completed card initialization, then returned
`FR_NO_FILESYSTEM` (13); each was refused with `#ERR:SD_REQUIRED` and `#STOP`.
The selected mode remained All after refusal. Repeated failures did not worsen:
the last two retry checks both reported 122,732 free bytes, 118,264 minimum, and
59,392 largest block. No recording files were opened. The UART logs are in
`benchmarks/robustness-2026-09-28/sd-retry-01.jsonl`, `sd-retry-02.jsonl`, and
`sd-retry-03.jsonl` (ignored local bench artifacts).

The card advertises 31,457,280 sectors of 512 bytes (15 GiB). Its MBR has one
FAT32-LBA partition (type `0x0C`) at LBA 2048 with 31,453,184 sectors, which
fits the reported card. The partition boot sector was read identically twice
and reports 512-byte sectors, 16 sectors per cluster, two FATs, 2,082 reserved
sectors, and **132,116,480 total sectors**—about 63 GiB, over four times both
the partition and reported card capacity. This is direct evidence of
inconsistent FAT32 volume geometry, not of a transient sector-read failure.
The card identity, CSD capacity, and FAT metadata still need comparison against
what Windows reports; this does not by itself prove which component is wrong.
No storage recording or UDP acceptance run was attempted after the firmware
correctly refused the unavailable SD.

The operator reports this card previously worked in this bracelet and was only
used there since its last format. That history means we should verify the size
reporting before changing the card. The follow-up firmware fix now distinguishes
`MOUNT` from `INIT`; on COM5 the board reports `#SDSTATE:FAILED,MOUNT` and
`#SD:FAIL,MOUNT,0` for the observed mount error 13. The updated image built at
72,996 bytes static RAM and 948,646 bytes flash; BIN SHA256:
`ED00C943A75EF04A438ECE672A597DFCC1430C3681DBC692E2D53CB24AE4BC7A`; ELF
SHA256: `74A06D9FF797915926B3ACDB955AD765488A3D060A33D3EFA20D48C7F642D0BA`.
The capture is `benchmarks/robustness-2026-09-28/sd-mount-label.jsonl`.

Next, compare the card's capacity and volume size using a computer/card reader,
without changing the card. In particular, check whether Windows sees a card
near 16 GB or 64 GB. If they disagree, collect the raw CSD/CID and sector data
before deciding whether a backed-up reformat or replacement is appropriate.
Once mounting succeeds, run the saved-file SD check first; only then proceed
to 60-second SD-only and SD+UDP captures. No analog-signal quality or physical
LED visibility conclusion is possible with open sensor inputs and no camera
view.

## Follow-up read-only register probe

Added a stopped-state CMD10/CMD9 reread through ESP-IDF's decoded CID/CSD
helpers. On mount failure, the diagnostic reports manufacturer/product ID and
serial plus the initialization-time and freshly decoded capacity values; it
still reads only identification registers and filesystem-identifying sectors.
Build and upload succeeded on COM5, with 72,996 bytes static RAM and 949,014
bytes flash (BIN SHA256
`B18BBB2FF2FC033CF0A5B93862B14C080C5EDF6274F1CA6F3A9C84CBE8FB2AC2`, ELF
SHA256 `1DB0FF833832E0813F6C5D89184577DACF51C0513A7AA6CE429F4252B9891771`).
A UART `?` query after upload confirms idle mode and `FAILED,MOUNT`; its capture
is `benchmarks/robustness-2026-09-28/sd-status-after-csd-flash.jsonl`. That
query did not reset the board, so it does not contain startup `#SDREG` lines.
Capture those on the next operator power cycle, then compare the reported
15 GiB CSD capacity with Windows before any card repair.
