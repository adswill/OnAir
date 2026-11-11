# Sensitivity 4.0, pass B2 (feat40-sensB2): modes not improved yet

Branch feat40-sensB2 on integ-40. Every figure is from simulated signals (the mode's own generator, white Gaussian noise and the impairments
named); no off-air recording. Harness: `tests/sens/test_sens40b.cpp` (ctest `sens40b`) as in SENSITIVITY-4.0-B.md: `<mode>` is the 4.0
receiver, `<mode>-off` the 3.9 path of the same receiver, same noise seeds.
`ONAIR_SENS_POINTS="-3,-4" ONAIR_SENS_TRIALS=20 ONAIR_SENS_MODES='^argos'` prints the pass rate per SNR (AWGN, 20 ppm carrier error, 50 ppm clock,
off-centre carrier) and noise only; `ONAIR_SENS_POINTS=-40` is the false-decode run.

## Argos uplink (PTT-A2) (core/src/argos/argos_rx.cpp)

SNR in 5 kHz as in sens_sweep. A trial passes when at least half of the units of a clean run decode (2 bursts a trial).

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point | 13 dB | **about -5.5 dB** | about 18 dB |
| AWGN, 50 % point | about 11.8 dB | about -6.8 dB | about 18 dB |
| 20 ppm carrier error, 50 ppm clock, off-centre (20 % of 5 kHz) | 13 dB | -5 to -6 dB | about 18 dB |
| noise only | 0 of 20 | 0 of 150 trials (6 s of noise each) | |

Passes of 20 trials (4.0, awgn / cfo / sro / off-centre): -4 dB 20 / 20 / 20 / 20, -5 dB 20 / 19 / 20 / 20, -6 dB 20 / 18 / 19 / 19, -7 dB 16 / 15 / 13 / 12;
3.9: 12 dB 12 of 20, 13 dB 20 of 20, 11 dB 0 of 20. The 3.9 receiver decodes nothing at -3 dB or below (0 of 20 at every point).

Cause. The 3.9 receiver sets the burst's extent from the mean power of 1 ms windows of the 32 kHz stream (noise bandwidth 32 kHz, the signal occupies
about 800 Hz) and gives up when the 70th percentile is not 4 times the 5th. With 5 kHz as the reference that is 13 dB. The demodulator itself
(angle between the two half-bit vector sums, +-2.2 rad for a bit) is limited much lower: its half-bit noise bandwidth is 800 Hz.

Changes:
1. The window power is the coherent sum over 1 ms (the burst is mixed to 0 Hz by then): the noise power per window stays that of one sample, the
   carrier grows with the window length (15 dB more contrast). Four windows are averaged for the gate; the noise floor is taken from the 5th
   percentile of that. Gain alone: 13 dB to about 3 dB.
2. When the gate still cannot place the burst, the burst finder's own start (the first block of the line) is used, the message runs to the end of the
   buffer and its length is the one of the 8 that exist (80 + 32 k bits) where the running sum of (bit closeness to +-2.2 rad - 0.75) peaks; bits of noise score
   about 0.51 and pull the sum down. The message needs a mean closeness of at least 0.72 (noise alone: 0.51, the maximum over the timing and
   rate search about 0.6).
3. The line detector accepts a line of 12 times the median bin (was 30) when the same bin (+-2) holds one in three blocks in a row. This did not move
   the threshold by itself (the demodulator is the limit now); it is kept as it costs no false alarm in 150 noise runs.

Argos A2 has no checksum, so the false-decode protection is the line detector (a persistent carrier line) and the closeness test above; an unmodulated
carrier scores -0.59 and is rejected. Simulated only: the generator's pulse shape (0.3 ms edges) and the real transmitters' carrier stability are assumed.
Test: sens40b guard (`argos` at 0 dB, noise only gives nothing; the 3.9 receiver fails at 0 dB); argos_rx, argos_sigfox_mode pass.

## NDB, 400 / 1020 Hz tone idents (core/src/ndb/ndb_rx.cpp)

SNR in 300 Hz. A trial (40 s of a beacon sending the ident BND) passes when the receiver reports the generator's ident (any rotation of it: the cycle has no
marked start) at least half as often as a clean run. The sens_sweep metric counts any confirmed word, so the harness entry here counts only the right ident.

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point | 14 dB (13 dB: 10 / 12, 12 dB: 0 / 12) | **9 dB** (9: 12 / 12, 8: 5 / 12) | 5 dB |
| 20 ppm carrier error (8 Hz at 400 kHz) | 13 dB: 11 / 12 | 9 dB: 11 / 12 | 4 to 5 dB |
| 50 ppm clock | 13 dB: 11 / 12 | 9 dB: 12 / 12 | 4 to 5 dB |
| off-centre carrier (60 Hz) | 13 dB: 11 / 12 | 9 dB: 11 / 12 | 4 to 5 dB |
| noise only | 0 of 12 | 0 of 60 trials (40 s of noise each) | |

1. The 8 kHz stream is limited to +-1.5 kHz (two biquads on I and Q) before the envelope: the tones are at 400 and 1020 Hz, the stream was 8 kHz wide. About 2 dB.
2. The tones are read from the carrier-synchronous component: a frequency loop (30 Hz average of the de-rotated stream, +-200 Hz range) holds the carrier at 0 Hz,
   a 3 Hz average of it is the phase reference, the in-phase part is band-passed at 400 / 1020 Hz and normalised by the carrier amplitude (not by the noise-driven
   peak of the envelope). The synchronous detector has no squaring loss against noise. Another 2 to 3 dB.
3. False-word guards (the first synchronous version produced single-letter 'E' and 'T' words from noise at 6 dB and below, where the 3.9 receiver produced
   nothing): the carrier line must stand out of the noise in the 3 Hz estimate (5 x the noise variance), a word counts only when the on / off contrast
   of the tone is at least 5 dB, one letter needs five repeats and two letters three (was 3 and 2). With these, in 16 trials at 7 to 8 dB two gave a wrong
   ident (7 dB, signal present); noise alone gave none in 60 runs.
4. The path choice (400 Hz, 1020 Hz, keyed carrier) leaves the keyed carrier for a tone only with 2.5 x the contrast (the synchronous tone depths of a keyed-carrier
   station flicker, ndb_census failed on it before this).
Keyed-carrier stations (no tone) are read as before (envelope): they gain only from item 1.
Test: sens40b guard (ndb at 14 dB, noise only), ndb_rx, ndb_census pass. Simulated only.

## Marine DSC on VHF channel 70 (core/src/marine/marine_dsp.cpp, VhfSlicer)

SNR in 25 kHz. A trial passes when at least half of the units of a clean run decode (16 trials a point, harness entries `dsc` / `dsc-off`).

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point | 4 dB (3 dB: 5 / 16) | **2 dB** (1 dB: 0 / 16) | 2 dB |
| 20 ppm carrier error (3 kHz at 156.5 MHz) | 6 dB (5 dB: 11 / 16, 4 dB: 0 / 16) | 2 dB | 4 dB |
| 50 ppm clock | 4 dB | 2 dB | 2 dB |
| off-centre carrier (5 kHz) | above 5 dB (0 / 16 at 5 dB) | 4 dB | 2 dB or more |
| noise only | 0 of 16 | 0 of 40 per scenario (-40 dB) | |

The VHF path took the 24 kHz stream (a 9 kHz front filter) straight into the FM discriminator: the signal (+-3 kHz deviation, 1300 / 2100 Hz tones) needs about +-5 kHz.
Now a 4.5 kHz two-section Butterworth low-pass sits in front of the discriminator, and the carrier error (the mean of the discriminator over 50 ms, followed by a slow
integrator, +-4 kHz) is taken off before it so that a radio some kHz off does not push the signal into the filter's edge (without the follow-up the 20 ppm case failed outright:
a filter alone gave 0 % at +3 kHz). 4 kHz instead of 4.5 kHz measured 0.3 dB better at AWGN; 4.5 kHz was kept for a transmitter with more deviation than the generator's.
`MarineReceiver::setVhfNarrowChannel(false)` is the 3.9 path. ATIS (atis_rx.cpp) uses the same VhfSlicer and has the narrow path now too (atis_mode passes; not measured).
Tests: sens40b guard (dsc), marine_dsc_* (audit, bands, bitsweep, calls, kat, sweep), marine_basic, marine_engine, atis_mode, marinevhf_mode pass. Simulated only: the generator's deviation is assumed.
