# QA 4.0: every mode through the Engine

Workers `feat40-qa` (round 1) and `feat40-qa2` (round 2), 2026-11-11/12. All of it runs on simulated signals: each mode's own generator and transmitter. No recording was used.
Run on GitHub (`quick.yml`, Linux, 3 cores): `qa_sweep_a` .. `qa_sweep_i` (the sweep in 8 shards), `qa_switch`, and a macOS run of `qa_switch` under
AddressSanitizer + UndefinedBehaviorSanitizer. The 3.9 sweep and its method are in `docs/QA-SWEEP-3.9.md`; this page is what 4.0 added and found.

## What the sweep checks now

| check | how | new in 4.0 |
|---|---|---|
| clean, +2 kHz +50 ppm, -2 kHz -50 ppm, 12 dB in the signal band, noise only, the mode's own transmitter | as in 3.9: generator -> file -> a fresh Engine in its own process | no |
| **rate** | the generator again at **1.37 x** the mode's sample rate (a non-integer ratio) and, for modes whose bandwidth is a filter, a **1.3 x** custom bandwidth, through a fresh Engine. DVB-T and DVB-T2 keep their channel bandwidth (it is the symbol rate there) | yes |
| **noise only, modes without a generator** | the modes that have no test signal (about 40) now get 3 s of white noise through a fresh Engine: no crash, no hang, no decode | yes |
| **rate + offset, rate + noise** (round 2) | the files of the 1.37 x run that were already written (+2 kHz +50 ppm, and noise alone) are run through a fresh Engine too: columns `rpos` and `rnoise`. A mode's known failure for `pos`, `noise` or `rate` also covers its new-rate column | yes |
| verbose runs (`QA_SWEEP_VERBOSE=1`, round 2) | the engine log and last report as before, plus the signal's power and 99 % band, what the engine took in (samples, share of non-zero ones, rms, the strongest slices of its spectrum) and vdl3 / ltpos internals | yes |
| the cell search drains (round 2) | after the file ends the sweep waits for a cell search that began before the end (1 to 5 s at 20 Msps on a CI runner), as it does for JS8 and KAIT | yes |
| lock-only modes | a lock without a block in a 3 s file gets the 8 s run (the satellite carrier trackers need 2.5 s of line) | yes |
| **`qa_switch`** | one Engine, every mode of the list in turn (stop, set the mode, start on its test signal, 0.4 s of signal), then the list again backwards; each step under a 90 s watchdog. Checks: no hang, the first report is the new mode's, samples were processed, threads and resident memory after the second pass not grown (threads +4, memory 25 % + 200 MB) | yes |
| `qa_sweep_sel` | reruns the modes named in `QA_MODES` (`QA_RATEMUL`, `QA_BWMUL`, `QA_DIRECT`, `QA_SWEEP_VERBOSE` change the run): for a few rows without the 40-minute shards | yes |

The memory and thread limits are loose on purpose: a receiver kept alive per mode is normal (the Engine keeps one of each).

## Bugs found and fixed

| bug | evidence | fix | result |
|---|---|---|---|
| After a mode switch `latestRx()` returned the **previous mode's last report** (standard, counters, SNR) until the new receiver published | `qa_switch`: 167 of 300 steps began with the old mode's standard | `Engine::start()` clears the stored report (its sequence number keeps growing, so the interface still sees every new one) | `qa_switch` passes: 300 modes x 2 passes, 0 problems (Linux and macOS) |
| **HF ID (milhf)** counted every 2 s analysis as a decoded block, also of noise | noise-only run: FALSE decode | counts only analyses that found a signal | noise run passes |
| **End of train (eot)** accepted a frame with two repaired bits behind a sync with an error too (three errors); noise alone passes that about once per 100 s of signal | noise-only run: FALSE decode | the frame is dropped in that case | noise run passes |
| jt6m: one false ping in 32 s of noise | noise run FALSE | round 1: not fixed (stricter limits turned the -50 ppm run from weak to fail) | fixed in round 2, below |

**Round 2 (feat40-qa2)**

| bug | evidence | fix | result |
|---|---|---|---|
| **vdl3, ltpos: nothing decoded through the Engine**, from a file or the synthetic source | the engine took in the signal (82 % non-zero samples) but both receivers stayed idle: the synthetic source rounds samples to 8 bits, so a noise floor below one step (7e-4 and 3e-4) was **exact zero between bursts**; vdl3 needs `floor > 0` for its squelch, ltpos a median block power `> 0` for its floor (state 1 "measuring the noise floor" for ever) | floors of at least 1e-10 (vdl3) and 1e-12 (ltpos); any recording padded with digital silence would have hit the same | unit tests with 8-bit samples: vdl3 75 of 75 bursts (**0 of 75** before), ltpos 14 sferics and a valid floor (**0** before); sweep: vdl3 507 blocks clean, ltpos lock in every column |
| **vdl3, fbr: the test signal was 100 / 30 kHz off the channel the engine listens to** | vdl3's generator ignored the mode's `tuneOffsetHz` (the other generators put the channel that far below the centre; the sweep and the live Engine mix it back); fbr's did the same: its spectrum sat at +31 kHz and it locked only through the decimation's alias (57 dB clean, **no carrier at 12 dB**: acquisition statistic 12 - 14 dB against 17) | both generators use `-tuneOffsetHz`; the unit tests' receiver offsets follow | vdl3: all conditions pass; fbr: lock at 8 dB in 1.2 kHz in the generator alone, **all nine columns pass** (snr was FAIL, rate weak) |
| **umts: no cell at any rate but 7.68 Msps** | the synthetic source labelled its 7.68 Msps samples with the requested rate: at 1.37 x the signal was time-scaled | the signal is read at the requested spacing (4-point Lagrange) | unit test: both cells at 7.68, 10.52 and 15.36 Msps; sweep `rate`, `rpos`, `rnoise` pass |
| **cell: the transmitter's file and the 1.37 x rate "decoded nothing"** | the cell search of the last block finished after the file ended (the log shows PCI 123 MIB found, the report was taken before) | the sweep waits for a running search; (no change to the receiver) | `tx` and `rate` pass |
| **jt6m: a false ping in 1 of 6 runs of 32 s of noise** | the best of ~900 sync hypotheses in a 6 s window scores 4 .. 6 in noise, the gate was 4 (349 of 1200 windows passed it) | gate 5.5 | decoder alone: **0 of 300** noise runs (57 of 300 before); 1.5 s ping at -12 dB 56 of 60 (58), 0.3 s ping at -6 dB 25 of 60 (53, about 19 % of them noise); continuous -17 dB unchanged; new unit test: 50 noise runs, the old gate gives 5. The -50 ppm case that stopped the round-1 attempt is unaffected (a signal, not a ping) |
| **ammon, fmmon, uvb76: decodes on white noise** (new modes, noise-only run) | AM monitor: "carrier present, modulation 74 %" (mean envelope > 1e-6 is a carrier); FM monitor: 4.5 kHz of "19 kHz pilot" from discriminator noise; UVB-76: a buzz | AM monitor and UVB-76 need a **carrier line** (5 FFT bins holding > 1.2 % of the channel's power); the pilot must stand 3 x over the bands either side | noise through the receiver (sigma 0.02 .. 0.5): 0 decodes (3, 1, 1 before); the monitors' own tests pass unchanged; new noise tests |
| **cis36, cishdr, cis128, cis3000** (identification only) counted nothing | the sweep could not tell an identification from silence | the lock flag is true while the analyser's "identified" line names a modulation | clean / pos / snr / rate columns `lock`, noise `pass` |
| flarm: a pointer past the end of an array in a sort bound | UBSan under the engine tests: `flarm_rx.cpp:297: index 64 out of bounds for type 'int[63]'` | `order + ((1 << L) - 1)` | no report |
| Windows macro names as variables | `near` / `far` in loraraw_survey, x40_plnoise, test_navtex_table, test_vor_navaids | renamed | Windows job passes; scan of all 177 4.0 source files: no other `small interface IN OUT ERROR DELETE CONST min max`, no unguarded POSIX header in a test |

No hang, no crash, no `restart refused` in any of the ~340 modes x 6 conditions on Linux, and none in the 300-mode switch test on Linux, macOS or under the sanitizers.

## Sanitizers

Round 1: `qa_switch` built with `-DDECT2_SANITIZE=ON` (AddressSanitizer + UndefinedBehaviorSanitizer, macOS runner): 300 modes forward and backward, **no report**.

Round 2: **48 engine unit tests** (channel, secondary_modes, multi_rx, dual_engine, decode_shift, radio_mode_glue, custom_capture, iq_modes, replay, time_machine, iq_record, remote_api, web_safety, modes, mode_numbers, atv_switch, mode_rate_sweep, chosen_rate_reaches_radio, gain, gain_opt, tx, tx_quality_a, custom_bandwidth_all, kaal, dab_dmb, multitv, and one engine test each of DMR, P25, ADS-B, AIS, ACARS, VDL2, DVB-S / C, ISDB-T, ATSC 3.0, DRM, HF digital RTTY, JS8, PSK, packet, pager, NXDN, TETRA, cell, mesh) built with `-DDECT2_SANITIZE=ON -DDECT2_SANITIZE_RECOVER=ON` on the macOS runner (all reports of a run, 2 h 30 min):

- **one finding**: `flarm_rx.cpp:297` (above), fixed.
- **no heap error, no use after free, no overflow** (AddressSanitizer). The macOS leak checker is not available: no leak statement.
- 12 of the 48 failed, all but one on a time or CPU budget that the sanitizer's slowdown (3 - 10 x) breaks: "the generator makes 0.15 s of signal per CPU second: far from real time" (tx_quality_a, cell, fpv), "one channel needs 4 cores" (multitv_wide), "0 fast / turbo decodes", "no cell identified" (cell_engine), JS8 and VDL2 frame counts, DVB-S and analog TV report timing, the time machine's saves, the gain tests' ADC ranges, "engine is not processing signal" at 3.5 Msps (custom_bandwidth_all, radiojove). The one that is not a timing: `mode_numbers` could not read the poll slot `a - 1` (a new macro line in `engine_telemetry.cpp`); `integ-40` already carries the test's fix. They were not rerun without the sanitizer in this task.

## Open findings (listed in `tests/qa/qa_known.inc`)

| mode | what | evidence | next step |
|---|---|---|---|
| ax100m6 (845), u482c (854), ngham (851) | 12 dB in the signal's own band does not decode: a **sensitivity gap**, not a bug | the cubesat receiver (one FM front, channel filter 12 kHz pass / 20 kHz stop for every framing) decodes half of the frames at **9.0 dB** (AX100 M6), **10.2 dB** (U482C), **7.3 dB** (NGHam) C/N in 25 kHz (generator alone, 12 s, 5 SNRs); the sweep's 12 dB in the 99 % band (10.7, 10.7 and 5.9 kHz) is 8.3, 8.3 and 5.7 dB in 25 kHz: 0.7, 1.9 and 1.6 dB too little. 9.0 dB C/N in 25 kHz is Eb/N0 13.2 dB for 9600 baud: about 3 dB from what a 9k6 FSK demodulator with RS needs | a narrower channel filter with the 4.0 `FmFront::setAfc` carrier follow-up (as sonde and EDACS got in round A2). Measured in a copy (generator alone, 10 s per point, 10 frames): the shared filter at 9 / 15 kHz (pass / stop) instead of 12 / 20 kHz moves the 50 % point of AX100 M6 by 0.7 dB, NGHam by 1.0 dB, AFSK 1200 by 0.8 dB, G3RUH by 0.3 dB, U482C by 0 dB: modest; not applied because the same front serves Doppler-shifted passes of every framing and the AFC margin was not measured |
| tetra (48) | the transmitter's file decodes nothing on a loaded runner; passes on a quiet one (3 of 5 CI runs fail) | the engine log says "searching for base stations" only; the input has the same spectrum as the test signal (-78 kHz 86 %, -109 kHz 14 %); the receiver alone and the Engine (3.9 tree, real time too) decode the same file on the Mac, 131 blocks per 8 s | a log line from the carrier search (floor, humps found) when nothing is found, then a run on a loaded runner |
| msk144 (27) | the same for its transmitter, 1 of 3 runs | | |
| globalstar (64) | `rpos`: at 13.7 Msps (1.37 x) a +2 kHz / +50 ppm file decodes 0 blocks (default rate: 3) | in the receiver alone, generator -> the sweep's 4-point Lagrange resampler -> receiver: **any** clock error (5 ppm) at a sample rate other than 6 and 10 Msps gives 0 blocks (8, 9, 10.5, 11, 12, 13.7, 14, 20 Msps tried; 10 Msps with 50 ppm decodes; a generator SNR of 30 dB does not change it). Without the resampler the same file decodes. With it no channel ever goes off: the receiver's floor never updates and no event closes (the unresampled run closes one every 0.8 s) | find what the resampler changes in the burst gaps of the second channel (the generator keys it 0.3 s of every 0.8 s) at those rates: a sweep artefact or a receiver weakness; a real radio has a clock error at every rate |
| nfc, noaaapt, noaagac, noaadsb, meteorhrpt, goesmdl, specan | count picture lines or measurements without a check: noise gives "decodes" | noise FALSE (known since 3.9) | a signal-present test before counting |
| cdma2000, tdscdma, ft8, ft4, iridium, olivia, thor, rtty (`weak`) | fewer than half of the clean run's blocks with +-2 kHz / +-50 ppm | table below | read, not investigated |
| fm, dvbt, atsc, dtmb, dvbc, dvbc2, ils, argos, vdl4, multitone, cb, frs, pmr446, mdc1200 | 12 dB in the signal band is below their threshold | table (`threshold` in `qa_known.inc`) | lane A of the sensitivity work |

Closed in round 2 (they were in this table): vdl3, ltpos, cell and umts at 1.37 x, cell transmitter, sonde 12 dB (passes since the sensitivity work), nbiot 12 dB (passes), jt6m false ping, cis3000 / cis36 / cishdr / cis128.

Every other row of the table below passes, or matches a known failure with its reason (HF audio modes at +-2 kHz, 12 dB below a modulation's threshold, a signal longer than the short run).

## What was validated on simulated signals only

Everything above. A mode that passes here decodes **its own generator's** signal; that proves the engine path (tuning, resampling, start / stop / switch, results) works, not that the decoder is right for real signals.

## Per-mode result (Linux)

Round 2: the eight shards `qa_sweep_a` .. `qa_sweep_i` run in parallel on eight runners (3 cores each; verbose, so the table of a passing shard is printed too), 356 modes; the rows of the modes fixed after that (fbr, ammon, fmmon, uvb76) and the rows of the modes changed in round 2 are from the reruns of `qa_sweep_sel` that follow it (`globalstar msk144 ammon fmmon uvb76 tetra fbr vdl3 ltpos cell umts jt6m cis36 cishdr cis128 cis3000 nbiot sonde ax100m6 u482c ngham`: 21 modes, 0 unexpected failures).
Columns: clean, +2 kHz +50 ppm (`pos`), -2 kHz -50 ppm (`neg`), 12 dB in the signal band (`snr`), noise only, **rate** (1.37 x rate + custom bandwidth), **rpos** (rate with +2 kHz +50 ppm), **rnoise** (rate, noise only), tx. `-` = not run (nothing in the clean run, or no transmitter), `lock` = the mode only measures or locks, `weak` = fewer than half of the clean run's blocks. `FAIL` / `FALSE` cells are known (`tests/qa/qa_known.inc`) or in the open findings above. A `FAIL` of `rpos` for a mode whose `pos` fails is the same reason (HF: +-2 kHz is outside an SSB window).

| mode | std | clean | pos | neg | snr | noise | rate | rpos | rnoise | tx | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| t2 | 1 | lock | lock | lock | lock | pass | lock | lock | pass | pass |   |
| dvbt | 2 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| atsc | 3 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| dab | 4 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| atsc3 | 5 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| isdbt | 6 | pass | pass | pass | weak | pass | pass | pass | pass | pass |   |
| fm | 7 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| dvbs | 8 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| dtmb | 9 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| atv | 10 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| dmr | 11 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| drm | 12 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| adsb | 13 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| gnss | 14 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| sonde | 15 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ais | 16 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| dsc | 17 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| marine | 17 | FAIL | - | - | - | pass | - | - | pass | pass |  (real time) |
| navtex | 17 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| wefax | 17 | lock | lock | FAIL | lock | pass | lock | lock | pass | - |   |
| acars | 18 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| inmc | 19 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| aero | 20 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| iridium | 21 | pass | weak | weak | pass | pass | pass | weak | pass | pass |   |
| loraaprs | 22 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| lorawan | 22 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| mesh | 22 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| meshcom | 22 | pass | pass | pass | pass | pass | pass | pass | pass | - |  generator sends silence |
| meshcore | 22 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| meshtastic | 22 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| hdr | 23 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| cdr | 24 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| flex | 25 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| pager | 25 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| pocsag | 25 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| packet | 26 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| contestia | 27 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| dominoex | 27 | pass | pass | FAIL | pass | pass | pass | pass | pass | pass |   |
| freedv | 27 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| fst4 | 27 | FAIL | - | - | - | pass | - | - | pass | FAIL |  (real time) |
| ft2 | 27 | pass | pass | weak | pass | pass | pass | pass | pass | pass |   |
| ft4 | 27 | pass | weak | weak | pass | pass | pass | weak | pass | pass |   |
| ft8 | 27 | pass | weak | weak | pass | pass | pass | weak | pass | pass |  (real time) |
| hamdrm | 27 | pass | FAIL | FAIL | FAIL | pass | pass | FAIL | pass | pass |  (saved bytes) |
| hell | 27 | pass | pass | FAIL | pass | pass | pass | pass | pass | pass |   |
| hfdig | 27 | pass | FAIL | weak | pass | pass | pass | weak | pass | - |   |
| jt4 | 27 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| jt65 | 27 | FAIL | - | - | - | pass | - | - | pass | FAIL |  (real time) |
| jt9 | 27 | FAIL | - | - | - | pass | - | - | pass | FAIL |  (real time) |
| mfsk | 27 | pass | pass | FAIL | pass | pass | pass | pass | pass | pass |   |
| msk144 | 27 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | FAIL |   |
| olivia | 27 | pass | weak | FAIL | pass | pass | pass | weak | pass | pass |   |
| q65 | 27 | FAIL | - | - | - | pass | - | - | pass | FAIL |  (real time) |
| rtty | 27 | pass | FAIL | weak | pass | pass | pass | weak | pass | pass |   |
| sstv | 27 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| thor | 27 | pass | weak | FAIL | pass | pass | pass | weak | pass | pass |   |
| wspr | 27 | FAIL | - | - | - | pass | - | - | pass | FAIL |  (real time) |
| airband | 28 | pass | pass | pass | pass | pass | pass | pass | pass | lock |   |
| vdl2 | 29 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ism | 30 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ndb | 31 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ils | 32 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| vor | 32 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| vorils | 32 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| m17 | 33 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| cell | 34 | pass | pass | pass | pass | pass | pass | pass | pass | pass |  (real time) |
| dstar | 35 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| cw | 36 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| hfdl | 37 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| psk | 38 | pass | pass | weak | pass | pass | pass | pass | pass | pass |   |
| js8 | 39 | pass | pass | weak | pass | pass | pass | pass | pass | pass |   |
| nxdn | 40 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| gsmr | 41 | pass | pass | pass | pass | pass | pass | pass | pass | pass |  (real time) |
| dvbc | 42 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| specx | 43 | lock | lock | lock | lock | pass | lock | lock | pass | lock |   |
| specan | 44 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | pass |   |
| uplink | 45 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| orbcomm | 46 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| dpmr | 47 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| tetra | 48 | pass | pass | pass | pass | pass | pass | pass | pass | FAIL |   |
| mac | 49 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| astro | 50 | lock | lock | lock | lock | lock | lock | lock | lock | lock |   |
| ale | 51 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| p25 | 52 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ble | 53 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| iono | 54 | lock | lock | lock | lock | pass | lock | lock | pass | lock |   |
| qo100 | 55 | lock | lock | lock | lock | pass | lock | lock | pass | lock |   |
| wifi | 56 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| am | 57 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| general | 57 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| rid | 58 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| nr | 59 | pass | pass | pass | pass | pass | weak | weak | pass | pass |  (real time) |
| time | 60 | lock | FAIL | lock | lock | pass | lock | FAIL | pass | - |   |
| meteor | 61 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| goes | 62 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| cubesat | 63 | pass | pass | pass | weak | pass | pass | pass | pass | pass |   |
| globalstar | 64 | pass | pass | pass | pass | pass | pass | FAIL | pass | pass |   |
| vdes | 65 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| hrpt | 66 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| zigbee | 67 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| flarm | 68 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| hfbeacon | 69 | lock | FAIL | FAIL | lock | pass | lock | FAIL | pass | lock |   |
| dji | 70 | pass | pass | pass | pass | pass | pass | pass | pass | pass |  (real time) |
| nbiot | 71 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| halow | 72 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| loraraw | 73 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| codar | 74 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| wb | 75 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| datawell | 76 | lock | lock | lock | lock | pass | lock | lock | pass | lock |   |
| multitone | 77 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| same | 78 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ant | 79 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| dect | 80 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| tetrapol | 81 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| elektro | 82 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| lbj | 83 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| vdl4 | 84 | pass | pass | pass | FAIL | pass | pass | pass | pass | pass |   |
| uat | 85 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| wisun | 86 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| eczas | 87 | pass | FAIL | FAIL | FAIL | pass | pass | FAIL | pass | pass |   |
| wmbus | 88 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| tones | 91 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| sarsat | 94 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| mt63 | 97 | pass | pass | FAIL | pass | pass | pass | pass | pass | pass |   |
| zwave | 100 | pass | pass | pass | pass | pass | weak | weak | pass | pass |   |
| fpv | 103 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| mscatter | 106 | lock | lock | lock | lock | pass | lock | lock | pass | lock |   |
| cquam | 109 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | pass |   |
| fsq | 115 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| ifkp | 118 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| sitor | 121 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| dgps | 122 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| dme | 123 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| kaal | 124 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| kavb | 125 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| kapp | 126 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| kaft | 127 | pass | FAIL | pass | pass | pass | pass | FAIL | pass | pass |   |
| eloran | 128 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| mpt1327 | 129 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| fusion | 130 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| ltr | 131 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| edacs | 132 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| kait | 133 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| kaws | 135 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) generator sends silence |
| ardop | 142 | pass | FAIL | pass | pass | pass | pass | FAIL | pass | pass |   |
| goesdcs | 143 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| modeac | 144 | pass | pass | pass | weak | pass | pass | pass | pass | pass |   |
| bletracker | 145 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| satbeacon | 146 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| milhf | 147 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| klw | 148 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| tdcs | 149 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| nfc | 150 | pass | pass | pass | weak | FALSE | pass | pass | FALSE | pass |   |
| sbas | 152 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| asterix | 153 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) generator sends silence |
| argos | 154 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| sigfox | 155 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| isdbs | 156 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| marinevhf | 157 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| horus | 158 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| p25p2 | 182 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| lorawanwide | 202 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| lorasat | 203 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| pskd | 204 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| pactor | 205 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| esubghz | 206 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| roshf | 207 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| ssdv | 208 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| ebicab | 209 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| inmsat | 210 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| iptv | 211 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) generator sends silence |
| nrf24 | 212 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| atis | 213 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| shortwave | 214 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| gk2alrit | 232 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| gk2ahrit | 233 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fy4lrit | 234 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fy4ahrit | 235 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fy2svissr | 236 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fy3ahrpt | 237 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fy3mpt | 238 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| pactor2 | 239 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |  (real time) |
| pactor3 | 240 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |  (real time) |
| pactor4 | 241 | lock | FAIL | FAIL | lock | pass | lock | FAIL | pass | - |   |
| selcal | 242 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| sensors | 243 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| sferics | 245 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| thread | 246 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| bgan | 247 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| noaaapt | 262 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | - |   |
| noaahrpt | 263 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| noaagac | 264 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | - |   |
| noaadsb | 265 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | - |   |
| meteorhrpt | 266 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | - |   |
| elektrordas | 267 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| arktikardas | 268 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| arktikaggak | 269 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| awsl | 270 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| msgraw | 271 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| himawaricast | 272 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| geonetcast | 273 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| goesgrb | 292 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| goesgvar | 293 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| goescda | 294 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| goesmdl | 295 | pass | pass | pass | pass | FALSE | pass | pass | FALSE | - |   |
| gk2auhrit | 296 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| gk2acdas | 297 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| sband | 298 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| geoscan | 299 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| lucky7 | 300 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| spino | 301 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| adsl | 302 | pass | pass | pass | pass | pass | pass | pass | pass | pass |   |
| smartnet | 322 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| tetradmo | 323 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| opensky | 324 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| cb | 342 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| frs | 343 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| pmr446 | 344 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| hamfm | 345 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| hfvoice | 346 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| mdc1200 | 347 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| wmic | 348 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| elrs | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| flysky | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| frsky | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| mavlink | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| rclink | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| reticulum | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| sfhss | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| spektrum | 362 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| btc | 382 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| fanet | 402 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| ogn | 403 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| gbas | 404 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| ldacs | 405 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| aeromacs | 406 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| eot | 407 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| dsrc | 408 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| its | 409 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| cv2x | 410 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| ermes | 411 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| mradar | 412 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| wradar | 413 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| milstd110 | 432 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| stanag4285 | 433 | lock | FAIL | FAIL | lock | pass | lock | FAIL | pass | - |  (real time) |
| arqe | 434 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| arqn | 435 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| autospec | 436 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| spreadhf | 437 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| gtor | 438 | pass | pass | weak | pass | pass | pass | pass | pass | - |   |
| efr | 439 | pass | pass | weak | pass | pass | pass | pass | pass | - |   |
| piccolo | 440 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| coquelet | 441 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| qrss | 462 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| dfcw | 463 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| fsk441 | 464 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| iscat | 465 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| pi4 | 466 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| throb | 467 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| umts | 482 | pass | pass | pass | pass | pass | pass | pass | pass | - |  (real time) |
| supersid | 502 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| riometer | 503 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| pulsar | 504 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| erminaz | 512 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| xb_npp | 532 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_jpss2 | 533 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_aqua | 534 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_terra | 535 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_metopd | 536 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_metopsg | 537 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_landsatx | 538 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_landsats | 539 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_probas | 540 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_probav | 541 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_gcomw | 542 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_gcomc | 543 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_stereo | 544 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_oceansat2 | 545 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_oceansat3 | 546 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_saral | 547 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_oceansat3x | 548 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_saralx | 549 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_kanopus | 550 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| xb_dmsp | 551 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| zombie | 552 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| whatsat | 553 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| uhfsatcom | 554 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| gmr1 | 555 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| starlink | 556 | pass | pass | pass | pass | pass | pass | pass | pass | - |  (real time) |
| oneweb | 557 | pass | pass | pass | pass | pass | pass | pass | pass | - |  (real time) |
| tdrs | 558 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| css | 559 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| tsikada | 560 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| gonets | 561 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| tiantong | 562 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| navic | 563 | FAIL | - | - | - | pass | - | - | pass | - |  (real time) |
| issdown | 564 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| wfbng | 602 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| xlink | 602 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| dectnrp | 603 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| mioty | 603 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| npr | 603 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| winmor | 603 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| xlinknb | 603 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| stanag4539 | 605 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| ale3g | 606 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| link11 | 607 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| stanag4481 | 608 | pass | weak | pass | pass | pass | pass | weak | pass | - |   |
| cisbee | 622 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| cis14 | 623 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| cis12 | 624 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| cis36 | 625 | lock | lock | FAIL | lock | pass | lock | lock | pass | - |   |
| cishdr | 626 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| cis128 | 627 | lock | lock | FAIL | lock | pass | lock | lock | pass | - |   |
| cis3000 | 628 | lock | lock | FAIL | lock | pass | lock | lock | pass | - |   |
| vdl3 | 642 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| cdma2000 | 662 | pass | weak | weak | pass | pass | pass | weak | pass | - |  (real time) |
| tdscdma | 663 | pass | weak | weak | weak | pass | pass | weak | pass | - |   |
| hrdcp | 702 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| isdbtsb | 732 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| dvbc2 | 743 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| dvbsh | 748 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| jt6m | 752 | pass | pass | FAIL | pass | pass | pass | pass | pass | - |   |
| ltpos | 792 | lock | lock | lock | lock | pass | lock | lock | pass | - |   |
| hfpacket300 | 802 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| voiceinv | 803 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| elt121 | 804 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| schumann | 822 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| whistler | 825 | pass | FAIL | FAIL | pass | pass | pass | FAIL | pass | - |   |
| emeecho | 828 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| radiojove | 831 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| callisto | 834 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| beaconevents | 837 | lock | lock | lock | lock | lock | lock | lock | lock | - |   |
| ax100m5 | 842 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| ax100m6 | 845 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| usp | 848 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| ngham | 851 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| u482c | 854 | pass | pass | pass | FAIL | pass | pass | pass | pass | - |   |
| beesat | 857 | pass | pass | pass | weak | pass | pass | pass | pass | - |   |
| fbr | 872 | pass | pass | pass | pass | pass | pass | pass | pass | - |   |
| cp16 | 882 | pass | pass | FAIL | pass | pass | pass | pass | pass | lock |   |
| rcppm | 883 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| uvb76 | 884 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| graves | 885 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| jove | 886 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| beacon | 887 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| em4100 | 888 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| uhfrfid | 889 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| qi | 890 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| fmmon | 891 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| ammon | 892 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| plnoise | 893 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| radar | 894 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| gnssjam | 895 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| passband | 896 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
| vlfmsk | 897 | - | - | - | - | pass | - | - | - | - |  no test signal (noise only) |
