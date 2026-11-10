# All-modes QA sweep (3.9)

`tests/qa/test_qa_sweep.cpp` runs every mode of the mode list (the core standards, every `ModeTuning` row and every sub-mode: about 300
entries) from its built-in test signal through the Engine to its decoder, under six conditions, and checks the start / stop / retune
cycle and the CPU per mode. One binary, eight ctest shards (`qa_sweep_a` .. `qa_sweep_i`, label `qa`, not on Windows: it forks).

All of it is on simulated signals: the modes' own generators and transmitters. No recording was used.

## What a run does, per mode

1. The test signal is made once (the synthetic source, generator SNR 60 dB) and written as 16-bit IQ files:
   - **clean**: as it comes. An offset-tuned mode's signal is moved to the centre, as a recording has it.
   - **pos / neg**: carrier +2 / -2 kHz and the sample clock +50 / -50 ppm (resampled; a cheap radio's crystal).
   - **snr**: white noise at 12 dB below the signal inside the signal's own 99 % band. The power is measured over the active part of the signal, so bursts count at their own level.
   - **noise**: the same noise with no signal. Nothing may decode.
   - **tx**: the mode's own transmitter (TX registry generator, output stage and limiter, as a TX session makes them) instead of the test signal, at the rate the transmitter asks for.
   - silence: the baseline for the modes that are measured by their saved results.
2. Each file goes through a fresh Engine in a child process, so a crash or hang marks only that row. The file is read as fast as the
   receiver takes it.
3. **Decoded** means that blocks passed the protocol's own check (CRC, FEC, parity): Engine `blocksOk`; DVB-T / ISDB-T RS-clean plus
   corrected packets; FM RDS groups; HF digital its decoders' own counts (characters, FT8 / WSPR decodes, SSTV rows, FreeDV speech frames).
   A mode that counts none of these is measured by what it saves (`Engine::saveResults`) beyond what it saves for silence. A mode that only
   measures or locks (spectrum tools, audio) passes on its lock flag (`lock`). `weak` means fewer than half of the clean run's blocks.
4. Lengths: 3 s; 8 s when the clean run had fewer than 8 blocks; 32 s; then 32 s played at the radio's real speed. The last try is for the
   decoders that work on their own threads and skip what they cannot keep up with (FT8 and the other slot modes). In the app a file plays at
   its own speed. The transmitter gets one more 32 s try when the short one decodes nothing. `ONAIR_TEST_LONG=1` (or `--long`) runs 8 s,
   32 s and 120 s (one WSPR slot), then 32 s at real speed.
5. After the clean run the same Engine is retuned 100 kHz away and back, stopped, started again on the file and stopped again. Each step
   runs under a watchdog: a hang, or no samples after the restart, fails the mode.
6. **CPU**: the CPU seconds (all threads) the receiver used per second of signal in the clean run, steady state (the first quarter is left
   out), as `bench_modes` measures it.

Run it locally: `test_qa_sweep --mode id[,id..]`, `--shard k/N`, `--list`, `--out prefix` (TSV), `--strict` (a known failure that
passes fails too). `QA_SWEEP_VERBOSE=1` prints each run's engine log and last report.

Known failures with a reason are in `tests/qa/qa_known.inc`. Anything else that fails, fails the test.
## Results

Sources: macOS arm64 CI runners (quick.yml runs 42893435035 and 42908822658: 3 cores, four shards at a time) for most rows. The shards that ran out of CI time (`qa_sweep_c`, `qa_sweep_d`) and the modes changed afterwards (Remote ID, BLE, HF digital) ran on the Mac through `localbuild.sh` (nice 15, beside other builds). The CPU figures are indicative: compare them within a run, not across machines.

The mode list has 302 entries. 39 of them have no built-in test signal: no generator, so they are listed but not run. Of the 263 that have one, 235 decode their clean signal and 154 pass every condition they ran.

| condition | modes run | pass (incl. weak / lock) |
|---|---|---|
| clean | 263 | 235 |
| pos | 235 | 197 |
| neg | 235 | 189 |
| snr | 235 | 214 |
| noise | 263 | 252 |
| tx | 125 | 109 |

Failures by class (one per mode and condition, as in `tests/qa/qa_known.inc`):

| class | count |
|---|---|
| unrealistic | 76 |
| open | 40 |
| threshold | 16 |
| too short | 15 |
| no check | 10 |
| no signal | 2 |
| generator | 1 |

| mode | std | Msps | s | clean | +2 kHz +50 ppm | -2 kHz -50 ppm | 12 dB | noise | tx | CPU s/s | clean measure | failure classes / notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| t2 | 1 | 9.143 | 3 | lock | lock | lock | lock | pass | pass | 0.225 | 0 |  |
| dvbt | 2 | 9.143 | 3 | pass | pass | pass | FAIL | pass | pass | 0.759 | 41730 | snr threshold |
| atsc | 3 | 8 | 3 | pass | pass | pass | FAIL | pass | pass | 1.262 | 37076 | snr threshold |
| dab | 4 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.269 | 372 |  |
| atsc3 | 5 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.522 | 44 |  |
| isdbt | 6 | 8 | 3 | pass | pass | pass | weak | pass | pass | 0.930 | 21244 |  |
| fm | 7 | 4 | 3 | pass | pass | pass | FAIL | pass | pass | 0.221 | 33 | snr threshold |
| dvbs | 8 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.447 | 384 |  |
| dtmb | 9 | 10 | 3 | pass | pass | pass | FAIL | pass | pass | 0.575 | 12618 | snr threshold |
| atv | 10 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.674 | 132 |  |
| dmr | 11 | 2.4 | 3 | pass | pass | pass | pass | pass | pass | 0.075 | 145 |  |
| drm | 12 | 2 | 8 | pass | pass | pass | FAIL | pass | pass | 0.059 | 110 | snr threshold |
| adsb | 13 | 4 | 3 | pass | pass | pass | pass | pass | pass | 0.120 | 235 |  |
| gnss | 14 | 4 | 17.5 | pass | FAIL | FAIL | pass | pass | pass | 0.441 | 9 | pos unrealistic, neg unrealistic |
| sonde | 15 | 8 | 3 | pass | pass | pass | pass | pass | pass | 0.102 | 1 |  |
| ais | 16 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.055 | 7 |  |
| dsc | 17 | 2 | 8 | pass | FAIL | FAIL | pass | pass | - | 0.206 | 1 | pos unrealistic, neg unrealistic |
| marine | 17 | 2 | 32 | FAIL | - | - | - | pass | pass | 0.065 | 0 | clean too short; (real time) |
| navtex | 17 | 2 | 32 | FAIL | - | - | - | pass | - | 0.035 | 0 | clean too short; (real time) |
| wefax | 17 | 2 | 32 | lock | lock | FAIL | lock | pass | - | 0.035 | 0 | neg unrealistic |
| acars | 18 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.087 | 10 |  |
| inmc | 19 | 2 | 32 | pass | pass | pass | pass | pass | pass | 0.049 | 3 |  |
| aero | 20 | 4 | 8 | pass | pass | pass | pass | pass | pass | 0.075 | 252 |  |
| iridium | 21 | 10 | 3 | pass | weak | weak | pass | pass | pass | 0.247 | 40 |  |
| loraaprs | 22 | 2 | 8 | pass | pass | pass | pass | pass | pass | 0.024 | 1 |  |
| lorawan | 22 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.138 | 2 |  |
| mesh | 22 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.095 | 3 |  |
| meshcom | 22 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.073 | 1 | generator sends silence |
| meshcore | 22 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.100 | 1 |  |
| meshtastic | 22 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.059 | 2 |  |
| hdr | 23 | 2 | 3 | pass | FAIL | pass | FAIL | pass | pass | 0.132 | 1 | pos open, snr threshold |
| cdr | 24 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.098 | 40 |  |
| flex | 25 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.045 | 88 |  |
| pager | 25 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.083 | 16 |  |
| pocsag | 25 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.092 | 16 |  |
| packet | 26 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.082 | 2 |  |
| contestia | 27 | 2 | 8 | pass | FAIL | FAIL | pass | pass | pass | 0.031 | 19 | pos unrealistic, neg unrealistic |
| dominoex | 27 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.060 | 11 | pos unrealistic, neg unrealistic |
| freedv | 27 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.068 | 8 | pos unrealistic, neg unrealistic |
| fst4 | 27 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.068 | 0 | clean too short, tx too short; (real time) |
| ft2 | 27 | 2 | 32 | pass | pass | weak | pass | pass | pass | 0.062 | 12 |  |
| ft4 | 27 | 2 | 32 | pass | FAIL | weak | pass | pass | pass | 0.060 | 5 | pos unrealistic |
| ft8 | 27 | 2 | 32 | pass | weak | weak | pass | pass | pass | 0.204 | 1274 | (saved bytes) (real time) |
| hamdrm | 27 | 2 | 8 | pass | FAIL | FAIL | pass | pass | FAIL | 0.046 | 1551 | pos unrealistic, neg unrealistic, tx open; (saved bytes) |
| hell | 27 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.052 | 50 | pos unrealistic, neg unrealistic |
| hfdig | 27 | 2 | 3 | pass | FAIL | weak | pass | pass | - | 0.061 | 14 | pos unrealistic |
| jt4 | 27 | 2 | 32 | FAIL | - | - | - | pass | - | 0.060 | 0 | clean too short; (real time) |
| jt65 | 27 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.052 | 0 | clean too short, tx too short; (real time) |
| jt9 | 27 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.076 | 0 | clean too short, tx too short; (real time) |
| mfsk | 27 | 2 | 8 | pass | FAIL | FAIL | pass | pass | pass | 0.036 | 15 | pos unrealistic, neg unrealistic |
| msk144 | 27 | 2 | 32 | pass | FAIL | FAIL | pass | pass | pass | 0.069 | 81 | pos unrealistic, neg unrealistic; (saved bytes) |
| olivia | 27 | 2 | 32 | pass | weak | FAIL | pass | pass | pass | 0.028 | 58 | neg unrealistic |
| q65 | 27 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.048 | 0 | clean too short, tx too short; (real time) |
| rtty | 27 | 2 | 8 | pass | FAIL | FAIL | pass | pass | pass | 0.039 | 43 | pos unrealistic, neg unrealistic; (saved bytes) |
| sstv | 27 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.050 | 11 | pos unrealistic, neg unrealistic |
| thor | 27 | 2 | 8 | pass | FAIL | FAIL | pass | pass | pass | 0.028 | 11 | pos unrealistic, neg unrealistic |
| wspr | 27 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.057 | 0 | clean too short, tx too short; (real time) |
| airband | 28 | 2 | 3 | pass | pass | pass | pass | pass | lock | 0.141 | 1 |  |
| vdl2 | 29 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.381 | 12 |  |
| ism | 30 | 2 | 3 | pass | pass | pass | pass | pass | pass | 1.207 | 6 |  |
| ndb | 31 | 0.256 | 32 | pass | pass | pass | pass | pass | pass | 0.072 | 16 |  |
| ils | 32 | 2 | 3 | pass | pass | pass | FAIL | pass | - | 0.064 | 10 | snr threshold |
| vor | 32 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.060 | 10 |  |
| vorils | 32 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.116 | 10 |  |
| m17 | 33 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.107 | 66 |  |
| cell | 34 | 20 | 3.5 | pass | pass | pass | pass | pass | pass | 0.884 | 9 | (real time) |
| dstar | 35 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.069 | 127 |  |
| cw | 36 | 2 | 3 | pass | pass | pass | weak | pass | pass | 0.075 | 128 |  |
| hfdl | 37 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.116 | 3 | pos unrealistic, neg unrealistic |
| psk | 38 | 2 | 3 | pass | pass | FAIL | pass | pass | pass | 0.078 | 13 | neg unrealistic |
| js8 | 39 | 2 | 32 | pass | FAIL | FAIL | FAIL | pass | FAIL | 0.104 | 3 | pos unrealistic, neg unrealistic, snr open, tx open; (real time) |
| nxdn | 40 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.123 | 38 |  |
| gsmr | 41 | 5 | 14 | pass | pass | pass | pass | pass | pass | 0.419 | 50 | (real time) |
| dvbc | 42 | 10 | 3 | pass | pass | pass | FAIL | pass | pass | 0.772 | 56107 | snr threshold |
| specx | 43 | 10 | 7 | FAIL | - | - | - | pass | FAIL | 0.156 | 0 | clean open, tx open; (real time) |
| specan | 44 | 20 | 3 | pass | pass | pass | pass | FALSE | pass | 0.241 | 58593 | noise no check |
| uplink | 45 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.156 | 70 |  |
| orbcomm | 46 | 1 | 3 | pass | pass | pass | pass | pass | pass | 0.093 | 94 |  |
| dpmr | 47 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.073 | 26 |  |
| tetra | 48 | 2 | 3 | pass | pass | pass | pass | pass | FAIL | 0.111 | 131 | tx open |
| mac | 49 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.681 | 6068 |  |
| astro | 50 | 2.5 | 28 | FAIL | - | - | - | pass | FAIL | 0.047 | 0 | clean open, tx open; (real time) |
| ale | 51 | 2 | 3 | pass | FAIL | FAIL | pass | pass | pass | 0.078 | 6 | pos unrealistic, neg unrealistic |
| p25 | 52 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.059 | 86 |  |
| ble | 53 | 4 | 3 | pass | pass | pass | pass | pass | pass | 0.122 | 857 |  |
| iono | 54 | 20 | 3.5 | FAIL | - | - | - | pass | FAIL | 0.675 | 0 | clean open, tx open; (real time) |
| qo100 | 55 | 10 | 7 | FAIL | - | - | - | pass | FAIL | 0.328 | 0 | clean open, tx open; (real time) |
| wifi | 56 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.814 | 765 |  |
| am | 57 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.099 | 299 |  |
| general | 57 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.099 | 299 |  |
| rid | 58 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.952 | 2963 |  |
| nr | 59 | 20 | 3.5 | FAIL | - | - | - | pass | FAIL | 0.799 | 0 | clean open, tx open; (real time) |
| time | 60 | 0.256 | 8 | lock | FAIL | FAIL | lock | pass | - | 0.022 | 0 | pos unrealistic, neg unrealistic |
| meteor | 61 | 1 | 3 | pass | pass | pass | pass | pass | pass | 0.062 | 24 |  |
| goes | 62 | 2.4 | 3 | pass | pass | pass | pass | pass | pass | 0.317 | 167 |  |
| cubesat | 63 | 2 | 3 | pass | pass | pass | weak | pass | pass | 0.080 | 5 |  |
| globalstar | 64 | 10 | 3 | pass | pass | pass | pass | pass | pass | 0.117 | 3 |  |
| vdes | 65 | 1 | 3 | pass | pass | pass | pass | pass | pass | 0.122 | 6 |  |
| hrpt | 66 | 6 | 3 | pass | pass | pass | pass | pass | pass | 0.525 | 850 |  |
| zigbee | 67 | 4 | 3 | pass | pass | pass | pass | pass | pass | 0.315 | 893 |  |
| flarm | 68 | 1 | 3 | pass | pass | pass | pass | pass | pass | 0.061 | 9 |  |
| hfbeacon | 69 | 10 | 7 | FAIL | - | - | - | pass | FAIL | 0.168 | 0 | clean open, tx open; (real time) |
| dji | 70 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.780 | 3 |  |
| nbiot | 71 | 2 | 8 | pass | pass | FAIL | pass | pass | pass | 0.090 | 1 | neg open |
| halow | 72 | 6 | 3 | pass | pass | pass | pass | pass | pass | 1.449 | 520 |  |
| loraraw | 73 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.144 | 6 |  |
| codar | 74 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.111 | 123 |  |
| wb | 75 | 2 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| datawell | 76 | 0.25 | 8 | lock | lock | lock | lock | pass | lock | 0.012 | 0 |  |
| multitone | 77 | 2 | 8 | pass | pass | pass | FAIL | pass | pass | 0.027 | 11 | snr threshold |
| same | 78 | 2 | 8 | pass | pass | pass | pass | pass | pass | 0.040 | 1 |  |
| ant | 79 | 4 | 3 | pass | pass | pass | pass | pass | pass | 0.229 | 36 |  |
| dect | 80 | 5 | 14 | FAIL | - | - | - | pass | pass | 0.209 | 0 | clean open; (real time) |
| tetrapol | 81 | 0.25 | 3 | pass | pass | pass | pass | pass | pass | 0.043 | 148 |  |
| elektro | 82 | 2.4 | 3 | pass | pass | pass | pass | pass | pass | 0.200 | 118 |  |
| lbj | 83 | 0.96 | 3 | pass | pass | pass | pass | pass | pass | 0.061 | 2 |  |
| vdl4 | 84 | 2 | 3 | pass | pass | pass | FAIL | pass | pass | 0.073 | 3 | snr threshold |
| uat | 85 | 2 | 32 | FAIL | - | - | - | pass | FAIL | 0.042 | 0 | clean open, tx open; (real time) |
| wisun | 86 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.907 | 45 |  |
| eczas | 87 | 0.256 | 32 | pass | FAIL | FAIL | FAIL | pass | pass | 0.005 | 3 | pos unrealistic, neg unrealistic, snr open |
| wmbus | 88 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.122 | 6 |  |
| tones | 91 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.064 | 1 |  |
| sarsat | 94 | 0.256 | 8 | pass | pass | pass | pass | pass | - | 0.046 | 2 |  |
| mt63 | 97 | 2 | 8 | pass | FAIL | FAIL | pass | pass | pass | 0.033 | 3 | pos unrealistic, neg unrealistic |
| zwave | 100 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.070 | 58 |  |
| fpv | 103 | 20 | 3 | pass | pass | pass | pass | pass | pass | 0.423 | 134 |  |
| mscatter | 106 | 2 | 8 | lock | lock | lock | lock | pass | lock | 0.035 | 0 |  |
| cquam | 109 | 2 | 3 | pass | FAIL | FAIL | weak | pass | pass | 0.063 | 4 | pos unrealistic, neg unrealistic |
| fsq | 115 | 2 | 8 | pass | pass | pass | pass | pass | pass | 0.026 | 8 |  |
| ifkp | 118 | 2 | 8 | pass | pass | pass | pass | pass | pass | 0.024 | 19 |  |
| sitor | 121 | 2 | 3 | pass | FAIL | FAIL | pass | pass | - | 0.069 | 20 | pos unrealistic, neg unrealistic |
| dgps | 122 | 0.96 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| dme | 123 | 2.4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| kaal | 124 | 0.24 | 32 | pass | pass | pass | weak | pass | - | 0.054 | 1264 |  |
| kavb | 125 | 2.4 | 3 | pass | pass | pass | pass | pass | pass | 1.719 | 3232 |  |
| kapp | 126 | 0.25 | 3 | pass | pass | pass | pass | pass | pass | 0.289 | 2 |  |
| kaft | 127 | 2.4 | 3 | pass | FAIL | pass | pass | pass | pass | 0.399 | 2 | pos unrealistic |
| eloran | 128 | 0.5 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| mpt1327 | 129 | 0.24 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| fusion | 130 | 2 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| ltr | 131 | 0.48 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| edacs | 132 | 0.48 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| kait | 133 | 1 | 32 | FAIL | - | - | - | pass | pass | 0.120 | 0 | clean open; (real time) |
| kaws | 135 | 2 | 32 | FAIL | - | - | - | pass | - | 0.045 | 0 | clean generator; (real time) generator sends silence |
| ardop | 142 | 2 | 8 | pass | FAIL | pass | pass | pass | pass | 0.077 | 4 | pos unrealistic |
| goesdcs | 143 | 1 | 8 | pass | pass | pass | pass | pass | pass | 0.055 | 3 |  |
| modeac | 144 | 4 | 3 | pass | pass | pass | weak | pass | pass | 0.284 | 72 |  |
| bletracker | 145 | 4 | 3 | pass | pass | pass | pass | pass | pass | 0.243 | 491 |  |
| satbeacon | 146 | 2 | 8 | pass | pass | weak | pass | pass | pass | 0.166 | 20 |  |
| milhf | 147 | 2 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| klw | 148 | 0.96 | 3 | pass | pass | pass | pass | pass | - | 0.079 | 4 |  |
| tdcs | 149 | 0.96 | 3 | pass | pass | pass | pass | pass | - | 0.097 | 3 |  |
| nfc | 150 | 4 | 3 | pass | pass | pass | weak | FALSE | pass | 0.379 | 4121 | noise no check |
| sbas | 152 | 4 | 8 | pass | FAIL | FAIL | pass | pass | - | 0.405 | 14 | pos unrealistic, neg unrealistic |
| asterix | 153 | 2 | 32 | FAIL | - | - | - | pass | - | 0.015 | 0 | clean no signal; (real time) generator sends silence |
| argos | 154 | 0.256 | 3 | pass | pass | pass | FAIL | pass | - | 0.040 | 1 | snr threshold |
| sigfox | 155 | 1.024 | 3 | pass | pass | pass | pass | pass | - | 0.059 | 1 |  |
| isdbs | 156 | 48 | 1.5 | pass | pass | pass | pass | pass | - | 6.672 | 37440 |  |
| marinevhf | 157 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| horus | 158 | 0.96 | 8 | pass | pass | pass | pass | pass | - | 0.028 | 1 |  |
| p25p2 | 182 | 2 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| lorawanwide | 202 | 4 | 3 | pass | pass | pass | pass | pass | - | 0.376 | 6 |  |
| lorasat | 203 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.092 | 1 |  |
| pskd | 204 | 2 | 3 | lock | lock | lock | lock | pass | - | 0.099 | 0 |  |
| pactor | 205 | 2 | 3 | pass | pass | FAIL | pass | pass | - | 0.072 | 2 | neg unrealistic |
| esubghz | 206 | 2 | 3 | pass | pass | pass | pass | pass | FAIL | 0.083 | 5 | tx open |
| roshf | 207 | 2 | 32 | pass | FAIL | FAIL | pass | pass | - | 0.177 | 11 | pos unrealistic, neg unrealistic |
| ssdv | 208 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.095 | 1 |  |
| ebicab | 209 | 10 | 3 | pass | pass | pass | pass | pass | - | 0.274 | 348 |  |
| inmsat | 210 | 6 | 8 | pass | FAIL | pass | FAIL | pass | - | 0.194 | 52 | pos open, snr open |
| iptv | 211 | 2 | 32 | FAIL | - | - | - | pass | - | 0.040 | 0 | clean no signal; (real time) generator sends silence |
| nrf24 | 212 | 8 | 3 | pass | pass | pass | pass | pass | - | 0.377 | 1383 |  |
| atis | 213 | 1 | 3 | pass | pass | pass | FAIL | pass | - | 0.077 | 1 | snr open |
| shortwave | 214 | 2 | 3 | lock | lock | lock | lock | lock | - | 0.141 | 0 |  |
| gk2alrit | 232 | 2.4 | 3 | pass | pass | pass | pass | pass | - | 0.090 | 20 |  |
| gk2ahrit | 233 | 10 | 3 | pass | pass | pass | pass | pass | - | 0.547 | 1094 |  |
| fy4lrit | 234 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.148 | 6 |  |
| fy4ahrit | 235 | 4 | 3 | pass | pass | pass | pass | pass | - | 0.283 | 75 |  |
| fy2svissr | 236 | 2.4 | 3 | pass | pass | pass | pass | pass | - | 0.414 | 5 |  |
| fy3ahrpt | 237 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.898 | 886 |  |
| fy3mpt | 238 | 20 | 3 | pass | pass | pass | pass | pass | - | 6.155 | 6840 |  |
| pactor2 | 239 | 2 | 32 | pass | pass | FAIL | pass | pass | - | 0.196 | 8 | neg unrealistic; (real time) |
| pactor3 | 240 | 2 | 32 | pass | FAIL | FAIL | pass | pass | - | 0.220 | 8 | pos unrealistic, neg unrealistic; (real time) |
| pactor4 | 241 | 2 | 32 | FAIL | - | - | - | pass | - | 0.109 | 0 | clean open; (real time) |
| selcal | 242 | 2 | 8 | pass | FAIL | FAIL | pass | pass | - | 0.024 | 1 | pos unrealistic, neg unrealistic |
| sensors | 243 | 0.25 | 3 | pass | pass | pass | pass | pass | - | 0.054 | 2 |  |
| sferics | 245 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.055 | 8 |  |
| thread | 246 | 4 | 3 | pass | pass | pass | pass | pass | - | 0.164 | 893 |  |
| bgan | 247 | 2 | 3 | pass | pass | pass | pass | pass | - | 0.142 | 144 |  |
| noaaapt | 262 | 1.024 | 3 | pass | pass | weak | pass | FALSE | - | 0.073 | 5 | noise no check |
| noaahrpt | 263 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.328 | 16 |  |
| noaagac | 264 | 8 | 3 | pass | pass | pass | pass | FALSE | - | 0.439 | 238 | noise no check |
| noaadsb | 265 | 1.024 | 8 | pass | pass | pass | pass | FALSE | - | 0.045 | 9 | noise no check |
| meteorhrpt | 266 | 6 | 3 | pass | pass | pass | pass | FALSE | - | 0.707 | 238 | noise no check |
| elektrordas | 267 | 45 | 1.6 | pass | pass | pass | pass | FALSE | - | 5.501 | 5654 | noise no check |
| arktikardas | 268 | 45 | 1.6 | pass | pass | pass | pass | FALSE | - | 4.205 | 5654 | noise no check |
| arktikaggak | 269 | 1.024 | 32 | FAIL | - | - | - | pass | - | 0.037 | 0 | clean open; (real time) |
| awsl | 270 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.608 | 515 |  |
| msgraw | 271 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.501 | 1081 |  |
| himawaricast | 272 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.261 | 180 |  |
| geonetcast | 273 | 52 | 1.3 | pass | pass | pass | pass | pass | - | 4.504 | 1243 |  |
| goesgrb | 292 | 20 | 3 | pass | pass | pass | pass | pass | - | 1.194 | 1578 |  |
| goesgvar | 293 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.573 | 22 |  |
| goescda | 294 | 1 | 3 | pass | pass | pass | pass | pass | - | 0.100 | 53 |  |
| goesmdl | 295 | 3 | 3 | pass | pass | pass | pass | FALSE | - | 0.207 | 316 | noise no check |
| gk2auhrit | 296 | 30 | 2.3 | pass | pass | pass | weak | pass | - | 1.875 | 2542 |  |
| gk2acdas | 297 | 8 | 3 | pass | pass | pass | pass | pass | - | 0.810 | 1827 |  |
| sband | 298 | 10 | 3 | pass | pass | pass | pass | pass | - | 0.505 | 944 |  |
| geoscan | 299 | 0.25 | 3 | lock | lock | lock | lock | lock | - | 0.032 | 0 |  |
| lucky7 | 300 | 0.25 | 3 | lock | lock | lock | lock | lock | - | 0.062 | 0 |  |
| spino | 301 | 0.25 | 3 | lock | lock | lock | lock | lock | - | 0.051 | 0 |  |
| adsl | 302 | 2 | 3 | pass | pass | pass | pass | pass | pass | 0.297 | 13 |  |
| smartnet | 322 | 0.96 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| tetradmo | 323 | 0.24 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| opensky | 324 | 0.24 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| cb | 342 | 1 | 3 | pass | pass | pass | FAIL | pass | - | 0.302 | 51 | snr threshold |
| frs | 343 | 2 | 8 | pass | pass | pass | FAIL | pass | - | 0.083 | 3 | snr threshold |
| pmr446 | 344 | 1 | 3 | pass | FAIL | FAIL | FAIL | pass | - | 0.103 | 2 | pos open, neg open, snr threshold |
| hamfm | 345 | 2 | 8 | pass | pass | pass | FAIL | pass | - | 0.113 | 4 | snr threshold |
| hfvoice | 346 | 0.25 | 32 | FAIL | - | - | - | pass | - | 0.086 | 0 | clean open; (real time) |
| mdc1200 | 347 | 2 | 8 | pass | pass | pass | FAIL | pass | - | 0.125 | 3 | snr threshold |
| wmic | 348 | 10 | 7 | FAIL | - | - | - | pass | - | 0.392 | 0 | clean open; (real time) |
| elrs | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| flysky | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| frsky | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| mavlink | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| rclink | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| reticulum | 362 | 8 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| btc | 382 | 20 | 3 | pass | pass | pass | pass | pass | - | 0.440 | 18 |  |
| fanet | 402 | 1 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| ogn | 403 | 1 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| gbas | 404 | 0.25 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| ldacs | 405 | 2 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| aeromacs | 406 | 5.6 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| eot | 407 | 1 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| dsrc | 408 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| its | 409 | 10 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| cv2x | 410 | 15.36 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| ermes | 411 | 1 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| mradar | 412 | 10 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| wradar | 413 | 10 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| milstd110 | 432 | 2 | 8 | pass | FAIL | FAIL | pass | pass | - | 0.041 | 9 | pos unrealistic, neg unrealistic |
| stanag4285 | 433 | 2 | 3 | lock | FAIL | FAIL | lock | pass | - | 0.087 | 0 | pos unrealistic, neg unrealistic |
| arqe | 434 | 2 | 32 | pass | pass | FAIL | pass | pass | - | 0.032 | 376 | neg unrealistic |
| arqn | 435 | 2 | 32 | pass | pass | FAIL | pass | pass | - | 0.014 | 425 | neg unrealistic |
| autospec | 436 | 2 | 32 | pass | pass | FAIL | pass | pass | - | 0.013 | 204 | neg unrealistic |
| spreadhf | 437 | 2 | 32 | pass | pass | FAIL | pass | pass | - | 0.027 | 195 | neg unrealistic |
| gtor | 438 | 2 | 3 | pass | pass | FAIL | FAIL | pass | - | 0.099 | 70 | neg unrealistic, snr open |
| efr | 439 | 2 | 3 | pass | pass | FAIL | pass | pass | - | 0.110 | 34 | neg unrealistic |
| piccolo | 440 | 2 | 8 | pass | pass | FAIL | pass | pass | - | 0.044 | 18 | neg unrealistic |
| coquelet | 441 | 2 | 8 | pass | pass | FAIL | pass | pass | - | 0.445 | 16 | neg unrealistic |
| qrss | 462 | 0.6 | 32 | FAIL | - | - | - | pass | - | 0.021 | 0 | clean too short; (real time) |
| dfcw | 463 | 0.6 | 32 | FAIL | - | - | - | pass | - | 0.028 | 0 | clean too short; (real time) |
| fsk441 | 464 | 0.6 | 3 | pass | FAIL | FAIL | pass | pass | - | 0.037 | 1 | pos unrealistic, neg unrealistic |
| iscat | 465 | 0.6 | 8 | pass | FAIL | FAIL | pass | pass | - | 0.061 | 1 | pos unrealistic, neg unrealistic |
| pi4 | 466 | 0.6 | 32 | pass | FAIL | FAIL | pass | pass | - | 0.027 | 1 | pos unrealistic, neg unrealistic |
| throb | 467 | 0.6 | 32 | pass | pass | FAIL | pass | pass | - | 0.043 | 16 | neg unrealistic |
| umts | 482 | 7.68 | 3 | pass | FAIL | FAIL | pass | pass | - | 0.343 | 2 | pos open, neg open |
| supersid | 502 | 0.25 | 8 | lock | lock | lock | lock | lock | - | 0.033 | 0 |  |
| riometer | 503 | 2 | 32 | FAIL | - | - | - | pass | - | 0.076 | 0 | clean open; (real time) |
| pulsar | 504 | 2.4 | 29.2 | lock | lock | lock | lock | pass | - | 0.057 | 0 |  |
| erminaz | 512 | 0.25 | 3 | lock | lock | lock | lock | lock | - | 0.050 | 0 |  |
| xb_npp | 532 | 25 | 2.8 | pass | pass | pass | pass | pass | - | 1.857 | 5053 |  |
| xb_jpss2 | 533 | 40 | 1.8 | pass | pass | pass | pass | pass | - | 2.903 | 4194 |  |
| xb_aqua | 534 | 15 | 3 | pass | pass | pass | pass | pass | - | 1.452 | 5335 |  |
| xb_terra | 535 | 45 | 1.6 | pass | pass | pass | pass | pass | - | 3.669 | 1238 |  |
| xb_metopd | 536 | 45 | 1.6 | pass | pass | pass | pass | pass | - | 6.912 | 12917 |  |
| xb_metopsg | 537 | 140 | 0.5 | pass | pass | pass | pass | pass | - | 22.530 | 5133 |  |
| xb_landsatx | 538 | 300 | 0.2 | pass | pass | pass | pass | pass | - | 39.719 | 3451 |  |
| xb_landsats | 539 | 6 | 3 | pass | pass | pass | pass | pass | - | 0.586 | 262 |  |
| xb_probas | 540 | 4 | 3 | pass | pass | pass | pass | pass | - | 0.244 | 231 |  |
| xb_probav | 541 | 45 | 1.6 | pass | pass | pass | pass | pass | - | 3.764 | 5240 |  |
| xb_gcomw | 542 | 20 | 3 | pass | pass | pass | pass | pass | - | 1.592 | 2860 |  |
| xb_gcomc | 543 | 90 | 0.8 | pass | pass | pass | pass | pass | - | 10.485 | 10397 |  |
| xb_stereo | 544 | 3 | 3 | pass | pass | pass | pass | pass | - | 0.266 | 315 |  |
| xb_oceansat2 | 545 | 35 | 2 | pass | pass | pass | pass | FALSE | - | 2.831 | 53 | noise open |
| xb_oceansat3 | 546 | 3 | 3 | pass | pass | pass | pass | pass | - | 0.321 | 255 |  |
| xb_saral | 547 | 3 | 3 | pass | pass | pass | pass | pass | - | 0.321 | 113 |  |
| xb_oceansat3x | 548 | 120 | 0.6 | pass | pass | pass | pass | pass | - | 8.801 | 5290 |  |
| xb_saralx | 549 | 25 | 2.8 | pass | pass | pass | pass | pass | - | 1.943 | 4348 |  |
| xb_kanopus | 550 | 90 | 0.8 | pass | pass | pass | pass | pass | - | 10.460 | 5866 |  |
| xb_dmsp | 551 | 2 | 3 | pass | pass | pass | pass | FALSE | - | 0.198 | 11855 | noise no check |
| zombie | 552 | 1 | 3 | lock | lock | lock | lock | pass | - | 0.078 | 0 |  |
| whatsat | 553 | 1 | 3 | lock | lock | lock | lock | pass | - | 0.046 | 0 |  |
| uhfsatcom | 554 | 10 | 7 | pass | pass | pass | pass | pass | - | 0.080 | 15 |  |
| gmr1 | 555 | 10 | 7 | pass | pass | pass | pass | pass | - | 0.096 | 10 |  |
| starlink | 556 | 20 | 3 | pass | pass | pass | pass | pass | - | 0.216 | 1 |  |
| oneweb | 557 | 20 | 3 | pass | FAIL | pass | pass | pass | - | 0.287 | 1 | pos open |
| tdrs | 558 | 2 | 3 | lock | lock | lock | lock | pass | - | 0.173 | 0 |  |
| css | 559 | 1 | 3 | lock | lock | lock | lock | pass | - | 0.101 | 0 |  |
| tsikada | 560 | 1 | 3 | lock | lock | lock | lock | pass | - | 0.101 | 0 |  |
| gonets | 561 | 4 | 3 | pass | pass | pass | pass | pass | - | 0.061 | 66 |  |
| tiantong | 562 | 10 | 7 | pass | pass | pass | pass | pass | - | 0.077 | 8 |  |
| navic | 563 | 4.096 | 17.1 | FAIL | - | - | - | pass | - | 0.606 | 0 | clean open; (real time) |
| issdown | 564 | 1 | 3 | lock | lock | lock | lock | pass | - | 0.095 | 0 |  |
| wfbng | 602 | 20 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| xlink | 602 | 20 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| dectnrp | 603 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| mioty | 603 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| npr | 603 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| winmor | 603 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |
| xlinknb | 603 | 4 | 3 | - | - | - | - | - | - | 0.000 | 0 | no test signal |

## Fixes from the sweep

- **Remote ID and BLE recordings** (`core/src/engine/engine_start.cpp`): on a file both receivers got no centre frequency, so Remote ID
  assumed a channel on the centre and every packet failed its CRC (0 good, about 600 bad in 3 s of the test signal and of the
  transmitter's signal). They now get the recording's centre (the frequency entered for it) when it lies in the 2.4 GHz band. Remote ID
  now passes all six conditions (2963 packets in 3 s).
- **DAB generator** (`core/src/dab/dab_gen.cpp`) and **DMR protocol bridge** (`core/src/dmr/dmr_proto_rs.cpp`): `memcpy` from an empty
  vector's null `data()`. This is undefined behaviour, and UBSan stops on it in `test_iq_modes` under `DECT2_SANITIZE`.
- **`bench_modes`** shifted an offset-tuned mode's test signal by -offset instead of +offset. The signal of the generator sits at -offset,
  as a live radio sees it, so the narrow receivers measured CPU on a signal they never found. The sweep had the same bug and the same fix.
- Build: `DECT2_SANITIZE_RECOVER=ON` (with `DECT2_SANITIZE`) reports every undefined-behaviour site of a run instead of stopping at the first.
- `quick.yml`: optional `cmake_args`, `env`, `timeout` and `minutes` inputs, for sanitizer runs and long sweeps on one runner.

## Unrealistic conditions and generators (not decoder bugs)

- **+-2 kHz on HF and on SSB-audio decoders.** Below 30 MHz a 50 ppm crystal is off by at most 1.5 kHz. The HF digital decoders
  (RTTY, PSK, the keyboard modes, FT8 and the other WSJT modes, PACTOR, ARQ-E, STANAG, MIL-STD) work on the sideband audio, in a window
  the user puts the signal into. A 2 kHz shift takes the tones out of that window by design. The VHF WSJT modes (FSK441, ISCAT, PI4,
  QRSS, DFCW) and KAFT work the same way.
- **50 ppm sample clock for GNSS** (GNSS, SBAS, NavIC): 51 chips/s of code drift, far beyond the 0.5-2 ppm TCXO a GNSS receiver has.
- **12 dB in-band SNR** is below or at the threshold of the high-order modulations (DVB-T 64-QAM, DVB-C 256-QAM, DTMB, ATSC, ISDB-T,
  DRM, the HD Radio sidebands under the analog carrier, RDS in FM). For analog FM voice it is at the FM threshold, where the default squelch
  stays closed.
- **Picture lines and measurements on noise.** APT, GAC, DSB, the Meteor and Elektro / Arktika imagers, GOES mode-L, DMSP, the spectrum
  analyser and NFC count lines or measurements that carry no check, so they also count on noise. No message decodes.
- **Lock-only modes on noise** (AM, the general receiver, shortwave, SuperSID, spectrum tools): their lock flag means "signal present or
  audio flowing", and noise is a signal to them. The sweep shows `lock` and does not fail them.
- **Long-slot modes** (WSPR 2 min, JT65 / JT9 / JT4 / FST4 / Q65 / PI4 60 s, QRSS / DFCW minutes per character, NAVTEX messages, SSTV and
  fax pictures) need more than the short sweep's 32 s. The long run (`ONAIR_TEST_LONG=1`) covers them.
- **KAWS generator**: the test signal waits for the next slot of the computer clock (`kaws.align`). When it is made faster than real time
  it is silence.
- **Internet TV and ASTERIX** have no radio signal of their own. Their generators send silence on purpose.
- **Slot decoders when the file runs faster than real time.** FT8 and the other slot modes decode on their own thread and skip slots they
  cannot keep up with. In the app a file plays at its own speed, so the sweep's last try plays it at the radio's speed. FT8 decodes there:
  16 messages in 32 s.

## Harness notes

- The first CI run counted nothing for 118 modes. There were two causes. The offset-tuned modes were shifted the wrong way (fixed). The
  text and slot decoders count characters or decodes, not blocks, so the sweep now measures them by their own counts and by their saved
  results.
- The X-band modes (90-300 Msps) ran out of the child's CPU limit on the noise file (`CRASH(152)`, SIGXCPU). On noise their receiver
  searches far longer per second of signal than it decodes on the clean file. This is listed under CPU.

## CPU: the top 10 and what was tried

The ten modes that cost the most per second of signal (clean run, macOS CI runner, ATSC 1.0 left out because another worker owns it):

| mode | Msps | CPU s per s of signal |
|---|---|---|
| xb_landsatx | 300 | 39.72 |
| xb_metopsg | 140 | 22.53 |
| xb_gcomc | 90 | 10.48 |
| xb_kanopus | 90 | 10.46 |
| xb_oceansat3x | 120 | 8.80 |
| xb_metopd | 45 | 6.91 |
| isdbs | 48 | 6.67 |
| fy3mpt | 20 | 6.16 |
| elektrordas | 45 | 5.50 |
| geonetcast | 52 | 4.50 |

All ten are wideband downlinks at 20 to 300 Msps (the X-band family, ISDB-S, FY-3 MPT, Elektro / Arktika RDAS, GEONETCAST). Their cost
grows with the sample rate: about 0.1-0.15 CPU s per Msps for the X-band receiver. They are used on recordings, not in real time. A
profile of `xb_gcomc` (90 Msps) put the time in the receiver's per-sample loop (`XbandReceiver::Impl::feed`) and the CCSDS deframer.
Two changes that keep the output the same were tried: the SNR worked out per report instead of per symbol, and a bulk bit path through
the deframer. Neither made a measurable difference (5.55 against 5.51-5.60 CPU s/s, the same blocks decoded), so both were left out.
No optimisation shipped in this branch.

Found on the way: on the **noise** file the X-band receivers (`xb_jpss2`, `xb_probav`, `xb_saralx` and others) went over the child's CPU
limit, 300 CPU s for under a second of signal (`CRASH(152)`, SIGXCPU). Their search with no signal costs far more than decoding a locked
signal. That is the first thing to optimise in this family.

Before the offset fix, the first CI run showed the cost of the modes that never found their signal. For example COQUELET used 4.2 CPU s/s
while it searched, against 0.29 when it decodes. The searching state of several narrow modes is much dearer than the decoding one.

## Still failing (open)

These failures are not explained by the condition. They are in `qa_known.inc` as `open`, so the test passes today, but each needs a look at its mode.

- `hdr/pos`: fails with the offset (to investigate)
- `hamdrm/tx`: its transmitter's signal does not decode (to investigate)
- `js8/snr`: fails at 12 dB (to investigate)
- `js8/tx`: its transmitter's signal does not decode (to investigate)
- `specx/clean`: does not decode its own test signal through the engine (to investigate)
- `specx/tx`: its transmitter's signal does not decode (to investigate)
- `tetra/tx`: its transmitter's signal does not decode (to investigate)
- `astro/clean`: does not decode its own test signal through the engine (to investigate)
- `astro/tx`: its transmitter's signal does not decode (to investigate)
- `iono/clean`: does not decode its own test signal through the engine (to investigate)
- `iono/tx`: its transmitter's signal does not decode (to investigate)
- `qo100/clean`: does not decode its own test signal through the engine (to investigate)
- `qo100/tx`: its transmitter's signal does not decode (to investigate)
- `nr/clean`: does not decode its own test signal through the engine (to investigate)
- `nr/tx`: its transmitter's signal does not decode (to investigate)
- `hfbeacon/clean`: does not decode its own test signal through the engine (to investigate)
- `hfbeacon/tx`: its transmitter's signal does not decode (to investigate)
- `nbiot/neg`: fails with the offset (to investigate)
- `dect/clean`: does not decode its own test signal through the engine (to investigate)
- `uat/clean`: does not decode its own test signal through the engine (to investigate)
- `uat/tx`: its transmitter's signal does not decode (to investigate)
- `eczas/snr`: fails at 12 dB (to investigate)
- `kait/clean`: does not decode its own test signal through the engine (to investigate)
- `esubghz/tx`: its transmitter's signal does not decode (to investigate)
- `inmsat/pos`: fails with the offset (to investigate)
- `inmsat/snr`: fails at 12 dB (to investigate)
- `atis/snr`: fails at 12 dB (to investigate)
- `pactor4/clean`: does not decode its own test signal through the engine (to investigate)
- `arktikaggak/clean`: does not decode its own test signal through the engine (to investigate)
- `pmr446/pos`: fails with the offset (to investigate)
- `pmr446/neg`: fails with the offset (to investigate)
- `hfvoice/clean`: does not decode its own test signal through the engine (to investigate)
- `wmic/clean`: does not decode its own test signal through the engine (to investigate)
- `gtor/snr`: fails at 12 dB (to investigate)
- `umts/pos`: fails with the offset (to investigate)
- `umts/neg`: fails with the offset (to investigate)
- `riometer/clean`: does not decode its own test signal through the engine (to investigate)
- `xb_oceansat2/noise`: decodes on noise (false decode, to investigate)
- `oneweb/pos`: fails with the offset (to investigate)
- `navic/clean`: does not decode its own test signal through the engine (to investigate)

## Not done

- The iq_modes crash on Windows and Linux (3.8 full CI run 42841590406: heap corruption `0xc0000374`, `malloc(): invalid size`). It
  happens in the engine-spectrum stage, where six Engines run at once, after all the receiver runs. ASan / UBSan runs of `iq_modes` on CI
  found two undefined-behaviour sites first (DAB generator, DMR bridge, both fixed). The ASan run that should reach the crash was still
  running when this was written (quick.yml run 42921904976 with `DECT2_SANITIZE=ON -DDECT2_SANITIZE_RECOVER=ON`).
- The 40 `open` failures above.
- The CPU optimisations (see above).
