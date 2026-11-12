# Sensitivity 4.0, pass X1 (feat40-sensX1): using what a single-frame decoder throws away

Branch feat40-sensX1 on integ-40. Goal: gains beyond the single-frame limit, from repeats and known content (the way FT8's a-priori passes
and GNSS long integration work). Every figure is from simulated signals: the mode's own generator, complex white Gaussian noise, the
impairments named. No off-air recording was used. All measurements ran on GitHub (quick.yml, Linux runner), not on the laptop.

Bench: `tests/sens/test_sens40x1.cpp` (ctest `sens40x1`). Each section runs the same seeded trials through the receiver with the technique
off and on (switches: `SarsatReceiver::setWeakBurstPath / setRepeatCombining / setRepairCostLimit`, `NdbReceiver::setIdentFolding`,
`TimeReceiver::setPredictedMinutes`), with a carrier error, a clock error and an off-centre carrier, and noise alone.
`ONAIR_X1_CURVE=sarsat,ndb,time ONAIR_X1_TRIALS=12 ONAIR_X1_VERBOSE=1 test_sens40x1` prints the curves; without it the binary is the guard.

## Summary

| mode | technique | before (3.9) | after (4.0) | false decodes (4.0) | conditions / what the gain needs |
|---|---|---|---|---|---|
SUMMARY_ROWS

## COSPAS-SARSAT 406 MHz (core/src/sarsat/sarsat_rx.cpp)

Test: one first-generation test beacon (all five test beacons in turn over the trials, long and short messages) sending 4 bursts 50 s
+-2.5 s apart (random), random carrier phase per burst, SNR = burst power over the noise in 3 kHz (the sens_sweep definition), 128 kS/s,
12 trials a point. "Identified within n bursts" = the beacon's identification was reported (and was the right one) by the end of burst n.

What limited the 3.9 receiver, found while building the repeat combiner (each fix has its switch for the A/B rows below):

1. **Burst end.** A burst ended 6 blocks (192 ms) after the last block with a carrier line. A weak burst shows its line only in the 160 ms
   bare-carrier preamble (the phase-modulated data lowers it by about 7 dB), so the burst ended about 200 ms into the 360 ms message and the
   demodulator never saw the end of it: below about 0 dB in 3 kHz no burst could decode, whatever the bit errors. Now a burst runs at least
   0.62 s from its first line.
2. **Weak burst detection.** The sum of the last 4 block spectra (the preamble spans 4 to 5 blocks of 32 ms) finds a line about 4 dB weaker
   than one block at the same false-alarm rate (threshold 8.5 x the median of the sum, about 5e-10 of noise bins).
3. **Coherent bits.** The +-1.1 rad Manchester modulation leaves cos(1.1) of the carrier in every bit; its 21-bit average (the preamble included)
   is a phase reference, and each bit is the projection of the difference of its half-bit vectors on it, scaled to a log-likelihood ratio by
   the burst's own spread. The 3.9 slicer multiplied the two noisy half-bit vectors (differential detection). The sync word is now checked on
   the coherent bits (the differential search only proposes timings).
4. **Soft-decision BCH (Chase).** 128 test patterns on the 7 least reliable bits of the BCH-1 code word; the codeword closest to the soft bits wins.
5. **Repeat combining.** The soft bits of a burst that does not decode are kept (up to 6 timings: a weak burst's best-looking timing is often half
   a bit or one rate step off) for 4 repeat intervals. A later failed burst on the same carrier (+-150 Hz) at 50 s +-2.5 s (or 2, 3, 4 times that)
   is aligned with each kept burst by the correlation of their soft bits (0.3 or more: the same message correlates by 0.4 to 0.8 at these levels,
   another timing or message by 0 +- 0.08) and the sum is decoded with the same checks.
6. **Expected carrier.** Near the carrier of a beacon heard in the last 4 minutes, the 4-block sum takes a line at 5.5 x its median (+-3 bins).
7. **Confirmation by repeat.** A message repaired by more than one bit (or a long one whose second check failed) is reported only when a later
   burst of the same beacon decodes to the same identification. A message with at most one repaired bit and its checks intact is reported at once.

Run 2 (GitHub run 43147175804, before items 6 and 7), share identified within 1 / 2 / 4 bursts:

| SNR in 3 kHz | 3.9 | items 1 + 2 | + 3 + 4 | + 5 (combining) |
|---|---|---|---|---|
| 0 dB, AWGN | 42 / 75 / 92 % | 75 / 92 / 100 % | 100 / 100 / 100 % | 100 / 100 / 100 % |
| -2 dB | 0 / 0 / 8 % | 0 / 0 / 17 % | 100 / 100 / 100 % | 100 / 100 / 100 % |
| -3 dB | 0 | 0 | 92 / 100 / 100 % | 92 / 100 / 100 % |
| -4 dB | 0 | 0 | 58 / 75 / 100 % | 58 / 75 / 100 % |
| -5 dB | 0 | 0 | 25 / 58 / 58 % | 25 / 58 / 58 % |
| -6 dB | 0 | 0 | 0 / 0 / 0 % | 0 / 0 / 17 % |
| -3 dB, 20 ppm carrier (8 kHz) | 0 | 0 | 75 / 92 / 100 % | same |
| -3 dB, 50 ppm clock | 0 | 0 | 92 / 100 / 100 % | same |
| -3 dB, off-centre +30 kHz | 0 | 0 | 67 / 83 / 100 % | same |

Single burst, 90 % point: about +1 dB (3.9) to about -3 dB (4.0), 4 dB. The 50 % point: 0 dB to -4 to -4.5 dB. Repeat combining added little in this
run (up to 17 to 33 % at -6 and -7 dB within 4 bursts where nothing decoded alone): at those levels most bursts are not detected at all, so there
is nothing to combine. Item 6 is the attempt at that; see the final table.

False decodes. The 3.9 receiver already reported wrong identifications at the edge of its range: 3 to 7 wrong in 48 bursts at 0 and -2 dB (a weak burst
with 4 or more errors in BCH-1 lands on another codeword within 3 bits; the old guards, a perfect sync word for 3 repairs and an intact second
check for 2 repairs in a long message, did not stop them). With soft decoding the wrong identifications grew (1 to 6 per 48 bursts at every level
from -2 to -7 dB): neither the soft cost of the repaired bits nor the Chase margin separates them from right ones (measured over 600 decodes:
cost per repaired bit 0.17 to 1.1 for wrong, 0.25 to 0.93 for right; margin 0 to 99 for both). Item 7 is the fix: two independent wrong decodes
giving the same identification do not happen.
Noise alone: no burst is found and no decode (the guard runs 4 x 210 s of noise).

Final receiver (all 7 items; GitHub run 43157120181), share identified within 1 / 2 / 4 bursts, 12 trials a point:

| SNR in 3 kHz | 3.9 | 4.0 without combining (items 1 - 4 and 7) | 4.0 (all items) | wrong identifications 3.9 / 4.0 (48 bursts) |
|---|---|---|---|---|
| -1 dB, AWGN | 17 / 17 / 33 % | 100 / 100 / 100 % | 100 / 100 / 100 % | 0 / 0 |
| -2 dB | 0 / 0 / 8 % | 100 / 100 / 100 % | 100 / 100 / 100 % | 1 / 0 |
| -3 dB | 0 | 92 / 100 / 100 % | 92 / 100 / 100 % | 0 / 0 |
| -4 dB | 0 | 42 / 67 / 83 % | 42 / 67 / 83 % | 0 / 0 |
| -5 dB | 0 | 8 / 17 / 17 % | 8 / 25 / 25 % | 0 / 0 |
| -6 dB | 0 | 0 | 0 / 0 / 8 % | 0 / 0 |
| -1 / -3 / -4 dB, 20 ppm carrier (8 kHz) | 33 / 0 / 0 % (within 4) | 100 / 100 / 50 % | 100 / 100 / 50 % | 1 / 0 at -1 dB |
| -1 / -3 / -4 dB, 50 ppm clock | 33 / 0 / 0 % | 100 / 100 / 92 % | same | 1 / 0 |
| -1 / -3 / -4 dB, off-centre +30 kHz | 42 / 0 / 0 % | 100 / 100 / 83 % | same | 0 / 0 |

(The middle column switches combining off, and with it item 6, which uses the combiner's state. Wrong identifications in that column: 0.)

* **Single burst: about 4 dB.** 90 % within 2 bursts: about +1.5 dB (3.9) to -3 dB (4.0); 50 % within 1 burst: about 0 dB to -3.8 dB.
* **Repeat combining: small, about 0.5 dB at the 4-burst point** (-5 dB: 17 % to 25 %, -6 dB: 0 to 8 %). The theory (3 dB for 2 bursts, 6 dB for 4)
  assumes every burst is found and timed; below -5 dB most bursts are not detected at all (the 160 ms preamble is the only thing the detector can see),
  so the combiner rarely has two bursts to add. The expected-carrier threshold (item 6) did not change that measurably.
* **False identifications: 0** in every 4.0 column of this run (and 0 in 4 x 210 s of noise alone), against 1 in 48 bursts for 3.9 at -1 / -2 dB
  and 3 to 7 at 0 dB in run 2. The price of item 7: a beacon whose bursts all need multi-bit repairs is reported one burst later.
* CPU: the soft decoder tries 128 patterns for each timing candidate that passes the sync check; a burst costs up to a few hundred ms of search
  (once every 50 s per beacon). Not profiled.

Guard (ctest `sens40x1`, part `sarsat`): -3 dB within 2 bursts with AWGN, 20 ppm carrier, 50 ppm clock and +30 kHz off centre (11 of 12 in the run above,
the 3.9 receiver 0), -4 dB within 4 bursts, noise alone gives nothing, two beacons alternating on one channel give no identification that was not sent.
Needs: the bursts of one beacon actually repeat on the same carrier (they do: C/S T.001 repeats every 50 s +-2.5 s); combining a burst of another
beacon on the same carrier is refused by the soft-bit correlation. Simulated only: a real beacon's phase noise, the modulation index (1.1 rad assumed)
and the satellite Doppler drift over a pass (a few Hz per second) are not modelled; the drift moves the carrier between bursts by more than the
+-150 Hz match window over 4 bursts in a fast pass, which limits combining to consecutive bursts there.

## NDB ident folding (core/src/ndb/ndb_rx.cpp)

NDB_BODY

## Time stations: the expected minute (core/src/time/time_rx.cpp)

TIME_BODY

## Not done, and why

* **A-priori decoding of ADS-B**: already there. `AdsbTracker::tryCorrect` repairs up to 3 (Advanced: 5) of the least certain bits only for a
  frame whose address is a known aircraft, with an altitude plausibility check for the deeper repair: the ADS-B form of an AP pass.
* **AIS, APRS, POCSAG repeat combining**: AIS position reports change between repeats (position, time stamp, communication state) and the
  identical static reports come 6 minutes apart; APRS digipeated copies differ in the path field and bit stuffing moves every later bit;
  POCSAG repeats depend on the network. Soft combining needs bit-aligned identical frames: not done.
* **Radiosonde RS41 known fields** (serial, frame counter, block headers): about 50 of 312 bytes are known, worth an estimated 0.3 to
  0.5 dB on the Reed-Solomon cliff (SENSITIVITY-4.0-A.md found the failures at the FM threshold with more than 20 bad bytes a codeword).
  Not done in this pass.
* **GNSS acquisition assistance**: needs the navigation data bits for coherent integration beyond 20 ms; not started.
