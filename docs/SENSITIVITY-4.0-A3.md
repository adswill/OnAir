# Sensitivity 4.0, lane A, part 3 (feat40-sensA3)

Same method as SENSITIVITY-4.0-A.md and -A2.md: `sens_curve` (the mode's generator signal, random start and carrier phase, white noise; fraction of the clean run's valid
units decoded; 4 to 8 trials a point), "before" = integ-40 at 4403f8d4b. Every kept change was also run noise-only (0 units in all), with a carrier error (20 ppm at the
mode's band, or the stated offset) and with a 50 ppm sample clock. All numbers are simulated signals only; the generators' deviations, skirts and levels are assumed values.
Hunt: the Argos find of part 2 (a fixed acquisition gate far above what the demodulator needs) was looked for in every burst / packet receiver; the sections below are the
receivers where a gap to theory was found and closed, then what was measured and left alone.

`sens_curve` gets `CURVE_SECS=<s>` (a longer trial, for modes with a slow frame rate: the MRZ sonde sends about one frame a trial at the default 5 s).
One guard test for the new gains: `sens40a3` (tests/sens/test_sens40a3.cpp: rows of mode, SNR, carrier, clock, need; noise-only runs; a +-0.26 dB level drift for Globalstar).
The modes with their own test (LTR, SAME, CubeSat, sondes) got their rows in those tests.

## 1. Wi-Fi HaLow: preamble gate 0.55 to 0.30

The STF detector (`halow_chain.cpp`) needs the normalised lag correlation above `kThr` for 64 samples. 0.55 is about 0 dB in the chain bandwidth (4 MHz): a 1 MHz
signal at 6 dB in 1 MHz (0 dB in the chain) never fired it, so the frames were lost before any decoding. 0.30 (0.35 is the same, 0.25 loses clean frames: 693 to 500
units, candidates collide) fires on them; the SIG field, channel estimate and data are untouched.

| HaLow (SNR in 1 MHz, mixed 1 / 2 / 4 MHz frames, 4 trials) | before: 9 / 7.5 / 6 / 4.5 / 3 dB | after |
|---|---|---|
| AWGN | 98 / 82 / 20 / 0.3 / 0 % | 98 / 87 / 60 / 30 / 12.6 % |
| +18 kHz carrier | | 98.5 / 86 / 61 / 31.5 / 12 % |
| 50 ppm clock | | 98 / 86 / 60.5 / 30 / 11 % |

About 1.5 dB at the 50 % point (6.7 to 5.2 dB). The wide frames carry the signal over 2 and 4 MHz, so their SNR in their own band is 3 to 6 dB below the figure on the
axis, which is why the curve is gradual; the remaining gap to a 1 MHz BPSK 1/2 receiver (about 1 to 2 dB for the narrow frames) is in the channel estimate and SIG decoding, not
the gate. Noise-only: 0 units. Guard: `sens40a3` (6 dB, 0 and +18 kHz and 50 ppm).

## 2. Globalstar: activity gate 2.0 / 1.0 dB to 0.8 / 0.4 dB

The receiver is an energy detector (power in each 1.23 MHz channel over a tracked floor, 10 ms slots, three in a row). The gate was 2 dB over the floor; the estimate of
a slot has a standard deviation of about 0.08 dB, so it was the floor tracker's stability, not the noise, that set it. At 0.8 dB the cliff goes from -2 dB (100 % at 0 dB,
0 % at -3 dB) to -5 dB (100 % at -4.5 dB, 12 % at -6), the same with 18 kHz carrier and 50 ppm clock. Noise-only 0 events, also with the level drifting +-0.26 dB in 7 s (the
floor follows quiet slots with a 0.3 weight and creeps up at 1 % a slot, a 0.5 dB slow gain change stays under the gate). About 3.5 dB. A real radio's AGC steps and an
adjacent strong channel leaking into the window are not simulated: the gate is one constant (`kOnDb` / `kOffDb`).

## 3. POCSAG and slow FLEX: a second decoder behind a 7 kHz channel filter

A2 found POCSAG 3 dB better behind a narrower filter and FLEX collapsing there (4 level 6400 baud needs 10 kHz). The pager receiver now runs a second complete decoder
(own filter, discriminator, ring and sync search, 25 kHz channels only) on the same samples. It decodes POCSAG and FLEX; the 4 level FLEX speeds (3200/4, 6400/4) do not survive 7 kHz, so there the first decoder carries them and the second one finds nothing. Its pages go to the first decoder's list through the same
`emit`, with a duplicate check (same address, function, type and text within 8 s from the other decoder is dropped); the code word counts of the telemetry are the larger of the
two per speed; its signal / lock flags are OR-ed. `setPocsagSecondPath(false)` switches it off. CPU: the POCSAG-only pipeline once more (about one more FIR at 96 kS/s and the
searches).

| POCSAG AWGN (SNR in 25 kHz, 6 trials) | -3 dB | -4.5 dB | -6 dB |
|---|---|---|---|
| before (one decoder, 10 kHz) | 70 % | 29 % | 6 % |
| second path 9 kHz | 74 % | 43 % | 10 % |
| second path 8 kHz | 74 % | 49 % | 15 % |
| second path 7 kHz (kept) | 77 % | 67 % | 17 % |
| second path 6 kHz | 81 % | 70 % | 34 % |

About 1.3 dB at the 50 % point with 7 kHz (-3.9 to -5.0 dB). 6 kHz is better still (1.5 dB) but with a 3.1 kHz carrier error the second path hears almost nothing (the filter edge
is in the upper tone; the first decoder is unchanged and decodes it as before, so nothing is lost: with +3.1 kHz at 0 dB the first decoder alone had 62 % and the guard row with both is 100 %). 7 kHz keeps a 2 kHz margin.
FLEX in the harness (mixed speeds, 6 trials): 0 dB 41 to 73 %, -1.5 dB 10 to 28 % (about 1 dB at the 50 % point), 3 dB and above unchanged (the 4 level speeds); 67 % at 8 dB with and without. Noise-only: 0 units. 16 pager tests pass (`pager_rx`, `pager_impair`,
`pager_flexreal`, `pager_sens`, ...). Guard: `sens40a3` (POCSAG -4.5 dB with 0 and 50 ppm, 0 dB with +3.1 kHz, FLEX 8 dB and 0 dB: 3.9 had 21 % there).

## 4. Zigbee / 802.15.4 (2.4 GHz O-QPSK): chip values bounded and weighted

The frame decoder sums the discriminator's phase steps of the 31 chips of a symbol against the 16 sequences. At low SNR a click (a step near pi) is worth up to 2 times
the nominal step and swamps the sum. The chip value is now `1.5708 * Im(z) / mean|z|` (z = the step product, mean over 256 samples; clamped to +-2.5): the sine of the step
(0 at a click, pi/2 at the nominal step) weighted by how strong the chip was. The trigger (hard chip registers, 7 errors a symbol) is not the limit (9 errors: no change).

| Zigbee (SNR in 2 MHz, 4 trials) | 3 / 1.5 / 0 / -1.5 dB before | sine only | after |
|---|---|---|---|
| AWGN | 91 / 38 / 2.8 / 0 % | 99 / 83 / 19 / 0.7 % | 99.7 / 90 / 31 / 1.5 % |
| +48 kHz (20 ppm) | 88 / 36 / 3 | 99 / 80.5 / 17 | 99.7 / 88 / 30 |
| 50 ppm clock | | 98 / 70 / 11 | 98 / 78 / 18 |
| -90 kHz | | | 99 / 84 / 25 |

About 1.5 dB at the 50 % point (1.9 to 0.4 dB). The receiver is still about 5 dB from an ideal coherent O-QPSK correlator (a spec receiver reaches 1 % PER at roughly
-4 dB chip SNR); a coherent MSK demodulator is the next step. `zigbee_rx`, `zigbee_engine`, `zigbee_proto`, `thread_rx` pass. Guard: `sens40a3`.

## 5. SAME (NOAA weather radio): the 520 baud tones on a 4.5 kHz channel of their own

SAME used the front end's single 9 kHz channel. The front end already has the optional second channel with its own discriminator (APRS and SSDV use it); SAME now sets
`chanPass2 = 4500` and feeds the tone demodulators from `narrowDisc()`. Messages out of 4 (two alerts and two ends, 8 seeds, SNR is the carrier to noise in 25 kHz):

| | 6 / 3 / 2 / 1 / 0 / -1 dB before | after |
|---|---|---|
| AWGN | 4 / 2.25 / 0.88 / 0 / 0 / 0 | 4 / 4 / 4 / 2.9 / 2.25 / 0.9 |
| +3 kHz carrier | 4 / 2.25 / 0.62 / 0 | 4 / 4 / 4 / 3.4 / 1.6 / 0 |
| -6 kHz carrier | | 4 / 4 / 4 / 3.25 / 2.1 / 0 |

50 % point 2.8 dB before, about -0.2 dB after: about 3 dB (3.5 kHz would be another 1 dB but the generator's attention tone has 3.5 kHz deviation: 4.5 kHz kept).
Noise-only (8 dB): no message, no tone (existing check). `same_rx` (weak 3 and 2 dB, +3 and -5 kHz: both alerts decoded exactly) and `same_engine` pass. One assertion of the
existing test changed: the "corrupted repeat" case read `bitsFixed >= 1`; the damaged repeat now arrives with a different tail (a few extra bytes after the last dash), so
the vote repairs it without counting fixed bits (the repaired text is the same and still checked).

## 6. CubeSat: the Bell 202 (AX.25 AFSK 1200) path on the same second channel

`cubesat_rx.cpp` fed all paths from the 12 kHz channel. The 1200 baud AFSK path now reads `narrowDisc()` (4.5 kHz channel); 9600 and 4800 baud paths and the extra paths keep the
12 kHz one. AX.25 AFSK 1200, every frame twice (SNR of the SmartSynth): 8 dB 50 % before, 6 dB 0; after 100 % down to 6 dB, 4 dB 0: about 3 dB, the same with +3 kHz and -11 kHz carrier
(-11 kHz: 4 dB 50 %). Coarse (two frames a cycle). `cubesat_rx` (three weak rows added), `cubesat_engine`, `cubesat_chain` pass.

## 7. LTR: 3.5 kHz filter and the carrier follow-up

LTR (FmFront) now uses a 3.5 kHz channel (was 4.5) with `setAfc(true, 4000, 9000)` (the work rate is 9.6 kHz, so the follow-up sees +-4.8 kHz). Words found out of 6, noise
sigma 1.5 / 1.75 / 2 (SNR figure of the test, 6 trials): carrier 0: 5.0 / 1.7 / 0.3 before, 5.3 / 2.7 / 0.8 after (about 0.5 dB); carrier +3.1 kHz: **0 at every noise level before**,
5.2 / 2.2 / 0.2 after. The voice tone of the test (1.5 kHz deviation) still passes the filter. A carrier 6 kHz off is half found (the work rate's limit), 8 kHz not at all.
`ltr_rx` has a +-3.1 kHz noise block now (before: the 162-case sweep only used +-900 Hz).

## 8. MRZ and iMS-100 sondes: sync with symbol errors, MRZ keeps its serial, both on the narrow path

Three separate things, found one after the other:

* **Sync.** MRZ matched its 44 symbol sync (9 x 1001 and 10101010) exactly, iMS-100 its 48 symbol word: at a 2 % symbol error rate that is 41 % and 38 % of the frames. Both accept 3 errors
  now (MRZ: at most one in the last 8 symbols, because the repeating preamble shifted by 2 or 4 symbols differs there by 4; iMS-100: no shifted copy of preamble and word comes within 6 of
  any of its four words, checked exhaustively). MRZ clean units 1.0 to 2.7 a 24 s trial.
* **MRZ serial (the part-2 "narrow path breaks sonde_extra_rf" mystery).** The serial comes from the config sub-frames C and D (one frame in four). `reset()` after a pause of more than 0.15 s
  cleared them, so on a signal with pauses the serial was never complete, `handleFix` found no key and dropped every frame. The wide path got through the test only because its bursts did not
  pause; the narrow path's presence flickered and lost them. The config is now kept for 5 minutes of stream time. (Other types with a serial from a config frame were checked: DFM and iMS-100 keep or
  rebuild theirs.)
* **Narrow path.** With the serial fixed, MRZ and iMS-100 run on the 4.5 kHz path of part 1.

| sonde (SNR in 10 kHz, 24 s trials, 4 trials) | 3.9 | after |
|---|---|---|
| MRZ frames a trial, clean | 1.0 | 7.3 |
| MRZ, 14 / 12 / 10 dB, carrier 0 | 0.75 / 0 / 0 frames | 6.1 / 4.0 / 0 frames |
| MRZ, 14 dB, +8 kHz | 0.5 frames | 6.6 frames |
| MRZ, 14 dB, +20 kHz | 0 | 5.3 frames |
| iMS-100 (of 19.7 frames), 10 / 8 / 6 / 4 dB | 93 / 37 / 0 / 0 % | 93 / 98 / 89 / 0 % |
| iMS-100, +8 kHz and 50 ppm | | same as carrier 0 |

MRZ: the 50 % point moves from about 14 to about 11.7 dB (it was measured as "13 to 14 dB" before only because 1 to 2 frames a trial were left), more than 2 dB, and the number of frames is 7 times
higher at every SNR. iMS-100: about 3 dB (it was called "cannot be measured in the harness" in part 2: the default 5 s trial is too short for its config frames; `CURVE_SECS=24` shows it).
A 20 kHz (50 ppm) carrier error needs about 30 s to pull in on the narrow path (the 16 s test of `sonde_extra_rf` runs 40 s for MRZ there); 8 kHz (20 ppm) and below lock as before.
Noise-only: 0 units. Tests: `sonde_extra` (sync 3 / 1-in-tail / 2-in-tail / 4 errors for MRZ, 0, 3, 4, 6 for iMS-100; the MRZ config kept over a reset, still there at 100 s, gone at 400 s),
`sonde_extra_rf` (MRZ at +-8 kHz added), `sens40a3` (MRZ 14 dB and iMS-100 6 dB, carrier 0 and 8 kHz, 24 s trials: about 4.5 minutes of the 5 in all), `sonde_all_rf`, `sonde_interf`, `sonde_sens40`,
`sonde_engine`, `sonde_basic` pass.

## Measured, not kept

* **Iridium.** The burst detector's 13 dB threshold looked like the Argos gate, but 10 dB gives the same curve (12 dB 80 % to 81 %) and 38 000 false triggers in 24 s of noise; the
  frames that fail have an EVM of 7 to 9 dB (symbol error rate about 1e-2), the limit of hard-decision BCH. The harness signal mixes bursts of 15 to 50 dB, so the curve
  is a mix, not one burst's. A soft-decision BCH decoder would be the gain. The carrier phase window of the fourth-power estimator (W 5 to 40): no change below 20, worse above.
* **Aero.** Search threshold 2.0 to 1.5 x floor: no change (9 dB 27 to 14 %). Three carriers (600, 1200, 10500) at different levels in the harness: 90 % at 13 dB in 12.5 kHz is the 10.5 kbit/s one.
* **ISM.** Burst threshold 13 to 10 dB: worse (clean units fall, 37 % at 14 dB). Wi-SUN burst threshold 5 to 3 x floor: +1 to 2 points. BLE with the discriminator clamped to 600 / 400 kHz: no change (BLE at 8 dB in 1 MHz is
  already better than the spec figure). VDL2 detection (0.56 to 0.45, 3 to 2 x floor): worse (14 dB 57 %).
* **M10 narrow path.** M10 needs no more: with 20 s trials it is at 10.5 dB (50 %) in 10 kHz, 10.7 dB Eb/N0 for 9600 baud, below the non-coherent figure, thanks to the tone chips of part B. (MRZ: see 8.)
* **Zigbee trigger** (7 to 9 chip errors): no change. **Wi-Fi HaLow 0.25**: clean frames lost.
* **POCSAG/FLEX single path**: unchanged from A2 (the second decoder is the fix).

## At theory, nothing to gain with the tools tried

Read against the Eb/N0 each needs (sens_curve, 4 trials, this pass or SENSITIVITY-4.0-B): Inmarsat-C (0 dB 85 % in 1.2 kHz for 1200 baud: 3 dB Eb/N0 with the rate 1/2 code), Orbcomm,
COSPAS-SARSAT, Argos (A2), ADS-B (1 aircraft: 50 % at 1.4 dB in 2 MHz), DSC (3 dB cliff in 25 kHz, an FM threshold), NAVTEX / SITOR (12.8 dB Eb/N0 with the repetition), VDL2 (D8PSK, 50 % at
10.5 dB in 25 kHz = 14 dB Es/N0 with RS), HFDL, LoRa raw (-12 dB for the mixed SFs of the harness), Sigfox, BLE, UAT, FLARM, ADS-L, DMR, AIS, APRS, P25, NXDN, dPMR, D-STAR, TETRA (A2 and 3.9),
M10 (above). The Mesh / LoRa entry of the harness counts something that does not depend on the SNR (22 % from -18 to 0 dB): not a measurement of the receiver.

## Left for later

Zigbee coherent MSK correlator (about 5 dB), Iridium soft BCH (about 3 dB), MRZ and iMS-100 soft Manchester and tone energies (they still decide hard symbols: another 3 to 5 dB), HaLow SIG / channel estimate (1 to 2 dB), GNSS acquisition,
CCSDS, ADS-B overlapping replies, LoRaWAN, Meshtastic (harness first), DSC, NAVTEX gap checks, ALE. The Globalstar gate is an energy detector: decoding the downlink would be a different receiver.
