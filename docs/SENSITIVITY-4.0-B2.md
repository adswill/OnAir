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

## MPT 1327 control channel (core/src/mpt1327/mpt1327_engine.cpp, new core/include/dect2/dsp/narrow_if.h)

SNR in 12.5 kHz, 12 trials a point (harness entries `mpt1327` / `mpt1327-off`); a trial passes when at least half of the codewords of a clean run decode.

| | 3.9 receiver | 4.0 receiver | gain |
|---|---|---|---|
| AWGN, 90 % point | 9 dB (8 dB: 6 / 12) | **6 dB** (5 dB: 4 / 12) | 3 dB |
| 20 ppm carrier error (3.1 kHz, capped) | above 9 dB (0 / 12 at 9 dB; 11 dB in 3.9-D) | 6 to 7 dB (6 dB: 10 / 12) | 4 dB or more |
| 50 ppm clock | 9 dB | 6 dB | 3 dB |
| off-centre carrier (2.5 kHz) | above 9 dB (3 / 12 at 9 dB) | 6 dB | 3 dB or more |
| noise only | 0 of 12 | 0 of 40 per scenario | |

The 24 kHz decimated stream went straight into the FM discriminator: an IF noise bandwidth of 24 kHz for a signal of +-3.3 kHz (FFSK tones 1.2 / 1.8 kHz, deviation 1.3 to 2.7 kHz). The same
filter-plus-follow-up as the DSC path now sits in front (`NarrowIf`: two-section Butterworth at 5.5 kHz, carrier error = 50 ms mean of the discriminator, slow integrator, +-4 kHz).
4.5 kHz gave 0.7 dB more but cut a 2.7 kHz-deviation transmission (mpt1327_std sweep: 2 wrong results), so 5.5 kHz it is. `MptReceiver::setWeakSignalPath(false)` is the 3.9 path.
Tests: sens40b guard (mpt1327 at 6 dB), mpt1327, mpt1327_mode, mpt1327_chan, mpt1327_std (216-case engine sweep: sample rate, offset, deviation, polarity, clock) pass. Simulated only.

## Wrong-text decodes of the WSJT modes (core/src/hfdig/hfdig_ftx.cpp, hfdig_jt4.cpp)

`test_hfdig_sens40b` (6 to 10 slots a SNR point of 3 stations spread over the passband, white noise; plain and with a dial error of +40 Hz and +100 ppm) now prints the text, SNR and slot of every
decode that was not sent. 10 slots, as the sweep goes down to the mode's last decoding point:

| mode | wrong texts before (plain / faults) | after (plain / faults) | 90 % / 50 % point before -> after |
|---|---|---|---|
| FT8 | 1 / 3 | **0 / 0** | -20 / -21 dB, unchanged |
| JT4 | 1 / 3 | **0 / 0** | -22 / -24 dB, unchanged |
| FT4 | 2 / 2 | 2 / 2 | -15 / -16 dB, unchanged (faults -14 / -16) |
| FT2 | 1 / 1 | 1 / 1 | -12 / -13 dB, unchanged |

Noise-only slots (6 per mode): 0 decodes before and after in all four.

* FT8: the wrong texts were `W9XYZ K1ABC R ED35` (sent: `R-05`), `K0YGG W9XYZ R LO29`, `W9XYZ K1ABC R JJ76`: the a-priori decode with the two calls of a conversation heard in the slot taken as known and about
  16 unknown bits, where an OSD hit with a valid CRC always fits (65536 words against a 14 bit CRC). Printing score and agreement of every such decode (36 of them) showed that all wrong ones had a candidate score of 2.19 to 2.49
  and every right one 2.65 or more, with the same agreement range (0.77 to 0.99), so the floor for that stage went from 2.0 to 2.6 (`HFDIG_FTX_APQSOSCORE`). This costs no measured sensitivity (same 90 / 50 % points, 10 slots a point);
  a station whose call-bearing candidate scores 2.0 to 2.6 and that is decoded only by this stage is lost, and none of the 36 decodes was of that kind.
* JT4: the Fano decoder, which has no CRC, returned the all-zero word (`000AAA 000AAA RA90`, reencoding correlation 0.85 to 0.89) for four candidates; no station sends it. It is rejected now.
* FT4 and FT2: `W9XYZ K1ABC JQ40` (-12 dB), `CQ B83LQK OP93`, `CQ HL2IPI/R R KH63` (-16 dB), FT2 `K1ABC W9XYZ R MI16`, `K1ABC W9XYZ BF14` (-5 dB, a strong station) are unchanged by the floor (the same counts at 2.0 and 2.6): the CQ ones come
  from the a-priori CQ decode with OSD (an agreement test of the unknown bits does not separate them: 0.86 to 0.99 for right and wrong), the others from a candidate beside a decoded signal that is not within the 0.75 tone spacing / 1.5 symbol
  skipped by `nearDecoded`. Open: a plausibility rule for the callsign of an a-priori CQ hit and a wider exclusion zone around decoded signals in the deep stage.
Test: `hfdig_sens40b` guard now also runs FT8 and JT4 at -22 / -23 dB with the dial and clock error and fails on a wrong text (the old code does not fail every run of 4 slots: the wrong decodes are about 1 in 10 slots).
hfdig_ft4, hfdig_ft8, hfdig_ftxsens, hfdig_ftxsmart, hfdig_ftxmulti, hfdig_jt4 pass. Simulated only.

## Tried, measured, not kept

* **Wi-SUN FAN, burst gate** (bins open at 3 x the learned floor instead of 5, close at 1.8 x instead of 2.5): 14 dB 94 % against 13 dB 82 % before, i.e. about 1 dB at the 90 % point and nothing at the 50 % point (10 dB, 54 % both).
  The limit is the discriminator-based start-of-frame and bit decisions (the FM threshold of the 100 kHz channel). A tone-energy detector for h = 1 FSK needs the carrier offset before the preamble search; not done.
* **EDACS, 6.5 kHz channel filter (was 9) with a carrier follow-up** in `FmFront`: 12 dB to 11 dB (12 trials a point, all scenarios). Reverted; the cliff is the BCH check of the frame.
* **FLEX tone symbols**: not pursued again (POCSAG's change lost FLEX pages in the shared channel last time).

## Where the modes stand against theory (from the baseline table of SENSITIVITY-4.0-B.md)

* At or near theory (measured or reasoned, no change): NAVTEX / SITOR-B (tone slicer already; about 2 dB), AIS (3 dB, GMSK 9600), VDL2 (12 dB, D8PSK with FEC), Sigfox (1 dB, DBPSK 100 bit/s: Eb/N0 9 dB),
  Orbcomm (7 dB), LoRa raw (-8 dB), HFDL, Zigbee, ADS-B, FT8 / WSPR / JT9 / Q65 / FST4 (WSJT-X figures).
* Improved here and now near their limit: Argos (the demodulator's half-bit noise bandwidth; about 3 dB from a coherent PLL demodulator, no FEC to help), NDB (about 3 dB from the idents' own keying statistics), DSC VHF, MPT 1327.
* Still open with a known gap: Aero (11 dB for 600 / 1200 bit/s BPSK in 12.5 kHz is about 20 dB Eb/N0; the squaring-line carrier search with a 5.5 x median threshold after 32 averages looks like the limit),
  ISM (a mix of protocols, 50 % at 7 dB), Wi-SUN (see above), ACARS (coherent AM detection with a carrier memory, SENSITIVITY-4.0-A.md), CW with a carrier error (5 dB against 1 dB), FT4 / FT2 wrong texts, FLEX.
* Not examined in this pass: STANAG, Iridium / Inmarsat bursts, LoRa SF12 coherent multi-symbol dechirp, AX.25 9600 and 300, ALE.
