# Threading in OnAir (0.2.7)

Survey of how threads are used, the shared framework (`core/include/dect2/common/threadpool.h`, `queue.h`, `stage_pipeline.h`), where it is applied and where it is deliberately not. Rule for everything here: **the output is bit-identical for any number of threads**; `DECT2_THREADS=0` runs every shared piece on the calling thread (debugging, determinism checks), `DECT2_THREADS=N` fixes the pool size, unset = hardware_concurrency - 1 (1 to 8). The same number can be saved as `threads=N` in the settings file (0 to 64, #372; automatic when absent), read at start-up; the environment variable wins.

## 1. Threads today

| Thread | Created by | Produces / consumes | When it is behind |
|---|---|---|---|
| source / reader (one per radio: `source_*.cpp`, `hackrf_usb.cpp`, `native_common.h`) | the source on start | USB / socket / file reader writes cf32 into the `IqRing` | the ring counts `dropped`; the reader never waits for the analysis thread |
| `IqRing` (`common/ring.h`; the C++ one by default, `-DDECT2_RUST_RING=ON` swaps in the Rust ring `rust/onair_ring`, both stay and `tests/common/test_ring_rs.cpp` compares them) | - | lock-free SPSC ring of cf32: the reader writes, the analysis thread reads; sequence-locked "last write" stamp for the loss detector | drops the new samples |
| analysis thread (`Engine::analysisLoop`, `onair-analysis`) | `Engine::start` | ring -> active receiver `feed()`; Realtime QoS. Contract (`engine/engine.h`): it never waits for the interface, results are polled | the ring drops |
| spectrum thread (`Engine::spectrumLoop`, `onair-spectrum`) | `Engine::start` | gets a copy of each block from `feedSpectrum` (queue of 6, extra blocks skipped), runs the analyser | skips blocks; **never slows the decoder** (already off the decode path) |
| secondary receivers rx2..rx4 (`Engine::secondaryLoop`, `onair-rxN`) | `Engine::startSecondary` | own ring, own shifter + resampler, own receiver; Background QoS | own ring drops |
| results / telemetry (`Engine::pollMode`, `pollMode(int a, RxTelemetry&, seqs[])`) | the UI / remote API thread | copies the receiver's telemetry when its `seq` moved | not a thread of its own: runs on the caller, takes the receiver's own mutex briefly |
| per-mode decode workers (wisun, tetra, sonde, nr, nbiot, js8, iridium, gsmr, hfdig/FTx, dvbs symbol thread) | the receiver's constructor | `Impl::run()` consumes blocks the demod pushes (mutex + cv queue of the mode) | each mode has its own drop / wait rule |
| DVB-T2 (`t2/t2rx_impl.h` stage thread, `t2/t2plp.cpp` `PlpDecoder` + LDPC) | `T2Receiver` | frame stage thread; `PlpDecoder` loop takes jobs (max 3 queued, drops frames beyond), LDPC codewords of a frame spread over threads (**now the shared background pool**, before: threads created per frame), optional GPU (`gpu_ldpc.h`) | `PlpDecoder::submit` drops the frame and counts it |
| other TV chains (atsc, atsc3, dvbc, isdbt, dtmb, conceal, tsout, hls) | their receivers | split demod/decode, per-batch threads | opt-tv owns these |
| player / TS (`tv/player.cpp` `threadMain`, `tsout.cpp`, `hls.cpp`, `demo_ts.cpp`) | the player on open | TS packets -> decoder -> audio/video out | the player drops / waits on its own queue |
| audio (`audio/audioout.cpp`) | the audio device callback | a ring from the receiver; DAB / DRM / HDR audio decoders run on the receiver's own thread (DAB FIG + audio decode inside `feed`, not separate) | underrun = silence |
| wide bank (`engine/wide_bank.h`) | analysis thread calls `feed` | per carrier Channelizer + decoder; **now parallel** (section 3) | - |
| network (`net/remote_api.cpp` accept + one thread per connection, `share_server.cpp`, `feeds.cpp`, `pskreporter.cpp`, `wsprnet.cpp`, `nettuner.cpp`) | on enable | I/O bound, not on the sample path | their own timeouts |
| scanner, updater, sig_ident, multitv (`engine/multitv.cpp`: a channelizer + one engine per TV channel) | on use | background jobs | - |

Thread names (debuggers, Activity Monitor, perf, TSAN reports): `onair-analysis`, `onair-spectrum`, `onair-rx2..4`, `onair-w0..` (shared pool), `onair-bg0..` (background pool), stage threads are named by the owner.

## 2. The framework

- `ThreadPool` (`common/threadpool.h`, `core/src/common/threadpool.cpp`): fixed workers created once. `ThreadPool::shared()` (decoding work) and `ThreadPool::background()` (bulk work at background QoS: the LDPC decoders). `parallelFor(n, fn)`: the caller works too and returns when all n ran; safe from inside a pool task (no deadlock: the caller drains the indexes itself, a queued helper that arrives late finds nothing to do); first exception is rethrown on the caller. `submit(fn) -> future`. 0 workers (`DECT2_THREADS=0`): everything runs in index order on the caller.
- `BoundedQueue<T>` (`common/queue.h`): mutex + two condition variables, any number of producers/consumers; policy `DropOldest` (live path, push never waits, counted in `dropped()`) or `Block` (files, benchmarks: nothing may be lost); `close()` wakes everybody, `pop` drains then returns false.
- `ReorderBuffer<T>`: parallel jobs `put(seq, result)` in any order, `take()` hands them out in sequence order 0,1,2..., `skip(seq)` for a job that produced nothing, `reset()` starts over. Used wherever parallel stages must emit deterministic output.
- `StagePipeline<Item>` (`common/stage_pipeline.h`): "demod -> decode": one stage thread behind a bounded queue. `push` (producer), `flush` (wait until idle), `reset` (drop the queue and wait for the running item: after it returns no stale item runs, safe for retune), `stop`/destructor (closes, joins; declare it **last** in the receiver so it is destroyed first and no task runs on a half-destroyed receiver). `DECT2_THREADS=0`: the handler runs inside `push`, same output.
- TSAN-friendly: only `std::atomic` with explicit ordering, `std::mutex` + `std::condition_variable` for blocking, no `volatile`, no benign races. `cmake -DDECT2_SANITIZE_THREAD=ON` adds `-fsanitize=thread` (not together with `DECT2_SANITIZE`).
- Tests: `tests/common/test_common_threads.cpp` (ctest `common_threads`, and `common_threads_single` with `DECT2_THREADS=0`): 10000 random jobs parallel == sequential, nested parallelFor, exceptions, reorder buffer with 4 producers, queue policies, stage order / drop-oldest / reset in flight / shutdown while busy and while a producer blocks / 300 start-stop cycles / self-reset from the handler / inline mode, stress with 6 callers x 200 parallelFor and pools created and destroyed while busy.

## 3. Classification and decisions

Source: `docs/benchmark-modes-0.2.7.md` (cost = CPU seconds per signal second, both rates listed). Class A < 0.15: single thread. B 0.15-0.5 with a heavy decode after the demodulator: stage. C multi-carrier / multi-channel: per-carrier jobs on the pool. D > 0.5 wideband TV / front-end bound: opt-tv (they use the pieces above).

Why most are left alone: a thread hand-off costs a queue lock and one block of latency (about 10-50 ms of signal), and wins nothing when the demod and decode stages are tiny (class A is 80 of 117 rows). The big modes that have a heavy FEC stage already have their own worker (wisun, tetra, sonde, nr, nbiot, js8, iridium, gsmr, FT8/FT4, T2 LDPC, DVB-S2 symbol thread). What was missing were the multi-carrier paths, which got the pool.

| Mode | id | Rate Msps | Cost (per rate) | Class | Decision | Expected gain |
|---|---|---|---|---|---|---|
| VDL Mode 4 | `vdl4` | 2/4 | 8.914/0.000 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| DVB-C | `dvbc` | 20/10 | 1.773/1.119 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| ATSC 1.0 | `atsc` | 8 | 1.069 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| ISDB-T | `isdbt` | 16/8 | 0.875/0.669 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| ATSC 3.0 | `atsc3` | 20/10 | 0.835/0.609 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| HFDL | `hfdl` | 4/2 | 0.768/0.283 | C | per-channel jobs in the receiver, ordered commit (done) | up to #channels x |
| Drone Remote ID | `rid` | 20 | 0.759 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| DTMB | `dtmb` | 20/10 | 0.708/0.671 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| DVB-T | `dvbt` | 9.143 | 0.654 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| HRPT / AHRPT | `hrpt` | 12/6 | 0.607/0.375 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| QO-100 | `qo100` | 20/10 | 0.599/0.348 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| DVB-S/S2 | `dvbs` | 20/10 | 0.575/0.494 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| Wi-SUN FAN | `wisun` | 20/10 | 0.575/0.382 | B | already has a worker thread | already staged |
| DJI drones | `dji` | 20 | 0.517 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| Ionospheric sounders | `iono` | 20/10 | 0.503/0.240 | D | wideband front end: DSP-bound, front-end split needed (opt-tv/opt-spectrum) | - |
| MAC | `mac` | 20 | 0.494 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Wi-Fi networks | `wifi` | 20 | 0.459 | C | channel receivers behind a ChannelFilter: cost is the 20 Msps filter bank (left, see notes) | - |
| DECT cordless phones | `dect` | 10/5 | 0.399/0.176 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| GNSS | `gnss` | 8/4 | 0.381/0.344 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Analog TV | `atv` | 20/10 | 0.358/0.251 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| Spectrum analyser | `specan` | 20/10 | 0.311/0.130 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Zigbee / 802.15.4 | `zigbee` | 8/4 | 0.293/0.256 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| LoRaWAN | `lorawan` | 4/2 | 0.285/0.199 | C | SF search shares one dechirp (left) | - |
| 5G NR cells | `nr` | 20/10 | 0.271/0.141 | B | already has a worker thread | already staged |
| Wi-Fi HaLow (802.11ah) | `halow` | 8/4 | 0.266/0.154 | C | width receivers share the front end (left) | - |
| VDL Mode 2 | `vdl2` | 4/2 | 0.245/0.149 | C | same pattern as HFDL (not yet: tuning feedback loop) | - |
| Cell towers | `cell` | 20/10 | 0.240/0.121 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| DVB-T2 | `t2` | 9.143 | 0.239 | D | opt-tv owns the chains (ATSC/DVB-C/ISDB-T); pool, queue and StagePipeline are theirs to use | opt-tv |
| Bluetooth LE | `ble` | 8/4 | 0.235/0.148 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Iridium | `iridium` | 20/10 | 0.233/0.128 | B | already has a worker thread | already staged |
| GOES HRIT | `goes` | 4.8/2.4 | 0.232/0.179 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| LoRa (raw) | `loraraw` | 4/2 | 0.216/0.187 | C | SF search shares one dechirp (left) | - |
| Spectrum explorer | `specx` | 20/10 | 0.205/0.111 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| ANT+ sensors | `ant` | 8/4 | 0.194/0.134 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Globalstar | `globalstar` | 20/10 | 0.182/0.094 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| HD Radio | `hdr` | 4/2 | 0.182/0.063 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Airband | `airband` | 4/2 | 0.176/0.099 | C | channels share one tuner state (left) | - |
| Mode S uplink | `uplink` | 20/10 | 0.173/0.074 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| Elektro-L | `elektro` | 4.8/2.4 | 0.162/0.157 | B | heavy decode inside the demod loop, no clean cut: left (see notes) | possible 1.3-1.8x; not worth the latency/risk |
| HF beacons | `hfbeacon` | 20/10 | 0.142/0.080 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| DAB | `dab` | 20/10 | 0.128/0.100 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| GSM-R | `gsmr` | 10/5 | 0.123/0.067 | B | already has a worker thread | already staged |
| ISM sensors | `ism` | 4/2 | 0.121/0.065 | C | one decoder bank, cheap (left) | - |
| CDR | `cdr` | 4/2 | 0.110/0.035 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| CODAR / HF ocean radar | `codar` | 4/2 | 0.088/0.045 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| General receiver | `general` | 4/2 | 0.082/0.051 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| D-STAR | `dstar` | 4/2 | 0.081/0.048 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| FLARM | `flarm` | 2/1 | 0.080/0.061 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| MeshCore | `meshcore` | 4/2 | 0.080/0.050 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Radio astronomy | `astro` | 5/2.5 | 0.079/0.037 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Inmarsat Aero | `aero` | 8/4 | 0.079/0.042 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| M17 | `m17` | 4/2 | 0.078/0.056 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| Mesh (LoRa) | `mesh` | 4/2 | 0.073/0.045 | C | LoRa presets share one dechirp (left) | - |
| FM | `fm` | 8/4 | 0.071/0.062 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| P25 | `p25` | 4/2 | 0.069/0.043 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| Inmarsat-C | `inmc` | 4/2 | 0.069/0.043 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| dPMR | `dpmr` | 4/2 | 0.066/0.044 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| ADS-B | `adsb` | 8/4 | 0.065/0.063 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Orbcomm | `orbcomm` | 2/1 | 0.064/0.036 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| ACARS | `acars` | 4/2 | 0.063/0.033 | C | 8 carriers in one detector pass; shared detect/advance (left) | - |
| Radiosonde | `sonde` | 16/8 | 0.060/0.035 | B | already has worker threads | already staged |
| VOR / ILS | `vorils` | 4/2 | 0.058/0.030 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| MeshCom | `meshcom` | 4/2 | 0.057/0.039 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| VOR | `vor` | 4/2 | 0.057/0.029 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| NXDN | `nxdn` | 4/2 | 0.057/0.032 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| TETRA | `tetra` | 4/2 | 0.056/0.028 | B | already has a worker thread | already staged |
| Marine | `marine` | 4/2 | 0.056/0.041 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| ILS | `ils` | 4/2 | 0.055/0.029 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Meteor-M LRPT | `meteor` | 2/1 | 0.055/0.036 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| HF digital | `hfdig` | 4/2 | 0.054/0.040 | B | FT8/FT4 decode already on a worker thread | already staged |
| VDES | `vdes` | 2/1 | 0.054/0.031 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| LoRa APRS | `loraaprs` | 4/2 | 0.054/0.027 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FT2 | `ft2` | 4/2 | 0.054/0.036 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| NB-IoT cells | `nbiot` | 4/2 | 0.053/0.029 | B | already has a worker thread | already staged |
| DRM | `drm` | 4/2 | 0.051/0.035 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Weather fax | `wefax` | 4/2 | 0.051/0.044 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Olivia | `olivia` | 4/2 | 0.049/0.023 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| HF ALE | `ale` | 4/2 | 0.047/0.027 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FT4 | `ft4` | 4/2 | 0.045/0.028 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| RTTY | `rtty` | 4/2 | 0.044/0.026 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| CubeSat telemetry | `cubesat` | 4/2 | 0.044/0.027 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| JS8Call | `js8` | 4/2 | 0.043/0.024 | B | already has a worker thread | already staged |
| PSK31 | `psk` | 4/2 | 0.042/0.024 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| HamDRM | `hamdrm` | 4/2 | 0.042/0.022 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FT8 | `ft8` | 4/2 | 0.042/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| POCSAG | `pocsag` | 4/2 | 0.042/0.025 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| WSPR | `wspr` | 4/2 | 0.041/0.025 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Meshtastic | `meshtastic` | 4/2 | 0.041/0.025 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FreeDV | `freedv` | 4/2 | 0.040/0.027 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| DMR | `dmr` | 4.8/2.4 | 0.039/0.024 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| Hellschreiber | `hell` | 4/2 | 0.038/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| DominoEX | `dominoex` | 4/2 | 0.037/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| JT65 | `jt65` | 4/2 | 0.037/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| MSK144 | `msk144` | 4/2 | 0.036/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| JT9 | `jt9` | 4/2 | 0.036/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Contestia | `contestia` | 4/2 | 0.036/0.025 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| CW | `cw` | 4/2 | 0.035/0.019 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| DSC | `dsc` | 4/2 | 0.035/0.019 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FST4 | `fst4` | 4/2 | 0.035/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| NOAA Weather Radio SAME | `same` | 4/2 | 0.035/0.020 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| THOR | `thor` | 4/2 | 0.035/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| NAVTEX | `navtex` | 4/2 | 0.034/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| MFSK | `mfsk` | 4/2 | 0.034/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| FLEX | `flex` | 4/2 | 0.034/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Q65 | `q65` | 4/2 | 0.034/0.021 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| SSTV | `sstv` | 4/2 | 0.034/0.023 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Pagers | `pager` | 4/2 | 0.034/0.023 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| APRS / Packet | `packet` | 4/2 | 0.033/0.021 | C | WideBank carriers in parallel on the pool (done); single-channel use unchanged | n/a single carrier; N carriers: up to min(N, cores)x on the carrier work |
| Multitone Paging | `multitone` | 4/2 | 0.030/0.016 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| LBJ | `lbj` | 0.96/1.92 | 0.026/0.018 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| UAT (978 MHz) | `uat` | 4/2 | 0.024/0.015 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| AIS | `ais` | 4/2 | 0.022/0.015 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Signal workbench | `wb` | 4/2 | 0.015/0.009 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| NDB | `ndb` | 0.512/0.256 | 0.009/0.008 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Time signals | `time` | 0.256/0.512 | 0.007/0.006 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| TETRAPOL | `tetrapol` | 0.25/0.5 | 0.007/0.005 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
| Datawell Buoy HF Link | `datawell` | 0.25/0.5 | 0.006/0.005 | A | single thread: dispatch + queue latency would cost more than it saves | none (0) |
## 4. What was applied (measured)

The Mac was under heavy load from other jobs (load average 16-36 on 10 cores) during all measurements; CPU seconds are the stable number, wall times only indicate the direction. "Before" = `DECT2_THREADS=0` (the sequential path, which is the old code path) or the integration-branch binary.

| Change | Before | After | Output |
|---|---|---|---|
| WideBank (DMR, P25, NXDN, D-STAR, M17, dPMR, pager, APRS banks): the live carriers' Channelizer + receiver run in `parallelFor`; the receivers' log lines are captured per carrier and replayed in carrier order | 5 DMR carriers, 6 s of 2.4 Msps: 0.80 s wall, 0.79 s CPU | 0.32 s wall (2.4x), 0.95 s CPU (+20%: wake-ups, cold caches) | carriers table, merged call log and log lines identical (`test_dmr_wide_threads`); `dmr_wide`, `nxdn_wide`, `pager_wide`, `packet_wide` unchanged |
| HFDL: the in-band channels (filter chain, search, burst decoder) in parallel; the hits for the shared lists (`serial`, messages, aircraft, stations, counters, log) are held per channel and applied in channel order after the join; the three lazy tables (`hfdlPreambleRef`, interleaver, Viterbi table) and the coarse reference are now thread-safe once-initialised | `bench_modes hfdl` 2 Msps: cost 0.286, wall/signal 0.27 | cost 0.360, wall/signal 0.10 (2.7x faster, +26% CPU) | 32 messages with order, serials, times, counters and channel table identical (`test_hfdl_threads`); `hfdl_rx` 175 failures before and after (known, same), `hfdl_engine` same output before and after |
| DVB-T2 LDPC: the codewords of a frame on `ThreadPool::background()` instead of threads created per frame | bench t2: cost 0.300, 5 items | cost 0.284, 5 items | same |
| Thread names on the analysis, spectrum, secondary threads | - | `onair-analysis`, `onair-spectrum`, `onair-rxN` | - |
| Spectrum off the decode path, secondary receivers on their own threads | already the case (section 1), no change | | |

## 5. Left single-threaded, and why

- Class A (cost < 0.15, 80 of 117 rows): a hand-off costs a lock and a block of latency and there is nothing to win.
- Modes that already have their own decode worker (wisun, tetra, sonde, nr, nbiot, js8, iridium, gsmr, FT8/FT4, T2 frames, DVB-S2 symbol thread): nothing to add; they can move to `StagePipeline` when they are next touched (the queue rules are the same).
- VDL2: same shape as HFDL (channels in `feed`), but its burst decoder feeds the tuning estimate back (`pendingTune`) and updates aircraft / stations inside the decode; the deferred-commit pattern of HFDL applies, not done in this pass to keep the output provably identical.
- ACARS, airband, ISM, mesh, LoRa, Wi-Fi, HaLow: the per-channel work shares one front end or one dechirp per block; the cost is in that shared part, so channel jobs would not help. Wi-Fi at 20 Msps is bound by the channel filter bank (an `opt-spectrum` / front-end job).
- D (TV, wideband): opt-tv's pipelines; they can use `ThreadPool::shared()`, `StagePipeline` and `ReorderBuffer` directly.
- The analysis thread itself is not split: the contract (it never waits for the interface) and the in-order feed of one receiver make it the single sequential stage by design.

## 6. Rules for the next one who threads a mode

1. Profile first (`docs/optimising-a-mode.md`); thread only a stage that costs more than ~30 percent of the mode.
2. Jobs write only their own slot; merge in index order (or `ReorderBuffer`); log lines go through a per-slot buffer.
3. Anything lazily initialised inside a job (`static` tables) must be `std::call_once` or a magic static.
4. Run it with `DECT2_THREADS=0` and with the default; the output must be byte-identical; run the mode's tests under `-DDECT2_SANITIZE_THREAD=ON`.
5. Comment every thread and queue with: who produces, who consumes, what happens when full, who joins.
