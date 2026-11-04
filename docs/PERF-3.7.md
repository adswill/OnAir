# Performance 3.7: per-mode CPU baseline and what was changed

Measured on branch feat37-perf (base integ-37), Release build (-O3, Apple silicon arm64, Accelerate), with `bench_modes --seconds 10 --rates default`
(tests/bench/bench_modes.cpp: every mode's built-in synthetic signal through the real Engine path, file source, no sound, no pacing).
**Cost** = CPU seconds (all threads) per second of signal = cores used per real-time second; real-time factor = 1/cost.
The Mac was heavily loaded by other jobs (load average 20 to 77) during the runs: CPU time stays comparable but moves by 5 to 15 % between identical runs (cache and memory contention). Read small differences as noise.

## Baseline (before any 3.7 change)

| # | Mode | Rate (Msps) | Cost (cores) | RT factor | Startup CPU (s) | Peak RSS (MB) | Threads | Busiest thread | Items | Status |
|--:|---|--:|--:|--:|--:|--:|--:|--:|--:|---|
| 1 | DVB-C (`dvbc`) | 10 | 1.2363 | 0.8 | 0.00 | 462 | 33 | 55% | 219429 | ok |
| 2 | ATSC 1.0 (`atsc`) | 8 | 0.8705 | 1.1 | 0.04 | 360 | 34 | 42% | 126624 | ok |
| 3 | DTMB (`dtmb`) | 10 | 0.7503 | 1.3 | 0.00 | 324 | 37 | 42% | 46219 | ok |
| 4 | ISDB-T (`isdbt`) | 8 | 0.6462 | 1.5 | 0.00 | 445 | 42 | 50% | 100929 | ok |
| 5 | Wi-Fi networks (`wifi`) | 20 | 0.6293 | 1.6 | 0.04 | 407 | 33 | 87% | 2493 | ok |
| 6 | Wi-SUN FAN (`wisun`) | 20 | 0.5037 | 2.0 | 0.09 | 374 | 34 | 63% | 1183 | ok |
| 7 | DVB-T (`dvbt`) | 9.143 | 0.4891 | 2.0 | 0.13 | 343 | 41 | 65% | 143246 | ok |
| 8 | Analog TV (`atv`) | 10 | 0.4754 | 2.1 | 0.18 | 337 | 36 | 89% | 489 | ok |
| 9 | DVB-S/S2 (`dvbs`) | 10 | 0.4660 | 2.1 | 0.01 | 365 | 40 | 40% | 1493 | ok |
| 10 | ATSC 3.0 (`atsc3`) | 10 | 0.4170 | 2.4 | 0.00 | 605 | 40 | 38% | 220 | ok |
| 11 | GNSS (`gnss`) | 4 | 0.4008 | 2.5 | 0.02 | 291 | 32 | 91% | 29 | ok |
| 12 | HRPT / AHRPT (`hrpt`) | 6 | 0.3295 | 3.0 | 0.06 | 289 | 32 | 88% | 2773 | ok |
| 13 | Spectrum analyser (`specan`) | 20 | 0.2722 | 3.7 | 0.03 | 366 | 33 | 71% | 193410 | ok |
| 14 | 5G NR cells (`nr`) | 20 | 0.2362 | 4.2 | 0.10 | 399 | 33 | 36% | 2 | ok |
| 15 | Zigbee / 802.15.4 (`zigbee`) | 4 | 0.2337 | 4.3 | 0.07 | 268 | 32 | 88% | 2946 | ok |
| 16 | Cell towers (`cell`) | 20 | 0.2308 | 4.3 | 0.11 | 463 | 33 | 36% | 9 | ok |
| 17 | VDL Mode 2 (`vdl2`) | 2 | 0.2131 | 4.7 | 0.06 | 260 | 32 | 92% | 31 | ok |
| 18 | DVB-T2 (`t2`) | 9.143 | 0.1910 | 5.2 | 0.14 | 357 | 33 | 82% | 6 | ok |
| 19 | Bluetooth LE (`ble`) | 4 | 0.1525 | 6.6 | 0.04 | 268 | 32 | 82% | 2 | ok |
| 20 | LoRaWAN (`lorawan`) | 2 | 0.1339 | 7.5 | 0.05 | 293 | 32 | 85% | 11 | ok |
| 21 | GOES HRIT (`goes`) | 2.4 | 0.1318 | 7.6 | 0.05 | 258 | 32 | 83% | 567 | ok |
| 22 | DAB (`dab`) | 10 | 0.1182 | 8.5 | 0.09 | 318 | 33 | 80% | 1252 | ok |
| 23 | HFDL (`hfdl`) | 2 | 0.1118 | 8.9 | 0.01 | 267 | 40 | 18% | 17 | ok |
| 24 | Iridium (`iridium`) | 10 | 0.1099 | 9.1 | 0.09 | 316 | 33 | 35% | 939 | ok |
| 25 | Spectrum explorer (`specx`) | 10 | 0.0903 | 11.1 | 0.09 | 309 | 33 | 59% | 2 | ok |
| 26 | VDL Mode 4 (`vdl4`) | 2 | 0.0848 | 11.8 | 0.05 | 252 | 32 | 78% | 16 | ok |
| 27 | ADS-B (`adsb`) | 4 | 0.0706 | 14.2 | 0.04 | 266 | 32 | 72% | 770 | ok |
| 28 | FM (`fm`) | 4 | 0.0545 | 18.4 | 0.06 | 269 | 32 | 69% | 2 | ok |
| 29 | ACARS (`acars`) | 2 | 0.0336 | 29.7 | 0.07 | 252 | 32 | 63% | 63 | ok |
| 30 | DMR (`dmr`) | 2.4 | 0.0269 | 37.2 | 0.08 | 259 | 32 | 51% | 530 | ok |
| 31 | AIS (`ais`) | 2 | 0.0206 | 48.6 | 0.03 | 256 | 32 | 54% | 40 | ok |

Modes not in the table fit under 0.07 cores (ACARS 0.034, AIS 0.021, DMR 0.027, FM 0.055, ADS-B 0.071).
Wide spectrum / waterfall: `specan` at 20 Msps costs 0.27 cores, `specx` at 10 Msps 0.09 (rows above).

## Where the top rows spend their time (`sample`, 6 s of steady state)

* DVB-C (1.24): receiver thread, `onSymbol` (28-tap half-symbol equaliser and its LMS update) 1912 samples, `process` 1314 (mixer, matched filter), `smooth` 865. The equaliser loops are already fully unrolled NEON (checked in the disassembly: `ld2`, `fmla.4s`), so no intrinsics would beat what the compiler emits.
* DVB-T (0.49): `fecp::viterbiRange` 1662 of about 3000 busy samples (the K = 7 Viterbi, NEON already), then demap 306.
* ATSC 1.0 (0.87): polyphase matched filter, equaliser, field decoder; all vectorised paths, spread over several threads.
* Wi-Fi (0.63): per-sample front end (`input`, `detect`, `feedOne`): a recurrence-bound scalar chain, not vectorisable without changing the float rounding.
* Analog TV (0.48): `extractPicture` 470, `temporalFilter` 360, `Mixer::run` (sound carriers) 314.

## Changes

### 1. Analog TV temporal noise reduction (core/include/dect2/atv/atv_blend.h)

The per-byte `std::abs`, double division and branch were replaced by a 256-entry weight table built once per picture; the arithmetic on each byte is unchanged.
`tests/atv/test_atv_blend.cpp` compares it with the old per-byte formula on all 65536 (c, p) pairs for 36 (k, noise) combinations: byte for byte equal.
Micro-benchmark (720x576 picture, single thread CPU, 50 pictures, run twice): **2.07 and 2.01 ms -> 1.34 and 1.35 ms per picture (-35 %)**; at 25 pictures per second that is about 0.017 cores. The whole-mode `bench_modes atv` figure (0.475 before, 0.486 and 0.514 after) is inside the load noise, so no whole-mode gain is claimed.

## Checked and left alone (no gain available without changing results)

* DVB-C equaliser: already vectorised (see above).
* DVB-T / T2 / S / S2 / C Viterbi and LDPC: NEON kernels exist; Rust Viterbi is 4 % slower (documented).
* Wi-Fi, ATV mixers: sequential phasor recurrences; splitting them into parallel phasors changes the float rounding, so the decoded values would no longer be bit-identical.
* Idle redraw rate, hidden-panel work, waterfall row upload, App Nap: already done in 0.3.0 (docs/PERFORMANCE.md).

## Not measured here

* Idle UI cost and startup time: `bench_ui_power` and the GUI need the app target, which was not built on this loaded machine (the load average was 77). The figures in docs/PERFORMANCE.md (0.3.0) are the last measured ones; they were not re-measured for 3.7.
* Multi-VFO CPU, and ACARS/VDL2/HFDL with several channels: only the single-channel rows above were run.

## How to repeat

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF && cmake --build build -j3 --target bench_modes
build/bench_modes --mode t2,dvbt,dvbs,dab,fm,adsb,acars,vdl2,hfdl,ais,specan,specx,dvbc,atsc,atsc3,isdbt,dtmb,atv --seconds 10 --rates default --out /tmp/before
build/bench_modes --gen atv 10e6 20 /tmp/atv.cs8; build/bench_modes --run atv 10e6 20 /tmp/atv.cs8   # one mode, then `sample <pid> 6`
```
