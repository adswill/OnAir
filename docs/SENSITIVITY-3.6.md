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

## New harness modes, 3.6 third pass (feat36-fe)

Added to `tools/sens/sens_modes.h`: `hfdl300 hfdl600 hfdl1200 hfdl1800` (one burst rate each, ACARS downlinks every 2.3 s, SNR = Es/N0 in the 1800 Hz symbol rate), `navtex`, `dsc` (VHF channel 70),
`cdr`, `dvbc` (64-QAM 6.952 Msym/s, Es/N0), `atsc3` (QPSK 8/15, 64800 LDPC, the engine's synthetic signal; the SNR is taken in 5.83 MHz, ATSC counts 6.144 MHz, so subtract 0.2 dB to compare with A/322).
AWGN, full tier, 8 to 12 trials, Mac, same criterion as above. Theory for the HFDL rows: K = 7 convolutional code, soft decisions, Eb/N0 about 3.5 dB for a 10 % burst loss,
converted with the information bits per symbol (300/600/1200/1800 bit/s on 1800 Bd = 1/6, 1/3, 2/3, 1 bit per symbol); 8PSK is worse than that.

| mode | floor (dB) | theory (dB) | gap | note |
|---|---|---|---|---|
| HFDL 300 bit/s | -3 | about -4.3 | about 1.3 | -4 dB: 42 % of bursts |
| HFDL 600 bit/s | -1 | about -1.3 | about 0.3 | |
| HFDL 1200 bit/s | 1 | about 1.7 | about -0.7 | |
| HFDL 1800 bit/s | 4 | about 3.5 to 5 | within the theory error | 8PSK |
| DVB-C 64-QAM | 22 (21 fails) | about 20.5 to 21 for half the RS packets valid (uncoded BER about 6e-3; RS(204,188)) | about 1 | QEF itself needs about 24 dB (BER 2e-4 before RS) |
| ATSC 3.0 QPSK 8/15 | 4 (3 dB: 75 %) | about 2 to 2.5 (LDPC 1.4 to 1.6 on an ideal channel plus pilots and guard) | about 1.5 to 2 | the LDPC decoder alone is at 1.4 dB on an ideal channel (bicm.cpp comment); the rest is OFDM sync / channel estimate; not changed |
| NAVTEX 100 Bd | 9 to 13 (3 dB steps in the noise bandwidth of 300 Hz) | about 7.5 (FSK non-coherent, message without a damaged character) | about 1.5 to 5 | one message a trial, so the 90 % point is coarse: needs a longer trial to claim anything |
| DSC VHF ch 70 | 4 | - | - | 25 kHz noise bandwidth: below the usual FM threshold of the demodulator, the AFSK is robust |
| CDR | -4 | - | - | 1 MHz noise bandwidth, LDPC; theory not worked out |

NAVTEX needs 40 s trials: a trial that starts in the middle of a message must still hold a whole one (8 s message, 3 s idle), and the receiver lists a repeat only once.

Nothing was changed in a receiver to close a gap in this pass: no mode was clearly more than 1.5 dB from theory with a one-transmitter signal (ATSC 3.0 and NAVTEX are the open candidates).

## HFDL "why bad" counters

`HfdlTelemetry::why*` (core/src/hfdl/hfdl_rx.cpp), shown in the Receiver tab as "why bad": `whyPreambleRejected` (the coarse search fired but the frequency/coherence/M1 check said it is
no preamble), `whyShort` (fewer than 4 octets out of the decoder), `whySpduFcs` (a squitter whose check failed: FEC did not repair it), `whyMpduFcs` (data PDU header check failed),
`whyLpduFcs` (an LPDU with a bad check inside a good MPDU), `whyUnknownLpdu` (good check, type not decoded). Bursts that failed = `whyShort + whySpduFcs + whyMpduFcs` = `blocksBad`
(tests/hfdl/test_hfdl_why.cpp). Visual check of the Receiver tab: not done (needs an HFDL source in the GUI).

## Digital voice modes: capture range and "why isn't it decoding" (feat36-voiceq, 2026-11-02)

New harness scenarios for every narrowband mode (noise bandwidth up to 25 kHz): `cfop4k` / `cfom4k`, the carrier 4 kHz above / below the channel centre (a cheap
oscillator, or a mistuned dial: 20 ppm at 446 MHz is 9 kHz). Quick tier (2 dB steps, 4 trials), threshold SNR in dB, before -> after:

| mode | awgn | cfo20ppm | +4 kHz | -4 kHz | what changed |
|---|---|---|---|---|---|
| D-STAR (6.25 kHz) | 7 -> 7 | 9 -> 7 | 19 -> 9 | 19 -> 11 | frequency capture while searching (the fixed +-4.5 kHz filter cut the GMSK signal) |
| NXDN (6.25 kHz) | 9 -> 9 | 9 -> 9 | 13 -> 9 | 13 -> 9 | MeanAfc follows a mean error of 2.5 kHz or more (was 4.7 kHz) |
| dPMR (6.25 kHz) | 7 -> 7 | 7 -> 7 | none -> 9 | none -> 7 | frequency capture while searching |
| P25 phase 1 | 9 -> 9 | 9 -> 7 | 11 -> 5 | 13 -> 7 | MeanAfc follows a mean error of 2.5 kHz or more |
| DMR | 5 | 5 | 5 | 5 | already fine (no change) |
| TETRA | 10 | 10 | 10 | 10 | harness wired now (see below) |

`MeanAfc` (core/include/dect2/dsp/mean_afc.h) has a fine regime: the discriminator mean over four 20 ms windows with a modulation-sized spread is followed when it
exceeds `fineMin` (2.5 kHz default; D-STAR and dPMR use 1.2 kHz because they move the oscillator only while searching, never mid-call: a move in a call upsets the level
tracking and cost frames in tests/dstar/test_dstar_rx.cpp). The first attempt with 1 kHz everywhere lost 3 frames of a +-1 kHz NXDN call and a lot of D-STAR voice:
that is why the threshold is 2.5 kHz for the modes that move in a call.
Guards: tests/{dstar,nxdn,dpmr,p25}/test_*_sens.cpp now check a carrier +-4 kHz off at 14 dB (at least 70 % of the clean blocks).

**TETRA in the harness** (it was "no valid output"): the test signal is made for the centre 392.5 MHz (`kTetraTestCentreHz`) and the receiver is a capture-block
decoder, so it needs `setCenterHz(kTetraTestCentreHz)` and the threaded session. AWGN 10 dB in 25 kHz (= Es/N0 about 11.4 dB, the figure of differential pi/4-DQPSK
before the channel code), lock 3.3 s (it is two capture blocks, not the demodulator), TU6 14, combined 16, static echo 15 (9 with a -6 dB echo at 5 us is the flat
fade of the echo: the mean power is the same, a worst-phase static echo takes 6 dB off the channel; nothing to fix in the receiver). Capture-bound lock time is not changed.

Gaps left (measured, not changed): D-STAR TU6 17 dB (7 AWGN; GMSK with no interleaving, FEC only on the header: about 9 dB of Rayleigh fading is the usual cost of
non-coherent FSK without coding), D-STAR lock 0.8 s (the late entry waits for two data syncs 420 ms apart; one strong sync would halve it but a single 24 bit match
false-locks on noise about once in 30 s), NXDN / dPMR / P25 AWGN 7 to 9 dB against DMR 5 (not comparable: the units counted differ; the earlier campaigns already moved
them by 5 dB). Not measured: soft-decision FEC gains, symbol-timing error sweeps beyond the 50 ppm clock scenario.

### Why isn't it decoding (Receiver tab)

`voiceWhy()` (core/include/dect2/voice/voice_why.h, per-mode adapters in voice_why_modes.h) reads each receiver's own counters and names the stage that stops the
signal: no power at the channel, no frame sync, syncs but the frame identity (NID, LICH, slot type) unreadable, identity fine but FEC / CRC fails, decoding with over a
third of the blocks lost, decoding well. DMR, P25, NXDN, D-STAR and TETRA show it as the first line of the Receiver tab, in 13 languages (tests/voice/test_voice_why.cpp).
The visual check of the tab was not done (the app was not built here; CI compiles it).
