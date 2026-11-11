# Sensitivity 4.0, pass B (feat40-sensB): narrowband and data modes

Branch feat40-sensB on integ-40. Every figure here is from simulated signals (each mode's own generator, white Gaussian noise and the
impairments named); no off-air recording was used. Pass A (feat40-sensA) has the broadcast, TV and satellite modes.

## Method

* `tools/sens/sens_sweep.cpp` (`sens_sweep --full --trials 8`, AWGN, 20 ppm carrier error, 50 ppm sample clock) for the registry modes.
  Threshold = the lowest SNR with at least 90 % of the trials passing, SNR in the mode's noise bandwidth (docs/SENSITIVITY.md).
* `tests/sens/test_sens40b.cpp` (ctest `sens40b`) holds the receivers this pass changed in two forms in one binary: `<mode>` is the 4.0
  receiver, `<mode>-off` the 3.9 path of the same receiver (a switch of the receiver, e.g. `PagerReceiver::setToneDetection(false)`).
  The two share their noise seeds, so a before/after difference is not a different noise draw.
  * default: a short guard (two trials at the guard SNR, noise alone gives nothing)
  * `ONAIR_SENS_POINTS="-3,-4,-5" ONAIR_SENS_TRIALS=30 ONAIR_SENS_MODES='^pocsag'`: the pass rate at those SNRs for AWGN, a 20 ppm carrier
    error (capped at a quarter of the channel), a 50 ppm sample clock, an off-centre carrier (20 % of the channel) and noise alone
  * `ONAIR_SENS_POINTS=-40`: every scenario is noise only: the false-decode check, `ONAIR_SENS_TRIALS` trials each
  * `ONAIR_SENS_CALIBRATE=1`: the threshold in 1 dB steps and `ONAIR_TEST_LONG=1` the longer sweep, as in sens39c
* `tests/hfdig/test_hfdig_sens40b.cpp` (ctest `hfdig_sens40b`): the weak-signal HF modes (FT8 FT4 FT2 WSPR JT65 JT9 Q65 FST4 JT4), 3
  stations a slot at one SNR, the share decoded, plain and with a dial error of +40 Hz and a clock error of +100 ppm, and slots of
  noise alone. `ONAIR_TEST_LONG=1` runs every mode, `ONAIR_SENS40B_MODES=FT8` picks, `ONAIR_SENS40B_SLOTS` sets the slots a point.

## Baseline: 3.9 receivers on integ-40 (sens_sweep, full tier, 8 trials, Mac, 2026-11-12)

Threshold in dB (90 % of the trials), SNR in the noise bandwidth of the last column. "false" = share of noise-only trials that gave a unit.

| mode | awgn | cfo 20 ppm | clock 50 ppm | false | noise bw (Hz) |
|---|---|---|---|---|---|
| acars1 (1 aircraft) | 4 | 5 | 5 | 0 | 25000 |
| adsb1 (1 aircraft) | 2 | 2 | 2 | 0 | 2e6 |
| adsl | 4 | 5 | 3 | 0.042 | 200000 |
| aero | 11 | 11 | 11 | 0 | 12500 |
| ais1 (1 ship) | 3 | 3 | 6 | 0 | 25000 |
| argos | 13 | 13 | 13 | 0 | 5000 |
| ble | 8 | 8 | 8 | 0 | 1e6 |
| cw | 1 | 5 | 2 | 0 | 500 |
| dmr | 5 | 5 | 5 | 0 | 12500 |
| dpmr | 7 | 7 | 7 | 0 | 6250 |
| dsc | 4 | 6 | 4 | 0 | 25000 |
| dstar | 7 | 7 | 8 | 0 | 6250 |
| flarm | 4 | 9 | 4 | 0 | 200000 |
| halow | 7 | 7 | 7 | 0 | 1e6 |
| hfdl 300 / 600 / 1200 / 1800 | 0 / -1 / 1 / 4 | -3 / -1 / 1 / 8 | -3 / -1 / 1 / 4 | 0 | 1800 |
| ism | 13 | 10 | 9 | 0 | 100000 |
| loraraw | -8 | -8 | -8 | 0 | 125000 |
| m17 | 5 | 6 | 5 | 0 | 9000 |
| navtex | 8 | 8 | 8 | 0 | 300 |
| ndb | 13 | 17 | 14 | 0 | 300 |
| nxdn | 8 | 8 | 8 | 0.083 | 6250 |
| orbcomm | 7 | 7 | 8 | 0 | 10000 |
| p25 | 9 | 8 | 6 | 0 | 12500 |
| packet (APRS) | 5 | 5 | 5 | 0 | 25000 |
| pactor2 | 3 | 2 | 2 | 0 | 450 |
| pager (POCSAG and FLEX in turn) | 1 | 5 | 1 | 0 | 25000 |
| sarsat | 1 | 1 | 1 | 0 | 3000 |
| sigfox | 1 | 1 | 5 | 0 | 600 |
| sitor | 8 | 8 | 8 | 0 | 300 |
| sonde | 13 | 17 | 12 | 0 | 10000 |
| tetra | 7 | 7 | 8 | 0 | 25000 |
| tetrapol | 9 | 10 | 9 | 0 | 12500 |
| uat | 9 | 9 | 9 | 0.042 | 1.2e6 |
| vdes | 9 | 9 | 9 | 0 | 25000 |
| vdl2 | 12 | 12 | 12 | 0 | 25000 |
| vorils | 3 | 3 | 3 | 0 | 25000 |
| wisun | 10 | 11 | 10 | 0 | 200000 |
| wmbus | 12 | 13 | 11 | 0 | 100000 |
| zigbee | 2 | 2 | 3 | 0 | 2e6 |
| zwave | 13 | 9 | 13 | 0 | 100000 |

The sens_sweep SNR of a multi-transmitter test signal is the mean over bursts of different level (SENSITIVITY-3.6.md), so ADS-B, AIS, ACARS
numbers for one aircraft or ship are the ones to compare with theory.

HF weak-signal modes (test_hfdig_sens40b, 6 slots x 3 stations a point, SNR in 2500 Hz; 90 % / 50 % of the stations decoded; "faults" =
dial +40 Hz and +100 ppm):

| mode | plain 90 % / 50 % | faults 90 % / 50 % |
|---|---|---|
| FT8 | -20 / -21 | -19 / -21 |
| FT4 | -15 / -16 | -14 / -16 |
| FT2 | -12 / -13 | -11 / -13 |
| WSPR | -28 / -30 | -29 / -29 |
| JT65 | -22 / -24 | -23 / -24 |
| JT9 | -25 / -26 | -23 / -26 |
| Q65 | -26 / -27 | -26 / -27 |
| FST4 | -24 / -25 | -24 / -25 |
| JT4 | -19 / -24 | -22 / -24 |

These sit at the WSJT-X figures already (FT8 -21 dB, FT4 -17.5, JT65A -25, JT9 -26 to -27, WSPR -29 to -31 in 2500 Hz for 50 %). Noise-only slots
gave no decode for any mode. A few decodes of a wrong message text (a signal report decoded as a grid, `W9XYZ K1ABC R JJ76` for
`W9XYZ K1ABC R-05`) appear next to decodes of strong signals in FT8 (faults), FT4, FT2 and JT4 (faults); they are the CRC passing on the
residue of a subtracted signal and are listed under Not done.

## POCSAG (core/src/pager/pager_rx.cpp)

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point (SNR in 25 kHz) | -1 dB | **-3 dB** | 2 dB |
| AWGN, 50 % point | about -2.2 dB | about -4.3 dB | about 2.1 dB |
| 50 ppm sample clock, 90 % / 50 % | -1 / about -2.4 dB | -3 / about -4.3 dB | 2 / 1.9 dB |
| 20 ppm carrier error (6.25 kHz at 466 MHz, capped) | no change (the receiver falls back to the discriminator off centre) | | 0 |
| off centre 5 kHz | no change | | 0 |
| noise only | 0 of 300 trials (60 each, at -40 dB: 40 minutes of noise) | 0 of 300 | |
| CPU per second of signal | 0.0194 | 0.0214 | +10 % |

30 trials a point, same seeds for both receivers (test_sens40b). Per point (4.0 / 3.9, passes of 30): AWGN -2: 30 / 12, -3: 28 / 0,
-4: 18 / 0, -5: 3 / 0.

What changed:

1. The symbol value is the energy at the upper tone against the energy at the lower tone, correlated over the symbol, instead of the mean
   frequency of the unwrapped discriminator phase. A weak signal gives clicks (a 2 pi step of the phase) that the mean frequency counts in
   full; the energies do not. The two tone frequencies come from the 32 symbols of the sync word, which are known: a grid search over the
   centre and the deviation finds the pair that puts the most energy on the expected tones, and the energy of each tone on its expected
   symbols gives a gain per tone (the channel filter's slope makes them differ with a carrier offset). Alone this gave about 1.5 dB.
2. The sync search accepted a sync word with at most 2 wrong bits of 32 in the discriminator's hard decisions, which at the threshold
   is what lost most of the pages. It now takes up to 5 wrong bits when the tone energies agree: the correlation of the 32 symbols with the
   pattern must be at least 0.52 + 0.025 per wrong bit. The tone decisions of something that only looks like the sync word agree with the
   discriminator's, so the correlation they reach is about 1 - 2 wrong/32; the real word is matched better. Together about 2 dB.
3. The carrier centre still follows the discriminator (the tone value does not carry the offset). When the centre found at the sync word is
   more than 3 kHz off, the receiver keeps the discriminator for that transmission (the channel filter cuts the upper tone there).

Tried and not kept: 8 wrong bits in the sync word (the threshold of POCSAG moved by a few tenths of a dB but FLEX in the same channel lost
pages to false POCSAG locks); a tone-energy timing error for the clock loop (no change); the same tone symbols for FLEX (the sync marker has
the same kind of known symbols; the first try lost pages to the looser sync search and was not pursued, FLEX is unchanged and measures the
same as the 3.9 path with the same seeds).
The pager test signal is the receiver's own (pager_gen): its pulse shape is not a real transmitter's.

Tests: sens40b (guard, noise), sens39d pocsag row (-1 -> -3 dB), pager_* (21 tests; pager_proto_rs is only built with the Rust crate): all pass.

## Z-Wave (core/src/zwave/zwave_rx.cpp)

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point (SNR in 100 kHz) | 9.5 dB | **6 dB** | 3.5 dB |
| AWGN, 50 % point | about 9.0 dB | about 5.3 dB | about 3.7 dB |
| 20 ppm carrier error (17 kHz) | 90 % at 9 dB (30 trials: 9: 30/30, 8.5: 2/30) | 90 % at 6 dB | 3 dB |
| 50 ppm sample clock, off-centre carrier (20 kHz) | same as AWGN | same as AWGN | |
| noise only | 0 of 300 trials (60 each at -40 dB; 4 s a trial) | 0 of 300 | |
| CPU per second of signal, 10 dB over threshold | 0.050 | 0.051 | |

Passes of 30 trials, same seeds (test_sens40b zwave / zwave-off): 4.0: 7 dB 30, 6 dB 30, 5 dB 1, 4 dB 0; 3.9: 10 dB 30, 9.5 dB 30, 9 dB 19, 8.5 dB 0.
(sens_sweep reads 13 / 9 / 13 dB for AWGN / 20 ppm / 50 ppm in the table above: its trials start at random points of the signal and need
half of the frames; the shape is the same, this harness' clean-signal reference and its criterion differ.)

1. Each chip is judged by the energy at the upper tone (carrier offset + deviation) against the lower one over the chip, from the burst's
   complex samples, instead of by the mean of the FM discriminator over the chip (noncoherent matched detection of the FSK; the
   discriminator's clicks fill a weak burst with chip errors). The chip clock search uses the same value. The deviation is the one
   of the rate (20 kHz for R1 and R2, 29 kHz for R3) and the carrier offset the burst's mean frequency from the preamble, as before. When the tones
   give no frame the discriminator is tried as before.
2. The burst detector started a burst at 6 times the noise floor (smoothed power in the +-110 kHz channel) and ended it at 2.5; at the
   threshold the signal is only about 3.5 times the floor, so most bursts were never opened. With the tone chips: start at 3, end at 1.8.
   A noise-only trial opens a few bursts now, each decoded and dropped by its checksum (no false frame in 300 trials).
The test signal is the generator's own (zwave_gen), R1 to R3 in turn: simulated only.

Tests: sens40b (zwave guard at 14 dB, noise), sens39c guard (zwave), zwave_rx / proto / engine / homenet: pass.

## Radiosondes (core/src/sonde/sonde_dsp.cpp, sonde_chan.h, sonde_rx.cpp)

Test signal: an RS41 on the centre, a DFM-17 and an M10 elsewhere (sens_modes.h `sonde`); SNR in 10 kHz; a trial passes when at least half of the
frames of a clean run decode.

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point | 11 dB | **9 dB** | 2 dB |
| AWGN, 50 % point | about 10.4 dB | about 8.4 dB | about 2 dB |
| 20 ppm carrier error, 50 ppm clock, off-centre (20 % of 10 kHz) | all 10 / 10 down to 12 dB, 0 / 10 at 10 dB | all 10 / 10 down to 10 dB, 0 / 10 at 8 dB | 2 dB |
| noise only | 0 of 40 trials (at -40 dB) | 0 of 40 | |
| CPU per second of signal at threshold + 10 dB | 0.189 | 0.200 | +6 % |

20 trials a point, same seeds (test_sens40b sonde / sonde-off): 4.0: 11 dB 20, 10.5 dB 20, 10 dB 20, 9.5 dB 18, 9 dB 20, 8.5 dB 7; 3.9: 11 dB 19, 10.5 dB 9, 10 dB 0.
sens_sweep reads 13 / 17 / 12 dB for the 3.9 receiver in the table above (random start points, 8 trials, 1 dB steps).

The symbol of RS41, RS92, DFM (+-2.4 kHz) and M10, M20 (+-4.8 kHz, the deviations of the generator) is decided by the energy at the upper
tone against the energy at the lower tone over the symbol, from the complex samples of the channel (centre = the discriminator's mean,
as the slicer's threshold was), instead of by the sign of the smoothed discriminator. The symbol clock still comes from the discriminator's
zero crossings; the decision is taken when the clock says the symbol has ended (the smoothing delays the clock by 0.35 symbol). The other types
(iMet, MRZ, Meisei, LMS6, the AFSK iMet) keep the discriminator. The M10 (9.6 kbaud in a 10 kHz noise bandwidth) is the type that sets the
threshold of this mix: its Eb/N0 equals the SNR, which is about the textbook figure for noncoherent FSK at the error rate its frames survive.
The RS41 and DFM would decode at lower SNR alone; not measured separately. Real sondes' deviations differ from the generator's by up to a
few tenths of a kHz; with +-25 % the loss of tone detection is about 1 dB (not measured here).

Tests: sens40b (sonde, noise), sonde_* (26 tests incl. rs41_rf, rs92_rf, rf_dfm, rf_m10, rf_m20, all_rf): pass.

## Not done

* The wrong-message decodes of FT8 / FT4 / FT2 / JT4 next to strong signals.
* Tried and not kept: a sum of two or three discriminator products before the angle (DMR, D-STAR, NXDN, dPMR, P25), thresholds
  within one 1 dB step of the plain discriminator (24 trials a point), so not kept.
