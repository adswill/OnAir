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
