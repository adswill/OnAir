# Sensitivity 4.0, pass B3 (feat40-sensB3)

Branch feat40-sensB3 on integ-40. Every figure is from simulated signals (each mode's own generator, complex white noise, the impairments
named); no off-air recording was used. Method as in SENSITIVITY-4.0-B.md: `test_sens40b3` (ctest `sens40b3`) holds the receiver changed in this
pass in two forms in one binary, `<mode>` (4.0) and `<mode>-off` (the 3.9 path of the same receiver: `IsmReceiver::setNarrowRetry(false)`), with
shared noise seeds. `ONAIR_SENS_POINTS=... ONAIR_SENS_TRIALS=20 ONAIR_SENS_MODES='^ism'` gives the pass counts, `ONAIR_SENS_POINTS=-40` the
false-decode check. New tools: `sens_lora_sf` (LoRa frames per spreading factor), `CURVE_SECS` and `CURVE_OPT0` (packet: 1 = 1200, 2 = 9600 baud)
for `sens_curve`.

## Summary

| item | result |
|---|---|
| Aero | **no receiver gap**: the sweep's reference was wrong (below). 90 % point -1 dB SNR in 12.5 kHz, at coherent theory. Harness fixed. |
| ISM sensors | **gain**: 50 % of the units at 3 dB instead of 6.5 dB, 90 % at 8 dB instead of 10 dB; 0 false decodes |
| FT8 / FT4 / FT2 wrong texts | **fixed** for the a-priori calls stage (FT8 measured), no loss of weak decodes |
| LoRa SF7 to SF12 | no gap: 50 % points -9 / -17.9 / -23.5 dB (SF7 / SF10 / SF12), 1.5 to 3.5 dB better than the SX127x figures |
| AX.25 9600 baud | no gap: 50 % at 8.5 dB in 25 kHz (Eb/N0 12.7 dB, theory for the G3RUH FM link 12 to 14 dB) |
| ALE | no gap: 90 % at -6 dB, 50 % at -7.5 dB in 3 kHz (8-FSK 125 Bd with the Golay and triple redundancy) |
| wM-Bus single PHYs | at theory (below), not changed |
| CW with a carrier error | no gap (the 4 dB in sens_sweep was trial statistics): same curve at 0, 100, 140 and 250 Hz |
| STANAG 4285 / 4539 / 4481, MIL-STD-188-110 | these are identifiers of the waveform family (core/src/milhf), not demodulators; their threshold is about 5 dB in 3 kHz (see below), not changed |

## Aero: the 11 dB was the generator's own noise

`aero_gen.cpp` bakes noise into its test signal at Eb/N0 `modeVal[0]` (12 dB per carrier by default; the SynthConfig SNR is not used). The
sweep harness (`clean()` = SNR 120 dB, then its own noise at the sweep SNR, referred to the signal's mean power) therefore measured the sum of
that 12 dB and the added noise, referred to a power that was mostly the baked noise: a sweep SNR of 11 dB was an Eb/N0 of about 0.6 dB per channel bit
(the receiver's own estimate in the debug trace: 1 dB). The "20 dB from theory" of SENSITIVITY-4.0-B2.md came from that.

* The sens harness generator for `aero` now asks for 60 dB (`SynthConfig::modeVal[0] = 60`), so the sweep SNR is the only noise.
* Result (`CURVE_SECS=30 sens_curve aero`, 8 trials a point, 1200 and 10500 bit/s carriers together, units = SUs with a good CRC):
  98 to 99 % from 8 down to 2 dB, 96 % at 1 dB, 89 % at 0 dB, 76 % at -1 dB, 30 % at -2 dB, 0 at -3 dB. Same with a 250 Hz carrier error
  (74 % at -1 dB). In Eb/N0 per channel bit (sweep SNR + 0.3 dB): 90 % at about 0 dB.
* The demodulators alone (`test_aero_sweep`, 99 % of the SUs good): 600 bit/s down to 3 dB, 1200 bit/s 3 dB, 10500 bit/s 1 dB Eb/N0 per channel bit; for a
  K = 7 rate 1/2 code with soft decisions about 1.4 dB on coherent BPSK / QPSK, 2 to 3 dB more for differential MSK. The carrier search (squared signal,
  lines at 2 df +- Rb/2) was looked at: with a band-limit before the squaring it did not change the result and was dropped; at 2 dB Eb/N0 the 1200 bit/s
  search finds its lines after 4 blocks (metric 39 against a threshold of 12).
* A short trial (6 s) still loses units to the 2 to 3 s the squaring search needs on a 2.5 Hz grid; with a 30 s trial 11 dB reads 86 % instead of 52 % in the
  old harness. Not changed.
* Other generators with noise of their own and no use of the SNR in their config (found by grep; not measured here): `inmsat_gen.cpp`,
  `qo100_gen.cpp`, `gnss_gen.cpp`, `astro_gen.cpp`.

## ISM sensors (core/src/ism/ism_rx.cpp)

Scene of 14 devices at 433.92 MHz (the engine's test scene), 8 s trials, a trial passes with at least half the packets of a clean run. Passes of 20, same seeds:

| SNR in 100 kHz | 12 | 10 | 8 | 6 | 5 | 4 | 3 | 2 |
|---|---|---|---|---|---|---|---|---|
| 3.9 path | 20 | 19 | 13 | 3 | 0 | 0 | 0 | 0 |
| 4.0 path | 20 | 20 | 19 | 12 | 14 | 11 | 10 | 0 |

With 20 ppm carrier error, 50 ppm clock error and an off-centre carrier the 4.0 counts are the same to within 1 or 2 trials at 3 to 8 dB (3 dB: 7, 8, 6 of 20).
Noise only: 0 of 30 trials gave a unit (-40 dB point, 4 s trials), 0 of 20 at the other points. CPU: only bursts no protocol read pay for the extra passes.
Per device (6 trials each, `tmp` run, not kept): at 8 dB the 3.9 path reads the nine OOK devices (Nexus, TX141TH, Acurite, WH1080, F007TH, Oregon v2.1 and v3, Schrader,
EV1527) and none of the FSK ones; the 4.0 path reads the same nine down to 5 dB (3 dB: 1 to 6 of 6) and the three TPMS (Toyota, Renault, Ford) at 8 dB. Efergy and
CurrentCost (FSK) still need 12 dB.

1. **Narrow-envelope retry** (OOK): the standard envelope is the carrier boxcar-filtered over 20 us (+-25 kHz). A burst that no protocol reads is sliced again with 40, 80
   and 160 us boxcars (+-12.5, 6, 3 kHz) after the carrier is measured from the burst's own phase step (the FFT's bin centroid is good to a few kHz only). The
   protocols' own checks (CRC, parity, repeats) accept or refuse the result, so the false-decode rate is the protocols'. The noise falls with the filter; the shortest
   OOK element of these devices is about 100 us.
2. **FSK by tone energy**: when no protocol reads an FSK train, the chips are judged by the energy at the upper against the lower tone over half a chip
   (tones from the discriminator's two clusters, chip from the run lengths) instead of by the discriminator's mean frequency, whose clicks fill a weak burst with
   chip errors. A burst whose wide envelope was up only half the time (it was 90 % before: weak FSK carriers dip) is tried as FSK after the OOK retries failed, the
   dropouts bridged, and only a train a protocol reads is kept.

Tests: `sens40b3` guard (ism at 6 dB: at least 3 of 8 trials, the 3.9 path gets 1 of 8; noise only gives no unit), ctest `ism_*` (8 tests) pass.

## FT8 and the other a-priori decodes

At -12 to -21 dB (the stations decode down to -20) `test_hfdig_sens40b` printed texts that were not sent next to the stations: `W9XYZ K1ABC R ED35`, `K0YGG W9XYZ R LO29`,
`CQ 1Y3GWX/R R DI01`. The real cause, measured over 86 a-priori decodes of 30 slots (`how` = 2): the wrong texts are decodes of **noise candidates**, not of the stations. The
a-priori stage takes the calls of a conversation heard on the band as known (58 + 3 bits of 91), the 16 bits left over plus the 14 bit CRC leave 65536 words that are all
valid codewords, and the LDPC decoder with the known bits clamped finds one that fits the soft bits of a pure noise candidate (agreement with the soft bits at the unclamped
positions 0.80 to 0.88, as good as a weak real signal's). What separates them is the candidate's sync score, as the earlier note found:

| | candidate score | agreement at the unclamped positions |
|---|---|---|
| wrong texts (24 decodes) | 2.06 to 2.49 | 0.80 to 0.88 |
| right texts, a-priori (62 decodes) | 2.65 to 4.96 | 0.84 to 0.97 |

(the agreement alone does not separate them: both ranges overlap.)

The a-priori stage already had a floor on the score (`HFDIG_FTX_APQSOSCORE`, 2.0). It is 2.6 now. B2 had reverted the same floor because `hfdig_ftxsmart` failed with it; the
failure was the test, not the receiver: its check counted `decodes - weak - 5` with four strong messages sent, so the count of 5 hid one wrong text per run and went
negative as soon as it was gone (-1 a run). The test counts four now and fails on any decode that was not sent. With the floor 2.0 the corrected test fails (4 false decodes at
-21 dB and at -22 dB); with 2.6 it passes with the same 4 of 4 weak messages decoded at both SNRs.

FT8 sweep (30 slots a point, 3 stations a slot, white noise, plain and dial +40 Hz / +100 ppm): see the numbers below.

| SNR in 2500 Hz | -12 .. -19 | -20 | -21 | -22 | wrong texts, 30 slots |
|---|---|---|---|---|---|
| floor 2.0 (3.9 / B2), plain | 100 % | 97 % | 69 % | 7 % | 11 |
| floor 2.6, plain | 100 % | 97 % | 69 % | 7 % | **1** |
| floor 2.0, dial +40 Hz and +100 ppm | 100 % | 97 % | 66 % | 14 % | 15 |
| floor 2.6, dial +40 Hz and +100 ppm | 100 % | 97 % | 66 % | 14 % | **1** |

The decode shares are identical to the percent; noise-only slots: 0 decodes. The one wrong text left, `CQ 1Y3GWX/R R DI01` (-21 dB, slot 29), is from the a-priori **CQ** stage with OSD (score
2.56 / 2.57); real weak CQ decodes of the same stage score 2.56 and up (46 decodes in the 30-slot run, 12 in `hfdig_ftxsens`), so a floor there would cost decodes: not changed. FT4 and FT2 take the
same floor in the calls stage; their sweeps were not repeated.

Tests: `hfdig_ftxsmart` (corrected), `hfdig_ftxsens`, `hfdig_ftxmulti`, `hfdig_ft8` pass.

## Checked against theory, not changed

* **LoRa** (`sens_lora_sf`, 125 kHz, CR 4/5, 16 B payload, CRC, 24 frames a point, through the Signal Lab demodulator): 50 % of the frames at SF7 -9 dB, SF10 -17.9 dB, SF12 -23.5 dB
  (Semtech's SX127x figures -7.5 / -15 / -20 dB), 90 % at -8 / -17 / -22 dB. SF12 with a 2400 Hz carrier error and 40 ppm clock: 94 % at -21 dB, 56 % at -23 dB; 6 kHz carrier
  error: 88 % at -22 dB; 50 ppm clock: 96 % at -22 dB. Only 100 ppm costs about 3 dB at SF12 (50 % at -20 dB); crystals are 20 to 40 ppm. 0 frames with a text that was not sent.
* **AX.25**: 1200 baud 50 % at 4 dB, 9600 baud (G3RUH) 50 % at 8.5 dB in 25 kHz, 0 false units. Eb/N0 12.7 dB for 9600 baud (theory for a limiter-discriminator link 12 to 14 dB). 300 baud HF packet is
  not in the packet receiver (it is 1200 and 9600 baud on one FM channel).
* **ALE** (`sens_curve ale`, units = decoded words): 98 % down to -4 dB, 97 % at -6, 16 % at -8 dB in 3 kHz.
* **wM-Bus**: 90 % at 14 dB, 50 % at 11 dB in 100 kHz for the T1 test signal (66.7 kbit/s, 100 kchip/s 2-FSK, 3-of-6 code, about 800 bits a frame). Non-coherent FSK needs Eb/N0 11.9 dB for a bit error rate
  of 1e-3 and 13.4 dB for 1e-4, a frame of that length fails above about 1e-3; the receiver already picks the nearest of the 20 3-of-6 words by soft correlation (3.9). At theory to within 1 dB.
* **STANAG and MIL-STD-188-110 modes** are identified, not demodulated (core/src/milhf: occupied band, baud line, order, frame period; "identification only"). 20 of 20 identified at 6 dB SNR in 3 kHz, 1
  to 6 of 20 at 3 dB, 0 at 0 dB (Link 11 like tone combs hold to -6 dB). A threshold of about 5 dB; not changed.

## Not done

* The FSK devices Efergy and CurrentCost of the ISM scene still need 12 dB (their deviation or chip length is not found at the threshold).
* The a-priori CQ stage (OSD) still gives one wrong text in 30 slots of FT8 (score not separable from right ones); FT4 / FT2 sweeps with the new floor were not repeated.
* Aero: a faster carrier search for short trials (the squaring search needs 2 to 3 s on a 2.5 Hz grid).
