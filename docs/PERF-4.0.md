# Performance and sample-loss warnings in 4.0 (feat40-perf)

Four reports from Discord after the 3.9 release, all from users with fast PCs. This page lists the cause of each, what changed, and the
measured numbers. Numbers from GitHub's CI runners are marked with the runner; nothing here was measured on a real nRSP-ST or on the
users' machines.

## (a) "This computer is too slow for this setting" at 3 % CPU

**Where it is decided.** `app/ui/next_step_logic.h` (`State::TooSlow`), filled by `nextStepLine()` in `app/panels/diag_ui.cpp` from
`Engine::sampleLoss()`. The text is not in the translation catalogs (`app/i18n/*.txt` have no entry for it), so every language showed the
English line.

**Cause.** 3.9 set `cpuLoss` for any ring overflow in the last 5 s, and called every one of them a slow computer:

1. A network radio that reconnects reports the samples it never got through `IqRing::noteLost()`. Those were added to the ring's
   `dropped()` count, so a reconnect of rtl_tcp, SpyServer, SDR++, TCI, KiwiSDR, Hermes Lite 2 or the nRSP-ST counted as "ring overflow"
   and showed "This computer is too slow".
2. A one-off hold-up of the sample thread (a start or retune building its tables on the first blocks, the OS taking the core for a moment)
   overflows the ring just like a receiver that cannot keep up.
3. "3 % CPU" in the Windows Task Manager is one full core on a 32-thread machine. The receiver of most modes was one thread, so a receiver
   that really cannot keep up looks idle there.

**Fix.**

- `IqRing::lostAtSource()` (C++ and Rust ring wrapper) keeps source-reported losses apart. They show as "link gaps" in the sample-loss
  tooltip and as "The network radio is not delivering its full rate. Use a wired connection, or a lower sample rate." in the next-step line.
- Every overflow is classified from the sample thread's own figures over the last two seconds (`core/include/dect2/engine/overflow_cause.h`):
  - Starting: within 5 s of a start or reset. No warning.
  - Cpu: the receive path needs at least 90 % of real time, and at least 60 % of that time is on the CPU. Only this case shows
    "The receiver cannot keep up at this sample rate: it needs more than one CPU core gives. Lower the sample rate or the bandwidth."
  - Waiting: behind, but mostly not on the CPU (worker threads, a lock, or the OS gave the core to other programs).
  - Stalled: the receiver keeps up, but the sample thread was held up outside it.
  Waiting and Stalled show "Some samples were lost while the receiver was held up for a moment. The CPU is not the limit; close busy
  programs if it keeps happening." The tooltip says which one it was. `ONAIR_TRACE_OVERFLOW=1` prints each overflow and its window.
- The receive-path timing now covers all of `Engine::ingest()`, because the front end before the receiver runs on the same thread.
- The Reception doctor's "OnAir cannot keep up" finding now needs the Cpu cause. Network link gaps count as radio-side drops.

Tests: `overflow_cause` simulates a ring of the engine's size and a sample thread running the engine's loop. It checks a receiver at 130 %
of real time on the CPU (Cpu), the same receiver 20 % on the CPU (Waiting), a fast receiver held up 1-9 s (Stalled, never Cpu), a hold-up
2 s after the start (Starting), and a receiver at 90 % (no overflow). `next_step` checks that held-up is never "too slow" and that a
network radio gets network advice. `common_ring_wait` checks that source losses are kept apart.

## (b) Missed-sample and "use a USB 3 port" warnings at LOW sample rates

**Cause.** The delivered rate (`deliveredPct`, which drives the red "% of set rate", the "USB 3 port, short cable" line and the doctor's
USB finding below 97 %) was the samples offered over a two-second window on the sample thread's clock. A radio hands over whole
transfers, so the window held a varying number of them and read up to one transfer short. At high rates a transfer is a few ms and the
error is small. At low rates it is not: 131072 samples are 0.5 s at 250 ksps. Simulated with the engine's look cadence (33 ms, now and
then 300 ms) and nothing lost, the 3.9 window read as low as:

| Radio (simulated) | 3.9 window, lowest | 4.0 |
|---|--:|--:|
| 62.5 ksps, 16384-sample transfers | 90.8 % | 99.5-100.5 % |
| 250 ksps, 131072-sample transfers | 77.9 % | 99.2-101.0 % |
| 1.024 Msps, 131072 | 95.0 % | 99.7-100.3 % |
| 2.4 Msps, 262144 | 95.7 % | 99.5-100.5 % |
| 10 Msps, 65536 | 99.7 % | 99.9-100.1 % |

**Fix.** `DeliveryRate` (`core/include/dect2/source/delivery_check.h`) measures between the radio's own write stamps. Each window (two
seconds, or four transfers when that is longer) keeps its least delayed write, and the rate is the samples between the least delayed writes
of two windows over the time between them. A late transfer or a short OS hiccup does not move it. A radio that stops writing still reads
low, because once the last write is older than one transfer plus 0.25 s, the observer's clock stands in for the next write. A radio really
at 90 % reads 89.7-90.3 %, and a 1.5 s stall reads 63-67 %.

The gap detector (`DeliveryCheck`) was checked at the same low rates (48 kHz sound card, 62.5 ksps, 250 ksps, 1.024 Msps, with jitter and
hiccups). It found no false gaps, so it is unchanged.

Tests: `delivery_check` gained the low-rate radios (no gap and about 100 % delivered at every rate from 48 kHz to 20 Msps), the 90 % radio,
the stall, and a check that the 3.9 window does read under 97 % in the same simulation.

## (c) nRSP-ST network receiver (SDRconnect WebSocket, Full IQ) stalls above about 5 Msps with "Ring overflow"

**Causes found** (`core/src/source/source_nrspst.cpp`, `core/src/net/ws_client.h`):

1. **Message size limit.** Any WebSocket message over 1 MiB was refused as bad data. The connection was dropped and opened again. The API
   names no message size, so a server whose IQ messages grow with the rate passes 1 MiB somewhere between 5 and 10 Msps (for example 40
   messages a second at 10 Msps is 1 MiB each). Above that rate every message was refused and the stream stalled. Because of (a)1, each
   reconnect's loss also showed as "Ring overflow". The limit is now 16 MiB.
2. Three `recv()` calls per message (header, length, payload). Reads now go through a 256 KiB buffer, and payloads of 64 KiB or more are
   read straight to their place.
3. A per-byte conversion loop. It now uses the vectorised `convertCs16`, which gives the same floats bit for bit (x / 32768 equals
   x * (1/32768)).
4. The ring overflow itself: at 6-10 Msps the single sample thread ran the front end and the receiver one after the other. See (d).

The socket already had a 16 MiB receive buffer, and the ring holds 1.6 s at 10 Msps. Neither was the limit.

Test `nrspst_rate`: a fake SDRconnect on localhost sends a sample counter in messages of 1024, 4096, 16384 and 262144 samples. Each size
runs twice: as fast as the source takes it (must be at least 15 Msps) and paced at 10 Msps for 3 s, with a consumer reading the ring as the
engine does. The paced run must drop nothing and keep every sample in order.

| Message size (samples) | As fast as it goes, macOS runner (macos-15) | Paced at 10 Msps |
|--:|--:|---|
| 1024 | 183 Msps (733 MB/s) | 10.02 Msps, 0 dropped, 0 out of order |
| 4096 | 258 Msps | 10.01 Msps, 0 dropped |
| 16384 | 517 Msps | 9.97 Msps, 0 dropped |
| 262144 (1 MiB + 2 B, refused by 3.9) | see the Linux / Windows runs below | |

The parser and the ring are 18 to 50 times faster than 10 Msps needs, so the source side was never the limit once messages were
accepted. Of the causes above, the message size limit is the one that matches "stalls above about 5 Msps". It is not confirmed on a real
nRSP-ST, because SDRconnect's message size is not known here.

## (d) "16 cores, only 1 core gets used"

**What was threaded already** (docs/threading.md): the spectrum (its own thread, skips when behind), the second modes (their own threads),
the TV chains (DVB-T2 LDPC on the pool, ATSC/DVB-C/ISDB-T/DTMB stage threads), the heavy decoders of about a dozen modes (FT8/FT4, TETRA,
Wi-SUN, sondes, NR, Iridium, ...), the whole-span banks (per-carrier jobs on the pool). What was not: the common path. One analysis thread
read the ring, ran the front end (non-finite check, gain meter, IQ correction, offset mixer, noise blanker, spectrum / second-mode /
monitor copies, DC and spur notches, the decode shift) and then the receiver, block after block. That thread was the limit for every
mode without its own worker.

**Change.** The receiver runs on a stage thread of its own, behind the front end (`Engine::rxPipe*`, engine.h / engine_loops.cpp):

- The analysis thread keeps the front end and hands each block, in order, to a `StagePipeline` (Block policy, 4 blocks) whose thread
  calls the receiver. The two threads overlap, so throughput is set by the slower of front end and receiver, not their sum.
- Results stay the same. Before anything that reads or resets the receiver (the reset at the top of a pass, telemetry and the mode
  switches at its end, the gap mark after a catch-up), the analysis thread waits until the stage is idle. So the receiver gets the same
  blocks, in the same order, and is read at the same points as on one thread. The hardware-time count moves with the block.
- Automatic by default: on with 4 or more hardware threads, from 4 Msps, not in Low power mode. Below 4 Msps and on laptops in Low power
  mode nothing changes (no extra thread, no extra wake-ups). Settings > Advanced > "Receiver on its own thread" (Automatic / On / Off;
  key `rxThread`), `ONAIR_RX_THREAD=0/1`, off with `DECT2_THREADS=0`. It is decided at each Start.
- The load figure and the overflow classification count the stage's CPU time and the waits for it.

**Verification.** `bench_modes --compare-rx` runs each mode and rate twice on one thread (the control) and once with the stage thread.
The engine is drained before the results are read. The saved results, the blocks decoded and every transport-stream packet must match.
It prints the max sustained rate of each run (signal through the whole receive path per wall second, unpaced, spectrum on). ctest
`perf40_rx_thread` covers fm, adsb, ais, dmr, pocsag and airband at 2.4, 10 and 20 Msps. `perf40_rx_thread_tv` covers t2, dvbt, dab and
atsc at their rates. RESULTS: see below.

## Numbers

**Before (3.9 path, macOS runner macos-15, `perf40_engine`, commit 0968801e0):** the max rate the whole receive path sustains, unpaced, one
thread. The spectrum was off in this run: before 8ce077481, bench_modes parsed `--spectrum` but did not pass it on to its runs.

| Mode | 2.4 Msps input | 10 Msps input | 20 Msps input | Busiest thread at 20 Msps |
|---|--:|--:|--:|--:|
| fm | 12.8 | 39.7 | 85.9 | 47 % of the process CPU |
| adsb | 27.1 | 80.3 | 105.0 | 42 % |
| ais | 71.6 | 122.2 | 95.0 | 50 % |
| dmr | 55.4 | 137.6 | 173.4 | 42 % |
| pocsag | 54.5 | 86.6 | 102.8 | 50 % |

("max Msps": the input rate this machine could carry for the mode. The numbers depend on the input rate because each rate gets its own
decimation chain.) On this runner every mode keeps up with 10 Msps more than three times over. So the "stalls above 5 Msps" report on a
fast PC does not fit a receiver that is short of CPU. It fits (c)1, the message limit, and the reconnects that 3.9 counted as ring overflow.

**With the receiver on its own thread** (`perf40_rx_thread`, spectrum on, the engine drained before the results are compared). Max
sustained input rate in Msps, one thread then the stage thread.

Linux runner (ubuntu-24.04, 4 vCPU Xeon Platinum 8573C), run 43158896959:

| Mode | 2.4 Msps | 10 Msps | 20 Msps | Results (one thread twice, then the stage thread) |
|---|---|---|---|---|
| fm | 7.0 / 7.0 (1.01x) | 30.7 / 30.6 (0.99x) | 56.2 / 56.4 (1.00x) | identical |
| adsb | 19.2 / 19.4 (1.01x) | 68.6 / 70.7 (1.03x) | 102.0 / 113.2 (1.11x) | identical (463 blocks) |
| ais | 53.3 / 53.5 (1.00x) | 125.5 / 125.5 (1.00x) | 72.8 / 78.2 (1.07x) | same counts (16; the saved list carries times) |
| dmr | 77.2 / 69.9 (0.91x) | 126.1 / 144.2 (1.14x) | 144.3 / 156.2 (1.08x) | identical (318 / 310 blocks) |
| pocsag | 43.1 / 43.5 (1.01x) | 70.8 / 74.0 (1.05x) | 76.8 / 80.5 (1.05x) | same counts (80) |
| airband | 11.8 / 12.6 (1.07x) | 13.4 / 13.6 (1.01x) | 13.8 / 13.9 (1.01x) | same counts (3) |
| t2 (9.143) | 21.7 / 22.3 (1.02x) | | | identical |
| dvbt (9.143) | 13.7 / 14.0 (1.02x) | | | identical (85845 blocks, 85842 TS packets) |
| dab (10) | | 43.9 / 45.8 (1.04x) | | identical (744) |
| atsc (8) | 19.4 / 19.7 (1.01x) | | | identical (76388 TS packets) |

macOS runner (macos-15, 3 cores), run 43152759661 (before the drain fix, so only the speed counts here): fm 1.0-1.1x, adsb 1.2x,
ais 0.9-1.45x, dmr 1.3-1.4x, pocsag 1.06-1.38x, airband 0.95-1.24x.

One earlier Linux run (43152727760) showed "DIFFERENT" for adsb at 2.4 and 10 Msps and for dvbt. That run stopped the engine while
the last ~50 ms of the file were still being read, so it was a tail-timing artefact of the benchmark. With the engine drained, every
comparison matches.

**What this says.** The stage thread gives 0.9-1.14x on Linux and up to 1.45x on macOS, and the results match. The gain is small because
the front end is a small part of the work: the receivers themselves are most of the cost. FM at a 2.4 Msps input sustains 7 Msps and
airband 12-14 Msps at any input rate, both on one thread inside the receiver. Using many cores for one mode needs parallel work inside
each receiver, for example splitting the decimation filters across output ranges, which gives the same results bit for bit, or putting
the airband channel bank on the pool. That is per-mode work for 4.x. The profile (`perf40_profile`, quick.yml `perf=`) lists where FM and
airband spend their time (below). On the CI runners, every mode measured except FM at a 2.4 Msps input and airband carries 10 Msps on
one thread with room to spare.

**Profile** (Linux runner, run 43160269772, `perf record` over `test_perf40_profile`: fm and airband at 2.4 and 10 Msps, the
generator stage included). By thread, the hottest receive-side functions are: `AirbandReceiver::feed` (9 % in all), `FmSubcarriers::feed`
(5.8 %), `llroundf` in a receiver loop (4.2 %), `forwardAvx2` FFT (2.6 %), `FmReceiver::Impl::feed` (1.9 %), `desampAvx2` (1.2 %),
`AirbandBandWatch::feed` (0.9 %). The front end on the analysis thread barely registers; the spectrum thread is 1.4 %. So the next step
for "use more cores" is inside these receivers: the airband channel loop and FM's subcarrier (RDS/SCA) stage are per-sample scalar code,
and each could run as a pool job or a stage of its own without changing its output. Not done in this branch.

## Files

- `core/include/dect2/source/delivery_check.h` (DeliveryRate), `core/include/dect2/engine/overflow_cause.h` (new)
- `core/include/dect2/common/ring.h` (lostAtSource), `core/include/dect2/engine/engine.h`, `core/src/engine/engine.cpp`,
  `engine_loops.cpp`, `engine_watch.cpp`, `engine_start.cpp` (the receiver stage, the classification, the delivered rate)
- `app/ui/next_step_logic.h`, `app/panels/diag_ui.cpp`, `app/panels/doctor_ui.cpp`, `app/ui/widgets.cpp`, `app/ui/settings_table.h`,
  `app/ui/settings_window.cpp`, `app/ui/toolbar_source.cpp`
- `core/src/source/source_nrspst.cpp`, `core/src/net/ws_client.h` (also used by KiwiSDR and TCI)
- Tests: `tests/engine/test_overflow_cause.cpp` (new), `tests/source/test_nrspst_rate.cpp` (new), `tests/source/test_delivery_check.cpp`,
  `tests/app/test_next_step.cpp`, `tests/common/test_ring_wait.cpp`, `tests/common/test_ring_rs.cpp`, `tests/source/test_nrspst.cpp`,
  `tests/bench/bench_modes.cpp` (`--compare-rx`, max Msps column, the spectrum flag passed on)
