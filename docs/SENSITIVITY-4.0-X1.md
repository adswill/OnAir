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
| COSPAS-SARSAT 406 MHz (first generation) | full burst window, 4-block detection, coherent bits, soft BCH (single burst) | 90 % within 2 bursts at about +1.5 dB in 3 kHz | about -3 dB | 0 wrong identifications (3.9: 1 to 7 in 48 bursts at 0 to -2 dB) | AWGN, 20 ppm carrier, 50 ppm clock, +30 kHz off centre, 4 bursts 50 s apart |
| COSPAS-SARSAT | repeat combining of failed bursts + expected carrier + confirmation by repeat | (single-burst 4.0 path) | about 0.5 dB more at the 4-burst point (-5 dB: 17 to 25 %, -6 dB: 0 to 8 %) | 0 | needs the beacon's bursts to repeat on the same carrier; below -5 dB most bursts are not detected, which limits it |
| NDB (400 Hz tone ident) | ident folding: average of the repeated idents | 50 % at about 6.5 dB in 300 Hz (4.0 weak path), 90 % at about 7 dB | 50 % at about 4.5 dB, 90 % at about 6.3 dB (AWGN, 20 ppm, 50 ppm) | 0 added (noise alone: 0 in 12 x 120 s) | 60 s of a beacon repeating its ident; no gain with the carrier 60 Hz off centre |
| DCF77, MSF, WWVB | expected-minute check (a-priori) | minutes kept after the first: 81 / 64 / 100 % at -5 dB in 1 kHz | the same | 0 wrong times | no gain measured: the check never fired (below) |

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

The receiver keeps the keying depth of each path (400 Hz, 1020 Hz, keyed carrier) at 50 Hz for 3 minutes. Every 5 s: the ident period from the
autocorrelation (2 to 30 s; the shortest lag within 80 % of the highest peak, so that 2 or 3 periods are not taken; robust z-score 3 or more), the
last periods averaged with the fractional period (linear interpolation), the average sliced at the middle of its 10th and 90th percentiles when its
contrast is 6 noise deviations of the average or more, and read as Morse three times over from the longest gap; the ident is the word of the last
two copies. Two evaluations in a row must read the same word, and the carrier must stand out of the noise (the existing carrier check). The keying
adds up over the periods while the noise averages down; the weak path's own rule (a word counts only with 5 dB of on / off contrast) is what stops
a single cycle at about 6.5 dB, and the average does not need it.

Test: generator ident 'BND' at 7 WPM on a 400 Hz tone (80 % AM, a 5.8 s cycle), 60 s from a random point of the cycle, 48 kS/s, SNR = the signal's
mean power over the noise in 300 Hz, 12 trials a point (GitHub run 43166550047). Share of trials with the ident at the end:

| SNR in 300 Hz | 9 | 7 | 6 | 5 | 4 | 3 | 1 dB |
|---|---|---|---|---|---|---|---|
| 3.9 receiver | 0 | 0 | 0 | 0 | 0 | 0 | 0 % |
| 4.0 weak path (SENSITIVITY-4.0-B2) | 100 | 92 | 17 | 0 | 0 | 0 | 0 % |
| + ident folding, AWGN | 100 | 100 | 83 | 42 | 50 | 17 | 0 % |
| + ident folding, 20 ppm carrier (8 Hz) | 100 | 100 | 83 | 42 | 42 | 25 | 0 % |
| + ident folding, 50 ppm clock | 100 | 100 | 100 | 50 | 58 | 33 | 0 % |
| + ident folding, carrier 60 Hz off centre | 100 | 92 | 8 | 0 | 0 | 0 | 0 % |

(The 3.9 receiver reads 0 here because this test's SNR is in 300 Hz of the 48 kS/s input and its trial is 60 s; in SENSITIVITY-4.0-B2.md it needed 14 dB.)
About 2 dB at the 50 % point and about 0.7 dB at the 90 % point, with 60 s (10 cycles); a longer listen averages more cycles. Off centre by 60 Hz the
folding does not help: the weak path's frequency loop leaves the keying depth too noisy there to average into a readable ident (not investigated further).
False idents: noise alone with folding, 12 x 120 s and 4 x 120 s in the guard: none. Words that are not part of the ident ("E", "T") appear at 6 to 9 dB
from the weak path's own reader, as before; folding adds none (8, 4, 1 against 13, 4, 1 in the AWGN rows). Guard: `sens40x1` part `ndb` (folding not
worse than the weak path at 7 and 5 dB with AWGN, 20 ppm and off centre; noise alone gives nothing). In that run: 5 dB AWGN 0 of 6 without, 4 of 6 with.
Simulated only: a real NDB's keying, its idents between long dashes and interference from other beacons are not modelled.

## Time stations: the expected minute (core/src/time/time_rx.cpp)

After one decoded minute the time code of the next minutes is known. A minute whose parity fails now counts when its 60 symbols match the expected
minute's code (encodeMinute with the last decoded options) in 54 or more seconds and at least 3 better than the minutes around it (+-1 and +-2 minutes,
+-1 hour, +-1 day), so that a wrong second count or a wrong anchor cannot pass. Not for RWM (no time code) and BPC (20 s frames).
`TimeTelemetry::minutesPredicted` counts them.

Measured (GitHub run 43166550047, 4 trials of 10 minutes a point, SNR = carrier over the noise in 1 kHz): the check **never fired**. DCF77 keeps 100 %
of the minutes after its first decode down to -2 dB, 81 % at -5 dB (94 % with 1 Hz carrier and 50 ppm clock error), and finds no first minute at -8 dB;
MSF 100 / 64 / 0 %, WWVB 100 / 100 / 0 %. The receiver folds the seconds over the minute and decides each second against the folded levels, so
its minutes fail by lost or misplaced seconds (the frame is not a candidate at all), not by one wrong symbol that a prediction could cover. No gain and
no wrong time (0 in every row and in the guard). Kept because it costs nothing and covers a minute with a single symbol error; it is not counted as a gain.

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
* **CW beacons**: the CW receiver already works in a 100 Hz bandwidth (SENSITIVITY-4.0-A.md); folding repeated beacon messages as for the NDB is the
  obvious next step for amateur beacons with a fixed message, not done in this pass.
* **SARSAT second generation (SGB)** and the wide-capture bank: the bank's child receivers get the same switches; SGB bursts are not combined.

## Integrator notes

* The bench is one ctest, `sens40x1` (about 25 minutes on a GitHub runner with the curves, about 8 without: `ONAIR_X1_CURVE` unset).
* New public API: `SarsatReceiver::setRepeatCombining / setWeakBurstPath / setRepairCostLimit`, `sarsat::decodeCombined`, `sarsat::SoftBurst`,
  `SarsatEntry::combined / confirmedByRepeat`, `Beacon::repairCost / repairMargin`, `NdbReceiver::setIdentFolding` and the `fold*` telemetry fields,
  `TimeReceiver::setPredictedMinutes`, `TimeTelemetry::minutesPredicted`. The SARSAT details view shows when a beacon was decoded from combined bursts.
* No change to modes.cpp, mode_table.cpp, kNumFamilies or kPollSeqSlots.
