# Sensitivity 4.0, pass C1 (feat40-sensC1): is the "clean" signal clean?

Simulated signals only (each mode's own generator, white noise). Method as in SENSITIVITY-4.0-B.md.

## 1. Clean-signal check of the whole harness

`probe_clean` (tests/sens/probe_clean.cpp, new target) runs every mode of tools/sens/ and reports, per mode: how much of the clean output depends on
`SynthConfig::snrDb` (generators that ignore it), the spectral floor against the signal in the mode's noise bandwidth, and the power in the gaps between bursts
(exact silence = no noise baked in). `clean()` now takes its SNR from `g_cleanSnrDb` (120 by default) so the probe can switch it.

Result for the 100 harness modes:

* **aero** (found in B3): noise baked in at Eb/N0 12 dB; harness fixed in B3.
* **NAVTEX, NAVTEX long, SITOR-B**: the harness asked the generator for `snrDb = 25`, which is noise at 25 dB in 3 kHz over the whole 2 Msps capture:
  the signal power the sweep refers to was about 3 times the real signal power, so every sweep SNR was about 5 dB too high. Now `clean()`.
  NAVTEX (one message a trial, 6 trials a point, SNR in 300 Hz): before 100 % at 8 dB, 0 % at 6 dB; after 100 % at 2 dB, 0 % at 1 dB.
  That is Eb/N0 about 6.8 dB for 100 Bd FSK, the coherent figure: the receiver is at theory, the old table was wrong by 5 dB.
  SITOR-B (units = characters): 57 % at 2 dB, 11 % at 1 dB, 0 at 0 dB.
* All other modes: either exact silence between bursts (gap power under -60 dB), or no noise floor above the signal's own spectral skirts, or they honour
  `snrDb` (inmc: Eb/N0 = snrDb - 20; vdl2/acars/hfdl `modeVal[0]` is a rate factor, not noise). UAT, ISM, DVB-T, ATSC, FM, DVB-C ignore `snrDb` but are driven
  with their own noise-free setting (120 to 200 dB) in the harness.
* Not in the harness at all: `inmsat`, `qo100`, `gnss`, `astro`. Their generators: inmsat sets Eb/N0 `modeVal[0]` (17 dB default) in each carrier it
  builds from aero and inmc, so 60 gives clean carriers; qo100's noise is the transponder's own (sigma 0.05, carriers 0.004 to 0.006); gnss takes C/N0 from
  the simulated sky (`cn0Top`) with a fixed noise floor, so its test already is a C/N0 sweep (`test_gnss_sweep`, SENSITIVITY-3.9-E.md); astro's noise is the signal.

## 2. FM RDS against theory

Generator: RDS = biphase symbol times a 57 kHz carrier of peak 2 kHz, so the mean power is A^2/4 (not A^2/2). Eb/N0 = CNR(200 kHz) - 9.8 dB - 3 dB.
At CNR 16 dB that is 3.1 dB, a symbol error rate of 2 %, and about 65 % of the groups correct with the standard burst trap: measured 70 % (sens_curve fm-rds,
10 trials: 18 dB 92 %, 17 dB 87 %, 16 dB 71 %, 15 dB 51 %, 14 dB 26 %, 13 dB 7 %, noise only 0 groups). At theory; the 16 dB in the old table is a property of the generator's
RDS level, not a receiver gap. Not changed.

## 3. Broadcast rows against theory (sens_curve, 6 trials a point, AWGN)

| mode | 90 % point | reference | |
|---|---|---|---|
| ATSC 8-VSB | 15 dB (92.9 %), 14 dB 5 % | 14.9 to 15.2 dB TOV | at theory |
| DAB mode I | 3 dB (96 %), 2 dB 45 % | about 4 to 5 dB Gaussian | at theory |
| DVB-C 64-QAM | 22 dB (96 %), 21 dB 30 % | about 22 dB | at theory |
| DVB-T 8K 64-QAM 2/3 | 98 % at 15 dB (6 s trials) | Gaussian 64-QAM 2/3 about 16.5 dB (Nordig) | at theory (better than the 17 dB of the 1.5 s table) |
| DVB-T2 8K 64-QAM 2/3 rot. | 74 % at 14 dB, 100 % at 16 dB, 0 at 12 dB | about 14.7 dB | at theory |
| DTMB 64-QAM 0.6 | 73 to 90 % at 16 dB, 0.3 % at 14 dB | about 15 to 16 dB | at theory; the plateau under 100 % at 20 dB is lock time inside the 6 s trial (random start), not noise |
| DVB-S2 QPSK 2/3 | 90 % at 3 to 4 dB Es/N0 | 3.1 dB | at theory |

## 4. The 50 ppm clock "loss" is a trial-length artefact

The 3.6 baseline (and the quick GitHub sweep, run 43134301593) list a 50 ppm clock error as costing ATSC +9 dB, DVB-T +4 dB, DTMB +4 dB, DVB-T2 +3 dB. With trials
of 6 s instead of 1.5 to 2 s (`CURVE_SECS=6 sens_curve <mode> ... 0 50`, same noise seeds) the curve with 50 ppm is the same as with 0 ppm:

| mode | 0 ppm | 50 ppm |
|---|---|---|
| ATSC | 99.7 % at 18 dB, 6 % at 14 dB | 99.7 % at 18 dB, 6 % at 14 dB |
| DVB-T | 98.1 % at 15 dB | 98.1 % at 15 dB |
| DVB-T2 | 73.6 % at 14 dB, 100 % at 16 dB | 73.5 % at 14 dB, 100 % at 16 dB |
| DTMB | 73 % at 16 dB | 90 % at 16 dB |

The receivers learn the clock offset in the first fraction of a second; a short trial counts the units lost while learning and the learning is slower at low SNR.
That is lock time, not sensitivity. No receiver was changed. Harness advice: use trials of at least 6 s when judging a clock-error scenario.

## 5. Other numbers from the quick sweep (2 dB steps, 4 trials, 90 % point)

NAVTEX / SITOR-B with the clean signal: AWGN 3 dB, 20 ppm carrier 5 / 3 dB, TU6 9 dB, combined 5 / 9 dB (the 3.6 table had 7 and more because of the baked noise).
DAB: AWGN 3, TU6 5. ISDB-T: AWGN 18, TU6 **30** (DVB-T 18, DVB-T2 20, DTMB 22 in the same channel): the largest gap seen. A slow Rayleigh channel (10 Hz Doppler) with 90 % of the
units required costs about 10 dB of fading margin without time diversity; DVB-T and DVB-T2 get frequency diversity from long interleavers, ISDB-T mode 3 64-QAM 3/4 in the
harness has no time interleaving (I = 0). Not investigated further here: the cause is not yet separated into channel estimate and diversity.

## Not done

* Harness rows for inmsat, qo100, gnss, astro (generator facts in section 1; none of them is a plain decoder threshold: gnss is a C/N0 sweep already, qo100 is a beacon lock, astro a detection by integration time).
* ISDB-T in TU6 (above).
* No receiver gained a dB in this pass: everything measured is at theory, the errors were in the harness (NAVTEX/SITOR 5 dB, aero 10 dB, clock-error rows).
