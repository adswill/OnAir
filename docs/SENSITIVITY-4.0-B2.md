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
