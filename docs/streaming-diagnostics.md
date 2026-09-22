# Streaming diagnostics

Sampling and SD recording take priority over UDP. The default rate is the
1000 Hz average ceiling. This does not imply evenly spaced 1 ms conversions:
All mode also schedules envelope conversions on the same ADCs.

## Live status

The existing UART `?` query now includes two additional diagnostic lines.
Existing STATUS, HEALTH, sample records, UDP packets and SD layouts are
unchanged. Diagnostic output follows the existing UART quiet handling.

`#NETDIAG` fields:
- TX: datagrams accepted by sendto, not acknowledgements from the receiver.
- ERR: unsuccessful send attempts; the pending batch is retained for retry.
- DROP: records rejected by full/unavailable UDP queues, before packet
  sequence assignment. No packet gaps does not prove no record loss.
- PAUSES: number of times the sender entered urgent SD-pressure deferral.
- THROTTLES / THROTTLE_MS / THROTTLE_MAX_MS: count, total and longest
  moderate-pressure interval; transmission continues at a reduced cadence.
- PAUSE_MS / PAUSE_MAX_MS / PAUSE_NOW_MS: total, longest and ongoing deferral,
  in milliseconds. Total includes the current pause.
- SEND_MAX_US: longest measured sendto call, including task preemption.
- RAW_HI / ENV_HI / IMU_HI: queue occupancy maxima sampled by the network
  task, including while deferred. These are lower bounds; a queue can peak
  between observations. Counts exclude records already in an outgoing batch.

Network counters reset at Wi-Fi start, not at recording start. Fields are
atomic individual readings, not a transactional snapshot. Pause accounting
handles the 32-bit millisecond clock wrap; durations/totals are uint32.

`#MEM:FREE=...,MIN=...,LARGEST=...` reports internal byte-addressable heap
in bytes: current available bytes, heap low-water accounting since boot,
and largest currently available allocation block. These query the allocator,
unlike the linker's static RAM percentage. Minimum values aggregate per-heap
low-water marks and should be treated as a conservative memory budget.
Wi-Fi buffers, RTOS queues, stacks and SD filesystem state use runtime heap.

Use `tools/bench_acquisition.py --status-interval 2` to include these counters
throughout a direct capture. Default zero retains existing test behavior.

## Interpreting pauses and display smoothing

Compare packet arrival times with original device timestamps, sender queue
drops and pressure counters. Large arrival gaps followed by back-to-back
packets can be delayed delivery; timestamps/record counts reveal whether
data is missing. Monitor display rows are interpolated, so their continuity
does not by itself establish original-sample integrity.

A bounded playback buffer can smooth arrival jitter by adding display
latency. It must preserve original device timestamps and keep lost-data
indicators visible. It should not filter amplitudes or change saved data.
Size it from measured arrival delays and label phase changes by device time,
rather than silently shifting experimental labels to display time.
No monitor changes are made by this firmware work.

## Queue memory budget

Current raw queues: SD 12000 records (96000 bytes of payload, 1.5 seconds
at 8000 records/second) and UDP 1500 records (12000 bytes, 187.5 ms).
The UDP raw queue was increased from 1000 by 500 records: 4000 more bytes.
Envelope/IMU queue sizes remain SD 1000/400 and UDP 256/100.
These totals exclude RTOS queue bookkeeping and outgoing packet batches.

Reserve allocator headroom for Wi-Fi bursts and shutdown/reconnect as well
as acquisition. Do not size new queues from the static build percentage
or one idle heap reading. Larger UDP queues do not increase sampling rate
or fix over-the-air loss, and can increase queued display latency.


The sender now attempts up to two raw packets per pass while catching up;
envelope and IMU remain limited to one packet per pass. Each pass is followed
by a 5 ms delay normally, or 10 ms when SD has 3000-8999 raw records pending.
At 9000 pending records, transmission is deferred and pressure is rechecked
every 5 ms. The moderate-pressure raw service budget is 348 records/10 ms,
with headroom above the 80 raw records arriving in that time. A pending full
batch is retried before dequeuing more records. After a successful retry, one
additional raw packet may be assembled and sent to reduce the backlog. Partial
Raw partial batches flush after 30 ms; envelope and IMU partial batches flush
after 100 ms. This bounds catch-up work while
avoiding the unrecoverable queue growth measured with a one-packet budget.

Wi-Fi transmit power is capped at the ESP-IDF 8 dBm step. With the present
auxiliary-board 3.3 V supply, the default 20 dBm ceiling repeatedly caused
hardware brownout resets even in the SD-free image. The 8 dBm image completed
the matching 30-second run with zero packet gaps and zero queue drops.


## Measured result on the attached board

The final 60-second All/1000 Hz capture saved all 479990 acquired raw and
23995 envelope samples to SD. UDP queue drops were zero, with a sampled
raw queue peak of 1305 (above its old 1000 capacity). Three raw packets,
522 samples, were missing from UDP but present on SD: 99.8964% ADC delivery.

Raw packet arrivals still had a 155.7 ms 99th-percentile interval and 313.3 ms
maximum. These are host receive times, not over-the-air timestamps.
Minimum internal heap was 21544 bytes in this run. One 19 ms moderate-pressure
interval occurred; no urgent deferral occurred. Longer stalls remain to be
tested under this policy. The prior rule produced an 804 ms full deferral
during a 709 ms SD sync, too long for available RAM to cover with UDP buffering.

For a monitor playback-buffer experiment, about 250-350 ms is a reasonable
starting latency from these observations, not a worst-case guarantee.
Keep real gaps/counts visible and preserve timestamps/saved samples.
The final IMU rate was about 109 Hz despite its nominal 200 Hz setting; that remains
a separate acquisition scheduling issue. Full results and limits: WORKLOG.md.
