# Sensitivity and lock-time measurement (`sens_sweep`)

Goal of the campaign: every decoder locks faster and decodes at a lower SNR. Without numbers nobody can claim that, so every mode is
measured the same way, with the same signals, noise and impairments, and a change is compared against a committed baseline.

## What it measures

For a mode and a *scenario* (an impairment, or none) it plays the mode's clean test signal through the impairment chain at a series of SNRs into
a **fresh receiver**, many seeded trials per point, and reports:

| figure | meaning |
|---|---|
| success rate | a trial succeeds when the receiver counted at least half as many *valid units* as it does on a clean run. A valid unit is what the receiver itself counts as good: its CRC / Reed-Solomon / LDPC+BCH / FIB check passed (`blocksOk`, `rsClean+rsCorrected`, `fibOk`, RDS groups). FM audio: the 1 kHz tone is 20 dB over everything else. |
| time to lock | seconds of signal from the start of the trial to the first valid unit (polled every 50 ms of signal, so 50 ms resolution). Median and 90th percentile over the successful trials, at the lowest measured point 3 dB or more above the threshold. |
| threshold SNR | lowest SNR with at least 90 % success, scanning down from the top until the first point below 90 % |
| PER | `bad / (good + bad)` of the receiver's own counters at the threshold (mean over trials) where the decoder exposes a failure counter |

A trial starts at a random point of the test signal (seeded), so lock time includes waiting for the next frame/sync like a user tuning in.
Trials are deterministic: seed = hash(mode, scenario, SNR, trial); the result of a trial does not depend on which thread ran it
(`DECT2_NOPACE` is set so threaded decoders wait instead of dropping frames). Gaussian noise is `PortableNormal`, identical on every platform.
The compiler and standard library can still change a floating point result in the last digit, so the committed baseline is made on the
Linux runner (the `Sensitivity sweep` workflow) and compared with runs from the same place.

### SNR definition

`SNR = Pa / (N0 * B)`. `Pa` is the mean power of the *active* part of the clean signal (blocks of 512 samples within 10 dB of the strongest
2 %, so a bursty mode such as ADS-B or Iridium is measured on its bursts, not diluted by the idle time). `B` is the mode's noise bandwidth
(`Mode::bwHz`, the channel width the standard quotes its C/N in: 7.61 MHz for DVB-T/T2 8 MHz, 1.536 MHz DAB, 200 kHz FM, 25 kHz for most
narrowband data modes...). `N0` is the white noise density, so the noise power per complex sample is `Pa * (fs / B) / 10^(SNR/10)`. For test
signals with several carriers (ADS-B aircraft, ACARS channels, AIS 1+2) `Pa` is the sum, so the per-carrier SNR is lower; use the figures to
compare versions, not as a datasheet sensitivity. FM and the single-signal modes are directly comparable with the usual C/N numbers.

### Scenarios

| name | impairment |
|---|---|
| `awgn` | white noise only |
| `cfo2ppm` / `cfo20ppm` | carrier error of a 2 / 20 ppm radio at the mode's frequency (20 ppm capped at a quarter of the channel) |
| `offcentre` | channel moved off the centre by 20 % of its width + 5 % of the sample rate |
| `sro50ppm` | sample clock 50 ppm fast (cubic interpolation) |
| `echo` | static echo, -6 dB, at about half the mode's guard interval (`Mode::echoUs`) |
| `tu6` | Rayleigh fading, COST 207 6-tap Typical Urban, Jakes Doppler (`Mode::fdHz`, 10 Hz default: pedestrian) |
| `rician` | the same with a line of sight, K = 10 dB |
| `phasenoise` | Wiener phase noise, 50 Hz linewidth |
| `combined` | 2 ppm carrier + 20 ppm clock + echo + phase noise, together |

Profiles are scaled to unit total power, so the mean signal power (and the SNR) is unchanged by multipath. DAB RA4 and DVB-T Portable/Pedestrian
profiles are not separate: TU6 with the mode's Doppler is the standard stand-in; add a profile in `sens_impair.h::profileTaps` for others.
Wraps/clipping and lost samples are covered by the existing `tests/*/test_*_impair.cpp` tests (pass/fail), not swept here.

### Search

Coarse: 4 dB steps from the mode's top SNR down, `trials/3` trials a point, until the first point below 90 %. Fine: 1 dB steps (2 dB in
`--quick`) below the last passing coarse point with the full trial count, until the first point below 90 %. Every measured point is in the JSON.

## Running it

```
cmake -S . -B build -G Ninja && cmake --build build --target sens_sweep
tools/sens/run.sh --quick --mode 'dab|fm'            # AWGN, TU6, 20 ppm carrier, combined; 4 trials a point; a few minutes
tools/sens/run.sh --full  --mode dvbt                # all scenarios, 1 dB steps, 12 trials (--trials N to change)
tools/sens/run.sh --quick --mode dvbt2 --scenario 'awgn|tu6'
./build/sens_sweep --list                            # modes and scenarios      ./build/sens_sweep --gaps   # every mode of the tuning table: test signal? in the harness?
```

`run.sh` writes `build/sens-out/sens-<mode>.json` and `sens-baseline.json` and prints the delta table against
`tests/sens/baseline/sens-baseline.json` (`tools/sens/compare.py`). It exits 1 when a threshold is worse by more than 0.3 dB, a median lock time
is more than 20 % (and 50 ms) longer, or a scenario lost its threshold. Quick runs have 2 dB threshold steps, so against a full baseline the dB
tolerance is 2 dB: quick catches big regressions, full claims fractions of a dB. `--update-baseline` merges the run into the committed file.

**On GitHub** (the Mac's battery and build queue are busy): Actions > *Sensitivity sweep* > Run workflow (`.github/workflows/sens-quick.yml`),
inputs: mode regex, tier (quick/full), scenario regex, trials, update_baseline. It builds `sens_sweep` only, runs it, compares with the baseline
and uploads the JSON as the artifact `sens-results`. Start it with `gh workflow run sens-quick.yml -f modes='dab|fm' -f tier=quick`, wait with
`gh run watch <id> --exit-status`.

### For a per-mode agent

1. Before changing anything: `tools/sens/run.sh --full --mode <id>` (or the workflow) and keep `sens-<id>.json` as *before*.
2. Make the change, run again, `python3 tools/sens/compare.py before.json after.json` (both are `{"modes":[...]}` files; `sens-baseline.json` has that shape).
3. Claim a gain only from a full run with a threshold change above the step and the same seeds; then `--update-baseline` and commit the file.
4. A mode missing from the registry: add a block to `tools/sens/sens_modes.h` (clean generator, receiver set-up as the engine does it, `blocks<Tel>` or your own measure, `bwHz`).

## Coverage

In the harness (generator + receiver wired like `tests/engine/test_iq_modes.cpp`): `dvbt2 dvbt atsc dab isdbt fm fm-rds dvbs2 dtmb atv dmr drm
adsb sonde ais acars aero iridium mesh(heavy) pager packet vdl2 dstar nxdn dpmr tetra p25`. A mode whose clean reference run gives no valid
output is reported as "skipped" (receiver wiring in the harness needs a look) instead of producing numbers.

**Gap list** (`sens_sweep --gaps` prints the current state):

* No built-in test signal at all: `wb dgps dme eloran mpt1327 fusion ltr edacs milhf marinevhf p25p2` (DVB-C has the `tests/dvbc` generator, DVB-T2 MISO/other
  PLP configurations only the one in the table).
* Test signal exists, not yet in the harness (needs a receiver block in `sens_modes.h`; most are `blocks<Tel>` plus a prepGeneric, a few need long
  signals): `gnss` (C/N0 defined, not SNR), `marine` NAVTEX/DSC (25 s signals), `inmc` (36 s), `atsc3`, `dvbc`, `hfdl`, `hdr cdr hfdig airband ism ndb vorils m17 cell cw
  psk js8 gsmr specx specan uplink orbcomm mac astro ale ble iono qo100 wifi general rid nr time meteor goes cubesat globalstar vdes hrpt zigbee flarm
  hfbeacon dji nbiot halow loraraw codar datawell multitone same ant dect tetrapol elektro lbj vdl4 uat wisun eczas wmbus tones sarsat mt63 zwave fpv
  mscatter cquam fsq ifkp kaal kavb kapp kaft kait kaws ardop goesdcs modeac bletracker satbeacon klw tdcs nfc sbas asterix argos sigfox isdbs horus
  lorawanwide lorasat`, and the weak-signal HF modes FT8/FT4/WSPR (no generator found in the repo), APT/LRPT (`meteor`).
* Not measured by design: speech quality of the voice modes (only the signalling/CRC units), and the image content of satellite modes.

## Baseline

`tests/sens/baseline/sens-baseline.json`: full sweep (8 trials a point, 1 dB steps) of 25 modes, made on the GitHub Linux runner 2026-10-23 (push to branch
`sens-baseline-run-030` runs the workflow; the build workers run the same as ctest `sens_full`, label `sens-full`, about 2 h). Not in it: `tetra` (no valid
output on its clean test signal: the harness wiring needs a look, probably `setCenterHz`/scene) and `mesh` (heavy, `--heavy`). Regenerate with the workflow
(`update_baseline`). Known harness limits: the sample clock uses a 4-point cubic (a fraction of a dB loss at the band edge of critically sampled OFDM);
multipath delays are rounded to whole samples; lock time has the 50 ms poll resolution (T2 and other threaded decoders also lag by their queue).

## DVB-T / DVB-T2 / DVB-C detail sweeps

`sens_dvb` (tests/dvbt/sens_dvb.cpp) and `sens_dvbc` (tests/dvbc/sens_dvbc.cpp) sweep one parameter set of these receivers with more
channels than the shared harness: static TU6, a 0 dB echo at 0.9 of the guard (`--chan echo0`, `SENS_ECHO=frac:dB` for others), TU6 or
Rician with Doppler (`--doppler`), carrier and clock offsets. C/N is in the occupied band (the K carriers). A trial passes when the stream
is clean from 100 ms after the first good packet (T2: from the frame after the first good one), so these thresholds are laxer than the
standards' BER points. `SENS_VERBOSE=1` prints the receiver's state, the data MER and the impulse response.

```
build/sens_dvb t 1 3 2 1 --snr 14:19:0.5 --trials 6                   # DVB-T 8K GI 1/4 64-QAM 2/3, AWGN
build/sens_dvb t 0 3 1 0 --chan tu6 --doppler 100 --snr 16:28:2       # 2K 16-QAM 1/2, TU6 at 100 Hz
build/sens_dvb t2 1 2 1 2 2 --snr 12:18:0.5                           # T2 8K GI 1/8 PP2 64-QAM 2/3
build/sens_dvbc 2 --snr 22:28:1                                       # DVB-C 64-QAM (Es/N0)
```

Reference figures (perfect channel estimation, from the standards' own tables). EN 300 744 V1.6.2 Table A.1, C/N for BER 2e-4 after
Viterbi, AWGN: QPSK 1/2 3.5, 16-QAM 1/2 9.3, 16-QAM 2/3 11.4, 64-QAM 2/3 16.7, 64-QAM 3/4 18.2 dB. TS 102 831 V1.2.1 Table 44, raw
(C/N)0 for BER 1e-7 after LDPC (64800 bits), AWGN, no pilots: QPSK 1/2 1.0, 16-QAM 1/2 6.0, 64-QAM 2/3 13.5, 256-QAM 2/3 17.8 dB, plus
0.29-0.53 dB for the pilots (Table 46; 8K PP2 0.39). L1-pre decodes at -4 dB (TS 102 831 clause 14.7).

## Measured sensitivity of every mode (2026-10-30, campaign feat34-quality)

Threshold SNR in dB (lowest SNR with at least 90 % success, 8 trials a point, 1 dB steps, full tier, GitHub runner; `-` = no 90 % point even at the top SNR). Mode order as the sweep lists them. Lower is better. These are the committed-harness numbers (windowed sinc clock error, audio tone found at its own frequency, per-mode phase noise: TV tuners 1 Hz, DRM 1 Hz, HDR 3 / 10 Hz, others 50 Hz). Narrowband modes under `offcentre` are moved outside their own channel, and `tu6` is Rayleigh fading with 10 Hz Doppler: gaps there are mostly physics, not receiver loss.

| mode | awgn | cfo2ppm | cfo20ppm | offcentre | sro50ppm | echo | tu6 | rician | phasenoise | combined |
|---|---|---|---|---|---|---|---|---|---|---|
| dvbt2 | 14 | 14 | 14 | 14 | 14 | 17 | 20 | 16 | 15 | 18 |
| dvbt | 15 | 15 | 15 | 15 | 15 | 16 | 22 | 16 | 15 | 16 |
| atsc | 15 | 15 | 15 | 15 | 15 | 17 | - | 22 | 15 | 17 |
| dab | 3 | 3 | 3 | 5 | 3 | 3 | 5 | 4 | 5 | 9 |
| isdbt | 18 | 18 | 18 | 18 | 18 | 20 | - | 19 | 19 | 20 |
| fm | 5 | 5 | 5 | 12 | 5 | 3 | - | 12 | 6 | 3 |
| fm-rds | 16 | 16 | 16 | 16 | 16 | 18 | 26 | 17 | 15 | 14 |
| dvbs2 | 3 | 3 | 3 | 3 | 3 | - | - | 4 | 4 | - |
| dtmb | 18 | 18 | 17 | 20 | 18 | 19 | 23 | 20 | 19 | 19 |
| atv | -4 | -4 | -4 | -4 | -5 | 9 -> **7** | - | 0 | - | - |
| dmr | 5 | 5 | 5 | - | 5 | 3 | 9 | 5 | 5 | 3 |
| drm | 16 | 16 | 16 | - | 16 | 20 | 24 | 17 | - | - |
| adsb | 14 | 14 | 14 | 14 | 14 | 13 | 22 | 14 | 14 | 12 |
| sonde | 12 | 13 | 12 | 13 | 14 | 13 | - | 17 | 13 | 9 |
| ais | 16 | 16 | 16 | - | 15 | 14 | 21 | 16 | 16 | 18 |
| acars | 7 | 7 | 7 | 12 | 10 | 5 | 19 | 9 | 7 | 5 |
| aero | 11 | 11 | 12 | 11 | 12 | 18 | - | 13 | - | - |
| iridium | 12 | 12 | 12 | 12 | 12 | 13 | 14 | 12 | 12 | 15 |
| globalstar | 0 | 0 | 0 | 3 | 0 | 0 | -3 | 0 | 0 | 0 |
| orbcomm | 8 | 8 | 8 | 7 | 8 | 5 | 17 | 9 | 8 | 6 |
| argos | 17 | 13 | 13 | 13 | 13 | 10 | 17 | 13 | 13 | 10 |
| sarsat | 0 | 1 | 5 | 5 | 1 | -2 | 3 | 0 | 3 | 0 |
| wisun | 10 | 10 | 11 | - | 10 | 15 | 17 | 11 | 10 | 15 |
| halow | 7 | 7 | 7 | 7 | 7 | 7 | 10 | 8 | 7 | 8 |
| zigbee | 2 | 2 | 2 | - | 3 | 6 | 13 | 3 | 2 | 7 |
| zwave | 10 | 13 | 9 | 22 | 10 | 17 | 13 | 10 | 10 | 13 |
| sigfox | 17 | 5 | 1 | - | 16 | 20 | - | 17 | - | - |
| ble | 8 | 8 | 8 | - | 8 | 14 | - | 9 | 8 | 17 |
| inmc | 0 | 0 | 0 | - | 0 | 0 | - | 0 | - | - |
| am | 27 | 27 | 27 | - | 27 | 28 | - | - | 27 | 29 |
| am-sync | 27 | 27 | 27 | - | 27 | 28 | - | - | - | - |
| ssb | 20 | - | - | - | - | 22 | - | - | - | - |
| hdr-fm | 16 | 17 | 17 | - | 16 | 22 | - | 17 | - | - |
| hdr-am | 37 | 24 | - | - | 36 | 37 | - | 40 | - | - |
| pager | 1 | 2 | 2 | - | 0 | 0 | 13 | 3 | 1 | 1 |
| packet | 5 | 5 | 5 | - | 5 | 5 | 21 | 8 | 5 | 5 |
| vdl2 | 12 | 12 | 12 | - | 11 | 14 | 15 | 13 | - | - |
| dstar | 8 | 7 | 9 | 8 | 8 | 8 | 21 | 9 | 8 | 8 |
| nxdn | 9 | 8 | 8 | - | 8 | 8 | 14 | 9 | 8 | 9 |
| dpmr | 7 | 7 | 7 | - | 7 | 7 | 12 | 8 | 10 | 9 |
| m17 | 5 | 5 | 6 | 5 | 4 | 5 | 5 | 4 | 5 | 5 |
| tetrapol | 10 | 10 | 17 -> **10** | - | 10 -> **11** | 8 | 13 | 11 | 12 | 10 |
| p25 | 6 | 6 | 8 | - | 6 | 6 | 13 | 9 | 6 | 7 |
| meteor | 8 | 5 | 8 | - | 5 | 3 | - | 7 | - | - |
| hrpt | 8 | 3 | 4 | - | 4 | 8 | - | 8 | 7 | 16 |
| goes | -1 | -1 | -1 | -1 | -1 | 1 | - | 0 | -1 | 1 |

`a -> b` marks a change made by this campaign (same seeds, same harness). Modes with `-` in the awgn column or in a long run of scenarios are the next candidates; the largest remaining gaps against the AWGN threshold are ATV echo (+11 after the fix), FM rician (+7), Z-Wave echo (+7), ACARS offcentre (+5) and every narrowband mode in TU6 fading.

### Harness corrections made on the way (why some old numbers moved)

- `sro50ppm` used a 4 point cubic interpolator that damaged wide modes by a filter that moved with the fractional position; a clean ATSC signal with 10 ppm read 19 dB data SNR. Now a 24 tap Kaiser sinc. ATSC at 30 dB, 50 ppm: good 4230 / bad 11006 before, 18356 / 0 after (the receiver was fine).
- FM, AM and SSB audio: the 1 kHz tone is searched +-1.5 Hz; at exactly 1000 Hz a 50 ppm clock error capped the audio SNR near 27 dB (FM `sro50ppm` 12 dB -> 5 dB).
- Phase noise linewidth per mode (`Mode::pnHz`); at the 50 Hz default no TV OFDM or ATSC receiver decoded even at 30 dB. False-lock rate on noise only is reported per mode.
