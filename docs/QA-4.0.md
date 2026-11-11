# QA 4.0: every mode through the Engine

Worker `feat40-qa`, 2026-11-11/12. All of it runs on simulated signals: each mode's own generator and transmitter. No recording was used.
Run on GitHub (`quick.yml`, Linux, 3 cores): `qa_sweep_a` .. `qa_sweep_i` (the sweep in 8 shards), `qa_switch`, and a macOS run of `qa_switch` under
AddressSanitizer + UndefinedBehaviorSanitizer. The 3.9 sweep and its method are in `docs/QA-SWEEP-3.9.md`; this page is what 4.0 added and found.

## What the sweep checks now

| check | how | new in 4.0 |
|---|---|---|
| clean, +2 kHz +50 ppm, -2 kHz -50 ppm, 12 dB in the signal band, noise only, the mode's own transmitter | as in 3.9: generator -> file -> a fresh Engine in its own process | no |
| **rate** | the generator again at **1.37 x** the mode's sample rate (a non-integer ratio) and, for modes whose bandwidth is a filter, a **1.3 x** custom bandwidth, through a fresh Engine. DVB-T and DVB-T2 keep their channel bandwidth (it is the symbol rate there) | yes |
| **noise only, modes without a generator** | the modes that have no test signal (about 40) now get 3 s of white noise through a fresh Engine: no crash, no hang, no decode | yes |
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
| jt6m: one false ping in 32 s of noise | noise run FALSE | **not fixed**: stricter limits (mean sync 14 for two frames, 7 for three) removed it but turned the -50 ppm run from weak to fail, so they were reverted; listed in `qa_known.inc` | open |

No hang, no crash, no `restart refused` in any of the ~340 modes x 6 conditions on Linux, and none in the 300-mode switch test on Linux, macOS or under the sanitizers.

## Sanitizers

`qa_switch` built with `-DDECT2_SANITIZE=ON` (AddressSanitizer + UndefinedBehaviorSanitizer, macOS runner): 300 modes forward and backward, **no report** (no heap error, no leak report at exit, no undefined behaviour). Resident memory 2.7 GB after the first pass and 3.1 GB after the second (+11 %: the sanitizer's quarantine), 41 -> 43 threads. The engine unit tests were not run under the sanitizer in this task (no time).

## Open findings (listed in `tests/qa/qa_known.inc`, not fixed here)

| mode | what | evidence | next step |
|---|---|---|---|
| vdl3 (642) | nothing decodes through the Engine, from a file and from the synthetic source directly; the receiver's own tests (`tests/vdl3`) pass | clean FAIL at 3, 8 and 32 s; `QA_DIRECT=1` run on macOS | find what the Engine path does differently (tune offset 100 kHz, signal offset) |
| ltpos (792) | same | same | same |
| cell (34), umts | at 1.37 x the default rate (27.4 Msps for cell) no cell is found; both pass at the default rate | rate FAIL; log: "searching for LTE cells" only | check the resampling of the search to the sync rate at odd rates |
| cell / tetra transmitter | the TX file decodes nothing in 3 s (cell) / in 32 s (tetra) | tx FAIL on Linux and macOS | tetra passed in 3.9 after feat39-tetrafix: look for a regression |
| sonde, ax100m6, u482c, ngham, fbr | 12 dB in the signal band: nothing decodes | snr FAIL | threshold or a real sensitivity gap: the sensitivity workers' list |
| jt6m | one false ping per 32 s of noise | noise FALSE | a better ping test than sync >= 5 for three frames |
| cis3000, cis36, cishdr, cis128 | identification only: the receiver names the modulation in its log but has no block counter and no lock flag, so the sweep cannot count a decode | clean FAIL by the sweep's measure | give them a lock flag when they identify |
| noaaapt, noaagac, noaadsb, meteorhrpt, goesmdl, nfc, specan | count picture lines or measurements without a check: noise gives "decodes" | noise FALSE (known since 3.9) | a signal-present test before counting |

Every other row of the table below passes, or matches a known failure with its reason (HF audio modes at +-2 kHz, 12 dB below a modulation's threshold, a signal longer than the short run).

## What was validated on simulated signals only

Everything above. A mode that passes here decodes **its own generator's** signal; that proves the engine path (tuning, resampling, start / stop / switch, results) works, not that the decoder is right for real signals.

## Per-mode result (Linux, last full run)

Columns: clean, +2 kHz +50 ppm, -2 kHz -50 ppm, 12 dB, noise only, rate (1.37 x rate + custom bandwidth), tx. `-` = not run (nothing in the clean run, or no transmitter), `lock` = the mode only measures or locks, `weak` = fewer than half of the clean run's blocks. `FAIL` / `FALSE` rows are in the open-findings table or in `qa_known.inc`.

| mode | std | clean | pos | neg | snr | noise | rate | tx | notes |
|---|---|---|---|---|---|---|---|---|---|
| t2 | 1 | lock | lock | lock | lock | pass | lock | pass |  |
| dvbt | 2 | pass | pass | pass | FAIL | pass | pass | pass |  |
| atsc | 3 | pass | pass | pass | FAIL | pass | pass | pass |  |
| isdbt | 6 | pass | pass | pass | weak | pass | pass | pass |  |
| fm | 7 | pass | pass | pass | FAIL | pass | pass | pass |  |
| dvbs | 8 | pass | pass | pass | pass | pass | pass | pass |  |
| dtmb | 9 | pass | pass | pass | FAIL | pass | pass | pass |  |
| atv | 10 | pass | pass | pass | pass | pass | pass | pass |  |
| dmr | 11 | pass | pass | pass | pass | pass | pass | pass |  |
| gnss | 14 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| sonde | 15 | pass | pass | pass | FAIL | pass | pass | pass |  |
| ais | 16 | pass | pass | pass | pass | pass | pass | pass |  |
| marine | 17 | FAIL | - | - | - | pass | - | pass | (real time) |
| wefax | 17 | lock | lock | FAIL | lock | pass | lock | - |  |
| acars | 18 | pass | pass | pass | pass | pass | pass | pass |  |
| inmc | 19 | pass | pass | pass | pass | pass | pass | pass |  |
| loraaprs | 22 | pass | pass | pass | pass | pass | pass | pass |  |
| meshcore | 22 | pass | pass | pass | pass | pass | pass | pass |  |
| meshtastic | 22 | pass | pass | pass | pass | pass | pass | - |  |
| mesh | 22 | pass | pass | pass | pass | pass | pass | pass |  |
| flex | 25 | pass | pass | pass | pass | pass | pass | - |  |
| pocsag | 25 | pass | pass | pass | pass | pass | pass | - |  |
| pager | 25 | pass | pass | pass | pass | pass | pass | pass |  |
| packet | 26 | pass | pass | pass | pass | pass | pass | pass |  |
| freedv | 27 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| q65 | 27 | FAIL | - | - | - | pass | - | FAIL | (real time) |
| dominoex | 27 | pass | pass | FAIL | pass | pass | pass | pass |  |
| sstv | 27 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| jt9 | 27 | FAIL | - | - | - | pass | - | FAIL | (real time) |
| thor | 27 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| rtty | 27 | pass | FAIL | weak | pass | pass | pass | pass |  |
| jt65 | 27 | FAIL | - | - | - | pass | - | FAIL | (real time) |
| mfsk | 27 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| wspr | 27 | FAIL | - | - | - | pass | - | FAIL | (real time) |
| olivia | 27 | pass | weak | FAIL | pass | pass | pass | pass |  |
| ft2 | 27 | pass | pass | weak | pass | pass | pass | pass |  |
| jt4 | 27 | FAIL | - | - | - | pass | - | - | (real time) |
| hfdig | 27 | pass | FAIL | weak | pass | pass | pass | - |  |
| hamdrm | 27 | pass | FAIL | FAIL | FAIL | pass | pass | pass | (saved bytes) |
| contestia | 27 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| airband | 28 | pass | pass | pass | pass | pass | pass | lock |  |
| vdl2 | 29 | pass | pass | pass | pass | pass | pass | pass |  |
| ism | 30 | pass | pass | pass | pass | pass | pass | pass |  |
| ndb | 31 | pass | pass | pass | pass | pass | pass | pass |  |
| vor | 32 | pass | pass | pass | pass | pass | pass | - |  |
| ils | 32 | pass | pass | pass | FAIL | pass | pass | - |  |
| cell | 34 | pass | pass | pass | pass | pass | FAIL | FAIL | (real time) |
| dstar | 35 | pass | pass | pass | pass | pass | pass | pass |  |
| cw | 36 | pass | pass | pass | pass | pass | pass | pass |  |
| hfdl | 37 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| js8 | 39 | pass | pass | weak | pass | pass | pass | pass |  |
| nxdn | 40 | pass | pass | pass | pass | pass | pass | pass |  |
| dvbc | 42 | pass | pass | pass | FAIL | pass | pass | pass |  |
| specx | 43 | lock | lock | lock | lock | pass | lock | lock |  |
| specan | 44 | pass | pass | pass | pass | FALSE | pass | pass |  |
| uplink | 45 | pass | pass | pass | pass | pass | pass | pass |  |
| orbcomm | 46 | pass | pass | pass | pass | pass | pass | pass |  |
| dpmr | 47 | pass | pass | pass | pass | pass | pass | pass |  |
| mac | 49 | pass | pass | pass | pass | pass | pass | pass |  |
| astro | 50 | lock | lock | lock | lock | lock | lock | lock |  |
| p25 | 52 | pass | pass | pass | pass | pass | pass | pass |  |
| ble | 53 | pass | pass | pass | pass | pass | pass | pass |  |
| iono | 54 | lock | lock | lock | lock | pass | lock | lock |  |
| qo100 | 55 | lock | lock | lock | lock | pass | lock | lock |  |
| rid | 58 | pass | pass | pass | pass | pass | pass | pass |  |
| nr | 59 | pass | pass | pass | pass | pass | pass | pass | (real time) |
| time | 60 | lock | FAIL | FAIL | lock | pass | lock | - |  |
| meteor | 61 | pass | pass | pass | pass | pass | pass | pass |  |
| goes | 62 | pass | pass | pass | pass | pass | pass | pass |  |
| cubesat | 63 | pass | pass | pass | weak | pass | pass | pass |  |
| hrpt | 66 | pass | pass | pass | pass | pass | pass | pass |  |
| zigbee | 67 | pass | pass | pass | pass | pass | pass | pass |  |
| flarm | 68 | pass | pass | pass | pass | pass | pass | pass |  |
| hfbeacon | 69 | lock | FAIL | FAIL | lock | pass | lock | lock |  |
| dji | 70 | pass | pass | pass | pass | pass | pass | pass | (real time) |
| loraraw | 73 | pass | pass | pass | pass | pass | pass | pass |  |
| codar | 74 | pass | pass | pass | pass | pass | pass | pass |  |
| wb | 75 | - | - | - | - | pass | - | - | no test signal (noise only) |
| datawell | 76 | lock | lock | lock | lock | pass | lock | lock |  |
| multitone | 77 | pass | pass | pass | FAIL | pass | pass | pass |  |
| same | 78 | pass | pass | pass | pass | pass | pass | pass |  |
| tetrapol | 81 | pass | pass | pass | pass | pass | pass | pass |  |
| elektro | 82 | pass | pass | pass | pass | pass | pass | pass |  |
| lbj | 83 | pass | pass | pass | pass | pass | pass | pass |  |
| vdl4 | 84 | pass | pass | pass | FAIL | pass | pass | pass |  |
| uat | 85 | pass | pass | pass | pass | pass | pass | pass |  |
| wisun | 86 | pass | pass | pass | pass | pass | pass | pass |  |
| eczas | 87 | pass | FAIL | FAIL | FAIL | pass | pass | pass |  |
| sarsat | 94 | pass | pass | pass | pass | pass | pass | - |  |
| mt63 | 97 | pass | pass | FAIL | pass | pass | pass | pass |  |
| zwave | 100 | pass | pass | pass | pass | pass | weak | pass |  |
| fpv | 103 | pass | pass | pass | pass | pass | pass | pass |  |
| mscatter | 106 | lock | lock | lock | lock | pass | lock | lock |  |
| cquam | 109 | pass | FAIL | FAIL | pass | pass | pass | pass |  |
| fsq | 115 | pass | pass | pass | pass | pass | pass | pass |  |
| ifkp | 118 | pass | pass | pass | pass | pass | pass | pass |  |
| sitor | 121 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| dgps | 122 | - | - | - | - | pass | - | - | no test signal (noise only) |
| dme | 123 | - | - | - | - | pass | - | - | no test signal (noise only) |
| kaal | 124 | pass | pass | pass | weak | pass | pass | - |  |
| kavb | 125 | pass | pass | pass | pass | pass | pass | pass |  |
| fusion | 130 | - | - | - | - | pass | - | - | no test signal (noise only) |
| ltr | 131 | - | - | - | - | pass | - | - | no test signal (noise only) |
| edacs | 132 | - | - | - | - | pass | - | - | no test signal (noise only) |
| kaws | 135 | FAIL | - | - | - | pass | - | - | (real time) generator sends silence |
| ardop | 142 | pass | FAIL | pass | pass | pass | pass | pass |  |
| goesdcs | 143 | pass | pass | pass | pass | pass | pass | pass |  |
| modeac | 144 | pass | pass | pass | weak | pass | pass | pass |  |
| bletracker | 145 | pass | pass | pass | pass | pass | pass | pass |  |
| milhf | 147 | - | - | - | - | pass | - | - | no test signal (noise only) |
| nfc | 150 | pass | pass | pass | weak | FALSE | pass | pass |  |
| sbas | 152 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| asterix | 153 | FAIL | - | - | - | pass | - | - | (real time) generator sends silence |
| argos | 154 | pass | pass | pass | FAIL | pass | pass | - |  |
| sigfox | 155 | pass | pass | pass | pass | pass | pass | - |  |
| isdbs | 156 | pass | pass | pass | pass | pass | pass | - |  |
| marinevhf | 157 | - | - | - | - | pass | - | - | no test signal (noise only) |
| horus | 158 | pass | pass | pass | pass | pass | pass | - |  |
| pactor | 205 | pass | pass | pass | pass | pass | pass | - |  |
| esubghz | 206 | pass | pass | pass | pass | pass | pass | pass |  |
| roshf | 207 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| ssdv | 208 | pass | pass | pass | weak | pass | pass | - |  |
| ebicab | 209 | pass | pass | pass | pass | pass | pass | - |  |
| inmsat | 210 | pass | pass | pass | pass | pass | pass | - |  |
| atis | 213 | pass | pass | pass | weak | pass | pass | - |  |
| gk2alrit | 232 | pass | pass | pass | pass | pass | pass | - |  |
| gk2ahrit | 233 | pass | pass | pass | pass | pass | pass | - |  |
| fy4ahrit | 235 | pass | pass | pass | pass | pass | pass | - |  |
| fy2svissr | 236 | pass | pass | pass | pass | pass | pass | - |  |
| fy3ahrpt | 237 | pass | pass | pass | pass | pass | pass | - |  |
| fy3mpt | 238 | pass | pass | pass | pass | pass | pass | - |  |
| pactor2 | 239 | pass | pass | FAIL | pass | pass | pass | - | (real time) |
| pactor3 | 240 | pass | FAIL | FAIL | pass | pass | pass | - | (real time) |
| pactor4 | 241 | lock | FAIL | FAIL | lock | pass | lock | - |  |
| selcal | 242 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| sensors | 243 | pass | pass | pass | pass | pass | pass | - |  |
| sferics | 245 | pass | pass | pass | pass | pass | pass | - |  |
| thread | 246 | pass | pass | pass | pass | pass | pass | - |  |
| bgan | 247 | pass | pass | pass | pass | pass | pass | - |  |
| noaaapt | 262 | pass | pass | pass | pass | FALSE | pass | - |  |
| noaahrpt | 263 | pass | pass | pass | pass | pass | pass | - |  |
| noaagac | 264 | pass | pass | pass | pass | FALSE | pass | - |  |
| noaadsb | 265 | pass | pass | pass | pass | FALSE | pass | - |  |
| meteorhrpt | 266 | pass | pass | pass | pass | FALSE | pass | - |  |
| arktikaggak | 269 | pass | pass | pass | pass | pass | pass | - |  |
| awsl | 270 | pass | pass | pass | pass | pass | pass | - |  |
| msgraw | 271 | pass | pass | pass | pass | pass | pass | - |  |
| himawaricast | 272 | pass | pass | pass | pass | pass | pass | - |  |
| geonetcast | 273 | pass | pass | pass | pass | pass | pass | - |  |
| goescda | 294 | pass | pass | pass | pass | pass | pass | - |  |
| goesmdl | 295 | pass | pass | pass | pass | FALSE | pass | - |  |
| gk2auhrit | 296 | pass | pass | pass | weak | pass | pass | - |  |
| gk2acdas | 297 | pass | pass | pass | pass | pass | pass | - |  |
| sband | 298 | pass | pass | pass | pass | pass | pass | - |  |
| geoscan | 299 | lock | lock | lock | lock | lock | lock | - |  |
| tetradmo | 323 | - | - | - | - | pass | - | - | no test signal (noise only) |
| opensky | 324 | - | - | - | - | pass | - | - | no test signal (noise only) |
| cb | 342 | pass | pass | pass | FAIL | pass | pass | - |  |
| frs | 343 | pass | pass | pass | FAIL | pass | pass | - |  |
| pmr446 | 344 | pass | pass | pass | FAIL | pass | pass | - |  |
| mdc1200 | 347 | pass | pass | pass | FAIL | pass | pass | - |  |
| wmic | 348 | lock | lock | lock | lock | pass | lock | - |  |
| rclink | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| sfhss | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| flysky | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| frsky | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| elrs | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| reticulum | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| mavlink | 362 | - | - | - | - | pass | - | - | no test signal (noise only) |
| btc | 382 | pass | pass | pass | pass | pass | pass | - |  |
| fanet | 402 | - | - | - | - | pass | - | - | no test signal (noise only) |
| ogn | 403 | - | - | - | - | pass | - | - | no test signal (noise only) |
| gbas | 404 | - | - | - | - | pass | - | - | no test signal (noise only) |
| ldacs | 405 | - | - | - | - | pass | - | - | no test signal (noise only) |
| dsrc | 408 | - | - | - | - | pass | - | - | no test signal (noise only) |
| its | 409 | - | - | - | - | pass | - | - | no test signal (noise only) |
| cv2x | 410 | - | - | - | - | pass | - | - | no test signal (noise only) |
| ermes | 411 | - | - | - | - | pass | - | - | no test signal (noise only) |
| mradar | 412 | - | - | - | - | pass | - | - | no test signal (noise only) |
| wradar | 413 | - | - | - | - | pass | - | - | no test signal (noise only) |
| milstd110 | 432 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| stanag4285 | 433 | lock | FAIL | FAIL | lock | pass | lock | - |  |
| arqe | 434 | pass | pass | FAIL | pass | pass | pass | - |  |
| arqn | 435 | pass | pass | FAIL | pass | pass | pass | - |  |
| autospec | 436 | pass | pass | FAIL | pass | pass | pass | - |  |
| efr | 439 | pass | pass | FAIL | pass | pass | pass | - |  |
| piccolo | 440 | pass | pass | FAIL | pass | pass | pass | - |  |
| coquelet | 441 | pass | pass | FAIL | pass | pass | pass | - |  |
| qrss | 462 | FAIL | - | - | - | pass | - | - | (real time) |
| dfcw | 463 | FAIL | - | - | - | pass | - | - | (real time) |
| fsk441 | 464 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| throb | 467 | pass | pass | FAIL | pass | pass | pass | - |  |
| umts | 482 | pass | pass | pass | pass | pass | FAIL | - | (real time) |
| supersid | 502 | lock | lock | lock | lock | lock | lock | - |  |
| riometer | 503 | FAIL | - | - | - | pass | - | - | (real time) |
| pulsar | 504 | lock | lock | lock | lock | pass | lock | - |  |
| erminaz | 512 | lock | lock | lock | lock | lock | lock | - |  |
| xb_npp | 532 | pass | pass | pass | pass | pass | pass | - |  |
| xb_jpss2 | 533 | pass | pass | pass | pass | pass | pass | - |  |
| xb_aqua | 534 | pass | pass | pass | pass | pass | pass | - |  |
| xb_metopsg | 537 | pass | pass | pass | pass | pass | pass | - |  |
| xb_landsatx | 538 | pass | pass | pass | pass | pass | pass | - |  |
| xb_landsats | 539 | pass | pass | pass | pass | pass | pass | - |  |
| xb_probas | 540 | pass | pass | pass | pass | pass | pass | - |  |
| xb_probav | 541 | pass | pass | pass | pass | pass | pass | - |  |
| xb_gcomw | 542 | pass | pass | pass | pass | pass | pass | - |  |
| xb_oceansat2 | 545 | pass | pass | pass | pass | pass | pass | - |  |
| xb_oceansat3 | 546 | pass | pass | pass | pass | pass | pass | - |  |
| xb_saral | 547 | pass | pass | pass | pass | pass | pass | - |  |
| xb_oceansat3x | 548 | pass | pass | pass | pass | pass | pass | - |  |
| xb_saralx | 549 | pass | pass | pass | pass | pass | pass | - |  |
| xb_kanopus | 550 | pass | pass | pass | pass | pass | pass | - |  |
| zombie | 552 | lock | lock | lock | lock | pass | lock | - |  |
| whatsat | 553 | lock | lock | lock | lock | pass | lock | - |  |
| uhfsatcom | 554 | pass | pass | pass | pass | pass | pass | - |  |
| gmr1 | 555 | pass | pass | pass | pass | pass | pass | - |  |
| starlink | 556 | pass | pass | pass | pass | pass | pass | - | (real time) |
| oneweb | 557 | pass | pass | pass | pass | pass | pass | - | (real time) |
| tsikada | 560 | lock | lock | lock | lock | pass | lock | - |  |
| gonets | 561 | pass | pass | pass | pass | pass | pass | - |  |
| tiantong | 562 | pass | pass | pass | pass | pass | pass | - |  |
| navic | 563 | FAIL | - | - | - | pass | - | - | (real time) |
| issdown | 564 | lock | lock | lock | lock | pass | lock | - |  |
| dectnrp | 603 | - | - | - | - | pass | - | - | no test signal (noise only) |
| winmor | 603 | - | - | - | - | pass | - | - | no test signal (noise only) |
| xlinknb | 603 | - | - | - | - | pass | - | - | no test signal (noise only) |
| npr | 603 | - | - | - | - | pass | - | - | no test signal (noise only) |
| mioty | 603 | - | - | - | - | pass | - | - | no test signal (noise only) |
| ale3g | 606 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| link11 | 607 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| stanag4481 | 608 | pass | weak | pass | pass | pass | pass | - |  |
| cisbee | 622 | pass | pass | FAIL | pass | pass | pass | - |  |
| cis36 | 625 | FAIL | - | - | - | pass | - | - | (real time) |
| cishdr | 626 | FAIL | - | - | - | pass | - | - | (real time) |
| cis128 | 627 | FAIL | - | - | - | pass | - | - | (real time) |
| cis3000 | 628 | FAIL | - | - | - | pass | - | - | (real time) |
| vdl3 | 642 | FAIL | - | - | - | pass | - | - | (real time) |
| cdma2000 | 662 | pass | weak | weak | pass | pass | pass | - | (real time) |
| tdscdma | 663 | pass | weak | weak | weak | pass | pass | - |  |
| hrdcp | 702 | pass | pass | pass | pass | pass | pass | - |  |
| isdbtsb | 732 | pass | pass | pass | weak | pass | pass | - |  |
| dvbc2 | 743 | pass | pass | pass | FAIL | pass | pass | - |  |
| jt6m | 752 | pass | pass | FAIL | pass | pass | pass | - |  |
| ltpos | 792 | FAIL | - | - | - | pass | - | - | (real time) |
| hfpacket300 | 802 | pass | FAIL | FAIL | pass | pass | pass | - |  |
| voiceinv | 803 | pass | pass | pass | pass | pass | pass | - |  |
| schumann | 822 | lock | lock | lock | lock | lock | lock | - |  |
| radiojove | 831 | lock | lock | lock | lock | lock | lock | - |  |
| callisto | 834 | lock | lock | lock | lock | lock | lock | - |  |
| beaconevents | 837 | lock | lock | lock | lock | lock | lock | - |  |
| ax100m5 | 842 | pass | pass | pass | pass | pass | pass | - |  |
| ax100m6 | 845 | pass | pass | pass | FAIL | pass | pass | - |  |
| usp | 848 | pass | pass | pass | pass | pass | pass | - |  |
| ngham | 851 | pass | pass | pass | FAIL | pass | pass | - |  |
| u482c | 854 | pass | pass | pass | FAIL | pass | pass | - |  |

Shards `qa_sweep_e` and `qa_sweep_i` passed in full (a passing test prints no table): adsb, adsl, aero, aeromacs, ale, am, ant, arktikardas, atsc3, beesat, cdr, cis12, cis14, cp16, css, dab, dect, drm, dsc, dvbsh, elektrordas, eloran, elt121, emeecho, eot, fbr, fst4, ft4, ft8, fy4lrit, general, globalstar, goesgrb, goesgvar, gsmr, gtor, halow, hamfm, hdr, hell, hfvoice, iptv, iridium, iscat, kaft, kait, kapp, klw, lorasat, lorawan, lorawanwide, lucky7, m17, meshcom, mpt1327, msk144, navtex, nbiot, nrf24, p25p2, pi4, psk, pskd, satbeacon, shortwave, smartnet, spektrum, spino, spreadhf, stanag4539, tdcs, tdrs, tetra, tones, vdes, vorils, wfbng, whistler, wifi, wmbus, xb_dmsp, xb_gcomc, xb_metopd, xb_stereo, xb_terra, xlink.
