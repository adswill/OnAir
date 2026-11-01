# Sensitivity against theory, 3.6 (feat36-sens)

Measured with `sens_sweep` (see SENSITIVITY.md), AWGN, full tier (8 trials a point, 1 dB steps), synthetic transmitters through the engine's own receivers.
Floor = lowest SNR (dB, in the mode's noise bandwidth, active-burst power) with at least 90 % of trials that decode half of the clean run's valid units
(CRC / FEC passed). The criterion is about "usable", not zero errors. Linux column = committed baseline / 10-30 campaign table (GitHub runner); Mac column = this
run on the Mac (same code, same seeds); they agree to 1 dB except FM RDS and ACARS (+3 on the Mac, thread timing; not investigated).

Theory = rough figure for an ideal receiver at the same criterion, worked out from the standard's own C/N tables where there is one (DVB: EN 300 744 A.1,
TS 102 831 table 44/46) and otherwise from Shannon / textbook BER (noncoherent FSK `0.5 exp(-Eb/2N0)`, D8PSK, K=7 convolutional codes) plus pilot and guard
overhead. Treat the theory column as +-1.5 dB. "Gap" = floor minus theory; negative means the criterion here is looser than the textbook one.

| mode (test signal) | floor Linux | floor Mac | theory | gap | note |
|---|---|---|---|---|---|
| DRM30 mode B, 64-QAM, R about 0.6 | 16 | 16 | about 15.5 | about 0.5 | MLC, 4 soft iterations, Wiener channel estimate: at theory |
| DRM30 mode B, 16-QAM, level 0 (new `drm16`) | - | 9 | about 9 | about 0 | the most robust DRM30 audio setting |
| DRM+ mode E, 4-QAM (new `drmE`, 100 kHz) | - | 2 to 3 | about 2 | 0 to 1 | cliff between 0 dB (0 %) and 2 dB (83 %), 3 dB 100 %: the "about 3 dB" owner figure |
| DAB mode I | 3 | 3 | about 4 | about -1 | FIB CRC criterion is looser than BER |
| DVB-T 8K 1/8 64-QAM 2/3 | 15 | 15 | 16.7 | about -1.7 | clean from 100 ms: looser than QEF |
| DVB-T2 8K PP 64-QAM 2/3 | 14 | 14 | about 13.9 | about 0 | |
| FM broadcast RDS (2 kHz injection) | 16 | 19 | about 15 | 1 to 4 | FM discriminator noise rises with f^2 at 57 kHz; Eb/N0 5 dB assumed |
| DMR | 5 | 5 | about 7 | about -2 | FEC of the voice frames helps the criterion |
| ADS-B (12 aircraft at different levels) | 14 | 14 | about 9.5 | about 4.5 | sum power of 12 aircraft, weak ones fail first: partly the test mix |
| AIS (AIS 1 + 2, many ships) | 16 | 16 | about 7 | about 9 | slope of the success curve is gentle (units fall from 22 to 10 over 4 dB): level mix of ships, not one signal |
| ACARS (6 aircraft) | 7 | 10 | about 2 | 5 to 8 | same remark; 3 channels |
| VDL2 (D8PSK) | 12 | 11 | about 13 | about -2 | RS FEC |
| POCSAG pager | 1 | 0 | about 0 to 2 | about 0 | at theory |
| APRS / AX.25 1200 baud AFSK | 5 | 5 | about 1 to 3 | 2 to 4 | three correlator sets times five slicers already (multi-slicer TNC) |
| P25 | 6 | skipped locally (no valid units on the Mac run) | about 7 | about -1 | |
| TETRA | - | - | - | - | harness has no valid output on its test signal (known, SENSITIVITY.md) |

Reading: every mode with one clean transmitter (DRM, DAB, DVB-T/T2, DMR, POCSAG, VDL2, P25) is within about 1 dB of an ideal receiver at this criterion.
The large gaps (AIS, ACARS, ADS-B, APRS) are all multi-transmitter test signals where the "SNR" is the mean over bursts of different level, and FM RDS,
where physics (f^2 noise) costs most. None of them can be called a receiver defect without a one-transmitter variant of the test signal.

## Experiments in 3.6

* DRM, phase noise (`phasenoise`, 1 Hz linewidth, 46.9 Hz / 37.5 Hz spacing): `drm` (64-QAM) did not decode even at 40 dB, `drm16` needs 13 dB instead of 9.
  Added a common-phase-error estimate from the three frequency pilots (measured against the channel estimate, weighted by phase noise / (phase noise + pilot
  noise)). Result: 16-QAM 13 -> 14 dB, no gain: the loss at 1 Hz is inter-carrier interference (about 15 dB for 64-QAM, 12 dB for mode B at 1 Hz) that no
  phase correction removes, not a missing common rotation. Reverted. A 0.1 Hz oscillator (a TCXO SDR) is not affected.
* DRM MSC: already 4 soft iterations of level-by-level Viterbi with hard-decision feedback; the next step (BCJR soft feedback) is a 0.5 to 1 dB candidate for
  64-QAM, not done here.
* Harness: modes `drm16` and `drmE` added to `tools/sens/sens_modes.h`; `--mode '^drm'` runs all three.

## Fairness and receiver work, 3.6 second pass (feat36-sens2)

New one-transmitter harness modes (`adsb1`, `ais1`, `acars1`, `tools/sens/sens_modes.h`), AWGN, 6 to 12 trials, same criterion. Their SNR is that of the only transmitter, so no level mix and no overlapping bursts.

| mode | multi-transmitter floor (dB) | one-transmitter floor (dB) | theory | verdict |
|---|---|---|---|---|
| ADS-B | 14 | 2 | about 9.5 (pessimistic) | the 4.5 dB gap was the test mix: one aircraft is better than the theory figure. The receiver already has soft-guided 1 to 2 bit repair against the tracker's known ICAO list (adsb_track.cpp) |
| AIS | 16 | 3 | about 7 | the 9 dB gap was the level mix (ships 0.1 to 1 amplitude): one ship is 4 dB better than theory |
| ACARS | 7 (Mac 10) | 5 before, 4 after | about 2 | a real gap of about 3 dB for one aircraft; 1 dB closed (see below); 6 aircraft on the Mac 10 -> 6 |
| APRS AFSK, FM RDS | 5, 16 | - | - | not changed: the packet receiver already runs 3 correlator sets x 6 slicers plus bit-fix; both sit at the FM discriminator threshold, no cheap gain found |

ACARS change (`core/src/acars/acars_chan.cpp`): bit-clock correction gain 0.04 -> 0.015 per bit and carrier-reference memory 0.9 -> 0.97 per bit. Steady-state timing jitter and reference noise fell, the cliff moved about 1 dB lower (at 4 dB: 42 % -> 83 to 100 % of trials). cfo20ppm, sro50ppm, tu6, combined unchanged or 0 to 1 dB better; echo scenario and `offcentre` (harness shifts the signal 105 kHz out of the channel the receiver is set to, a harness artefact for every narrowband mode) not affected. A timing gain of 0.007 gave no further gain.
False decodes: the sweep's noise-only false-lock rate stays 0.000 for acars, acars1, ais1, adsb1; no AIS/ADS-B receiver code was changed, and no CRC-only bit repair was added to AIS (16-bit CRC, a one-bit repair would accept about 0.4 % of damaged 256-bit frames by chance; the ship already beats theory).
