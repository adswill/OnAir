# Sensitivity 4.0, lane A, part 2 (feat40-sensA2)

Same method as SENSITIVITY-4.0-A.md: `sens_curve` (generator signals, random start and carrier phase, white noise, SNR in 10 kHz for sondes),
fraction of the clean-signal units decoded, 10 trials a point (8 for iMet-54), "before" = integ-40 at b0a1be3e5. Simulated signals only.

## 1. Narrow channel path for RS92, DFM and iMet-54

The 4.5 kHz half-width path next to the wide one (RS41, part 1) now also feeds the symbol chains of RS92 (4800 Bd), DFM (2500 Bd) and
iMet-54 (4800 Bd). Change: `narrowChannel()` returns true in those three decoders. No new DSP.

| type, condition | 90 % point or fraction at the cliff, before | after | gain |
|---|---|---|---|
| RS92 AWGN | 8 dB 8 %, 6 dB 0 % | 8 dB 95 %, 6 dB 68 % | about 2.5 dB |
| RS92 +8 kHz (20 ppm) | 8 dB 5 % | 8 dB 95 %, 6 dB 70 % | about 2.5 dB |
| RS92 +150 kHz off-centre | 8 dB 14 % | 8 dB 95 %, 6 dB 71 % | about 2.5 dB |
| RS92 50 ppm clock | 8 dB 8 % | 8 dB 95 %, 6 dB 68 % | about 2.5 dB |
| DFM AWGN | 8 dB 50 %, 6 dB 0 % | 8 dB 60 %, 6 dB 40 % | about 1.5 to 2 dB at the 50 % point |
| DFM +8 kHz | 8 dB 10 %, 6 dB 0 % | 8 dB 70 %, 6 dB 30 % | about 2 dB |
| DFM 50 ppm clock | 8 dB 50 %, 6 dB 0 % | 8 dB 60 %, 6 dB 40 % | about 1.5 dB |
| DFM +150 kHz | 8 dB 10 % | 8 dB 70 %, 6 dB 50 % | about 2 dB |
| iMet-54 AWGN | 19 dB 91 %, 16 dB 3 % | 19 dB 98 %, 16 dB 98 %, 13 dB 20 % | about 3 dB |
| iMet-54 +8 kHz | 19 dB 93 %, 16 dB 0 % | 19 dB 98 %, 16 dB 96 %, 13 dB 10 % | about 3 dB |

(RS92 and DFM curves are in the 4 to 14 dB range, 2 dB steps; iMet-54 in the 10 to 22 dB range, 3 dB steps.) Noise-only runs: 0 units
before and after for all three (4 runs of 4 s each).
Not kept: MRZ (the narrow path made `sonde_extra_rf` fail: nothing decoded at any carrier error, its deviation is wider than the filter in the
generator) and Meisei iMS-100 (the harness decodes 0 % before and after, so it cannot be measured; left as it was). M10 / M20 keep the wide
path: at 9600 Bd they need more than the 4.5 kHz half width.
Test: `sonde_sens40` extended (RS92 8 dB at 0 and +8 kHz 4 of 4 trials, iMet-54 16 dB +8 kHz 4 of 4, DFM 8 dB +8 kHz 2 of 4; the 3.9 receiver
fails these). `sonde_extra_rf`, `sonde_rf_dfm`, `sonde_rs92_rf`, `sonde_all_rf`, `sonde_interf`, `sonde_basic`, `sonde_rs92_kat` pass locally.
Real RS92, DFM and iMet-54 deviations and skirts are not measured: the generator's values are assumed.

## 2. EDACS and Motorola Type II: narrower channel filter plus a carrier follow-up (FmFront)

Cause. The control-channel receivers (shared `FmFront`) used a 9 kHz (EDACS) and 7 kHz (SmartNet) half-width channel filter, much wider than
the signal (h = 0.29, 9600 and 3600 Bd), so the discriminator sat below its threshold early. Narrowing the filter alone cost 2 to 5 dB when the
carrier was off its nominal frequency (3.1 kHz error: EDACS 12 dB 77 % -> 72 %, SmartNet 3 dB worse; the earlier SmartNet attempt in
SENSITIVITY-3.9-D.md failed for this reason).
Change (`core/include/dect2/dsp/spectral_afc.h`, used by `FmFront::setAfc()` in `fm_front.h`): the mean power spectrum of the resampled channel is averaged over 0.8 s, the noise
floor is the quietest quarter of the bins, and the centroid of what stands above 1.4 x the floor inside +-9 kHz moves the mixer (steps of 80 % of
the centroid, dead band 500 Hz because the centroid of a short FSK stretch is data dependent by a few hundred Hz, at most 20 kHz in all). Unlike the
discriminator mean, the centroid is not pulled to the filter centre by the noise. EDACS now uses a 6.5 kHz filter, SmartNet 5 kHz, both with
`setAfc(true)`. (EDACS 5.5 kHz was 0.5 dB better in the generator; 6.5 kHz was kept as margin for real deviations. SmartNet 4 kHz was 1 dB better
at 2 dB; 5 kHz kept.) LTR uses the same FmFront but is not in the harness: unchanged. CPU: one 256 to 1024 point FFT per 3 to 13 ms of channel.

| | 90 % of units before | after | gain | fraction before / after at the cliff |
|---|---|---|---|---|
| EDACS AWGN | 12.8 dB | 11.3 dB | about 1.5 dB | 12 dB 75 % / 98 %, 10 dB 4 % / 45 % |
| EDACS +3.1 kHz | 12.7 dB | 11.5 dB | about 1.2 dB | 12 dB 77 % / 92 % |
| EDACS -6 kHz | > 14 dB | 12.3 dB | > 2 dB | 12 dB 33 % / 82 % |
| EDACS 50 ppm clock | 12.8 dB | 11.3 dB | about 1.5 dB | 12 dB 74 % / 98 % |
| SmartNet AWGN | 5.5 dB | 3.8 dB | about 1.7 dB | 4 dB 72 % / 93 %, 2 dB 0 % / 20 % |
| SmartNet +3.1 kHz | 7 dB | 4.5 dB | about 2.5 dB | 6 dB 39 % / 95 %, 4 dB 0 % / 79 % |
| SmartNet 50 ppm clock | 5.5 dB | 3.8 dB | about 1.7 dB | 4 dB 68 % / 94 %, 2 dB 1 % / 26 % |

(SNR in 12.5 kHz, 8 trials a point; EDACS 16 dB and above 99 to 100 % both ways with +3.1 kHz 98 % after, a small settling cost.) Noise-only runs: 0 units
before and after. The AFC needs about 1 s of carrier to settle; the control channels are continuous.
Test: `edacs_sens40` (tests/edacs/test_edacs_sens40.cpp): EDACS 12 dB at 0, +3.1 and -6 kHz, SmartNet 4 dB at 0 and +3.1 kHz, 6 dB at +3.1 kHz,
noise alone: all pass; the 3.9 values above fail the same thresholds. `edacs_rx`, `edacs_follow`, `edacs_provoice`, `ltr_edacs_mode`, `ltr_rx`,
`smartnet_rx` pass locally. Simulated signals only: the real deviations of EDACS and Type II control channels, and a real radio's drift, are not
measured. `sens_curve` now also knows the POCSAG, FLEX, EDACS, SmartNet, MPT 1327, Fusion, DVB-S, ALE and APT entries of sens_modes_d.h.

## 3. Yaesu Fusion (C4FM): narrow channel filter that follows the carrier

The receiver's channel filter was 8 kHz (windowed sinc) for a C4FM signal of about +-3 kHz. A 5 to 5.5 kHz filter gave about 3 dB more
sensitivity with the carrier on its nominal frequency but lost the signal at 3.1 kHz error. Change (`core/src/fusion/fusion_rx.cpp`): the same
`SpectralAfc` runs in open loop on the 100 kS/s stream before the filter (nothing is retuned: a retune lost one frame of a 0.7 s
transmission in `fusion_sweep`); until two looks agree the 8 kHz filter stays, then a 5.5 kHz filter with complex taps centred on the measured
carrier takes over, so the discriminator sees the same signal. CPU: one FFT per 5 ms of stream, a complex instead of a real FIR after the lock.

| Fusion (SNR in 12.5 kHz, 8 trials) | before: 12 / 10 / 8 / 6 dB | after: 12 / 10 / 8 / 6 dB | gain |
|---|---|---|---|
| AWGN | 99 / 95 / 64 / 9 % | 99 / 99 / 92 / 51 % | about 2.5 dB |
| +3.1 kHz carrier | 99 / 95 / 73 / 12 % | 99 / 98 / 92 / 49 % | about 2.5 dB |
| -6 kHz carrier | 12 / 0 / 0 / 0 % | 90 / 89 / 83 / 38 % | decodes where 3.9 does not |

Noise-only: 0 units. The first second of a carrier-error signal is decoded with the old filter, so the first transmission after tuning at the
threshold is where the 3.9 numbers apply. `fusion_rx`, `fusion_sweep`, `fusion_mode`, `fusion_interop` pass. `edacs_sens40` has the Fusion rows
(7 dB at 0 and +3.1 kHz, 10 dB at -6 kHz).

## 4. MPT 1327 control channel: narrow filter that follows the carrier

The engine receiver (`core/src/mpt1327/mpt1327_engine.cpp`) decimated to 24 kS/s with a plain average and took the discriminator of the whole
+-12 kHz: no channel filter. Change: `SpectralAfc` in open loop on the 24 kS/s stream; once two looks agree the stream is shifted to 0 Hz,
filtered by a 4.5 kHz (two-section) low-pass on I and Q, shifted back (so the discriminator values keep their frequencies) and handed to the same
discriminator. Until then it is the old path. 3.5 kHz was 1 dB better still in the generator, 5.5 kHz 1.5 dB worse; 4.5 kHz kept as margin.
The harness's score plateaus at about 70 % of the clean units even at 14 dB, before and after (the SNR figures below are relative to that).

| MPT 1327 (SNR in 12.5 kHz, 8 trials) | before: 10 / 8 / 6 / 4 dB | after | gain at the 50 % of plateau |
|---|---|---|---|
| AWGN | 67 / 47 / 15 / 0 % | 70 / 64 / 56 / 25 % | about 2.5 dB |
| +3.1 kHz carrier | 51 / 14 / 0 / 0 % | 61 / 57 / 42 / 10 % | more than 3 dB |
| -5 kHz carrier | 13 / 0 / 0 / 0 % | 54 / 45 / 17 / 1 % | decodes where 3.9 does not |

Noise-only: 0 units. 50 ppm clock: no change in the harness number (the 6 s test signal does not see it). `mpt1327`, `mpt1327_mode`, `mpt1327_chan`,
`mpt1327_std` pass; `edacs_sens40` has three MPT rows. Generator signal only: the deviation of real MPT 1327 FFSK (+-1.5 to 2 kHz here) is assumed.

## 5. Argos PTT uplink: burst extent and message length

Found by looking for modes with no gap measured yet: Argos decoded 90 % of its bursts only at 13 dB in 5 kHz (23 dB Eb/N0 for a 400 bit/s
message, about 12 dB over what its demodulator needs). The burst finder detects the carrier line at 30 dB, so the loss was after it:
`demodUplinkBurst` (`core/src/argos/argos_rx.cpp`) takes the burst's start and end from the mean power of 1 ms windows of the 32 kHz stream and
gave up unless the burst stood 6 dB over the noise of the whole stream (that is 12 dB in 5 kHz). Changes: (1) the window power is the coherent 1 ms
sum (the carrier has been mixed to 0 Hz, so the sum keeps the signal and takes 15 dB off the noise); (2) the start needs six windows above the gate in
a row and the end the same with each window counting when its neighbour is above (the Manchester part dips where a window straddles a phase step); (3) the
message length is no longer read off the envelope but chosen from the valid A2 lengths: the first length when its bits are mostly +-2.2 rad
steps, then each further 32 bits while their mean closeness stays at 0.70 or more (a block of bits that are not steps averages 0.51 with a spread of 0.1).

| Argos (SNR in 5 kHz, 8 trials, 2 units a trial) | before: 14 / 12 / 10 / 6 dB | after: 4 / 2 / 0 / -2 / -4 / -6 dB | gain |
|---|---|---|---|
| AWGN | 88 / 50 / 0 / 0 % | 100 / 100 / 94 / 63 / 63 / 25 % | about 14 dB |
| +3.1 kHz carrier | 88 / - / 0 / 0 % | 100 / 94 / 94 / 44 / 38 / 19 % | about 14 dB |
| 50 ppm clock | 88 / - / 0 / 0 % | same as AWGN | about 14 dB |

(The plateau of 88 to 100 % is the harness's two bursts a trial and the random start.) At 0 dB in 5 kHz the message has an Eb/N0 of 10 dB, the figure a
coherent differential demodulator of this kind needs: the receiver is now at theory. Noise-only: 0 units before and after (4 runs, plus 3 in the
test). `argos_rx`, `argos_proto`, `argos_sigfox_mode` pass. Test: `argos_sens40`. Simulated signals only: a real PTT's phase-modulation index
(+-1.1 rad assumed), Doppler rate and oscillator noise are not measured. The COSPAS-SARSAT receiver has a similar burst finder but sits at 8 dB
Eb/N0 in the harness (-1 dB 19 %, 2 dB 75 %), so it was not changed.

## Tried, measured, not kept

* **ACARS: coherent AM detection with a carrier memory.** Built it: a phase-locked loop (40 Hz, 150 Hz for the first 8 ms) on the 25 kS/s channel
  signal, started on a remembered carrier frequency (per channel candidate, kept between bursts because the receiver reassigns a decoder to the
  channel for every burst), the frequency from the power spectrum (log-parabola through the strongest line of a 1024-point averaged spectrum, good to
  a few Hz), the phase initialised from the last 64 samples at the carrier's onset, lock detection from the share of power on the real axis. It
  locked for 85 to 90 % of the carrier time in about 70 % of the bursts (the first burst on a channel has no memory, and in some bursts the loop
  stayed 20 Hz off with a weak carrier and never pulled in). Result with 24 trials a point, one aircraft, carrier 0 and 2.5 kHz off: 4 dB 51 %
  -> 57 %, 3 dB 25 % -> 26 %: about 0.2 dB, not the 1.5 dB the 100 Hz case of part 1 suggested. Not kept (more state and a failure mode for a
  gain that small); reverted.
* **POCSAG / FLEX narrower channel filter** (the 25 kHz path has a 10 kHz pass band). POCSAG gets about 3 dB better at 9.5 kHz (-4 dB 64 % against 3 %)
  but FLEX collapses (2 dB 6 % against 77 %: its 4-level 6400 Bd needs the width) and 9 kHz and below break both; a switch by protocol needs a
  second filter path in parallel. Not done.
* **M10 on the narrow path** (cut 7 to 10 kHz): no change in the harness (its score plateaus at 61 to 67 % even at 20 dB with 2.7 units a trial).
* **MRZ on the narrow path:** breaks `sonde_extra_rf` (see part 1). **Meisei:** cannot be measured in the harness (0 % before and after).

## At theory, nothing to gain with the tools tried

Measured with sens_curve in this pass, and read against the Eb/N0 each needs:
* **DMR** (4FSK 4800 sym/s, 12.5 kHz): 6 dB 89 %, 4 dB 41 %, same with +3.1 kHz; 6 dB is about 7 dB Eb/N0 for 4FSK, the usual figure for 5 % BER. At theory.
* **AIS** (GMSK 9600 Bd, 25 kHz): 4 dB 86 %, 2 dB 27 %, same with +3.1 kHz; about 7 dB Eb/N0, below the coherent GMSK figure for a clean packet because
  the receiver combines repeated slots. At theory.
* **APRS / AX.25 1200**: the packet front end already runs a second, narrower channel with its own discriminator (`chanPass2`); the limit is the FM threshold
  (SENSITIVITY-3.6.md, 3.9-B). **P25, NXDN, dPMR, D-STAR, YSF, TETRA:** within 1 dB of the figures in 3.6, 3.7, 3.9-B and 3.9-D; the TETRA
  decision-feedback attempt of 3.9-D lost 1 dB. No change made.
* **FM click suppression, soft-decision RS, MSDD:** see part 1.
* **Survey with sens_curve, AWGN, 4 trials, 3 dB steps, noise in the harness bandwidth** (first point where the fraction falls under 50 %, and Eb/N0 there):
  Sigfox (DBPSK 100 bit/s in 600 Hz) 0 dB, 7.8 dB; Z-Wave (100 kbit/s FSK) 9 dB, 9 dB; FLARM 3 dB, 6 dB; ADS-L 3 dB; BLE (1 Mbit/s GFSK) 9 dB, 9 dB;
  Zigbee 0 dB; Remote ID 9 dB; UAT 978 about 3 dB (plateau 89 %); Wi-SUN FAN 9 dB in 200 kHz (its per-rate matched filter is already
  deviation + 0.35 x bit rate wide); Orbcomm above. Each is within about 3 dB of the non-coherent figure for its modulation and frame length, and the
  receivers already filter to the signal's own width: no gap found that a filter or follow-up would close.

## Not reached in this pass

DSC, NAVTEX, POCSAG/FLEX filter switching, the Iridium and Inmarsat voice modes, GNSS acquisition, ADS-B overlapping replies, LoRa, CCSDS and
the other modes of the brief's "no gap measured yet" list were not measured (Orbcomm was: 8 dB 90 %, 5 dB 10 %, about 10 dB Eb/N0, at theory;
COSPAS-SARSAT above). The Argos result suggests the same kind of fixed acquisition gate may sit in other burst receivers: that is the first place to look.
