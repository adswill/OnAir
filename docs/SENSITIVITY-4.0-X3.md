# Sensitivity 4.0, pass X3 (feat40-sensX3): realistic channels

Branch feat40-sensX3 on integ-40. Every figure here comes from simulated signals: each mode's own generator, complex white noise and the
sens_sweep channel chain (static echo, COST 207 TU6 Rayleigh / Rician K = 10 dB with Jakes Doppler, Wiener phase noise, carrier and
clock error, tools/sens/sens_impair.h). No off-air recording was used. Every before/after pair was run in one binary on one GitHub runner
with the same noise seeds. The 3.9 path of the changed receiver stays in the code behind an A/B switch (environment variable) for this.

New tests:

* `test_sens40x3` (ctest `sens40x3`, tests/sens): runs the sens_sweep scenarios through a fresh receiver per trial. A trial passes when it
  counts at least half the units of a clean run (as sens_sweep does). It prints pass rate, mean unit fraction and process CPU for the new
  path and the 3.9 path. With no environment it is a guard: ISDB-T TU6 at 26 dB and AWGN at 20 dB must pass, and noise alone must give no
  packet. `ONAIR_SENS_CALIBRATE=1` gives the table (`X3_MODES`, `X3_SCEN`, `X3_TRIALS`, `X3_SNR=hi:lo:step`, `X3_SECS`).
* `test_cubesat_narrow` (ctest `cubesat_narrow`, tests/cubesat): runs each CubeSat framing at a series of C/N values, with the carrier still,
  held at +-1.4 kHz (just under the AFC threshold, so never corrected), or drifting from +6 to -6 kHz over the run. It reports frames decoded
  against frames sent, wrong frames, and frames out of noise alone, for the 4.0 and the 3.9 receiver. With no environment it is a guard;
  `ONAIR_SENS_CALIBRATE=1` gives the table.

## 1. ISDB-T: channel estimation in time (the TU6 gap)

**Harness check first.** C1 suspected that the harness ran without time interpolation (I = 0). It does not: the synth
(core/src/engine/modes.cpp `IsdbtSynth`) sends layer B (12 segments, 64-QAM 3/4) with time-interleaving code 1, which is I = 1 in mode 3.
Layer A is QPSK 2/3 with code 2. So the measurement was right, and the gap was in the receiver.

**Cause.** `isdbt_demod.cpp` equalised every symbol with the newest scattered pilot of each grid point (every third carrier). That pilot is
0 to 3 symbols old (up to 3.4 ms in mode 3). At 10 Hz Doppler the hold error averages about -20 dB against the channel. That is an error
floor just under what 64-QAM 3/4 needs in fading: the 3.9 receiver had no 90 % point at 30 dB in TU6 (sweep table: `-`). The noise
estimate (half the power of the pilot change over four symbols) also counted the Doppler as noise. There was no averaging over time and
no delay window, so the estimate carried the full noise of one pilot. The DVB-T receiver already does all three.

**Change** (the DVB-T scheme, `dvbt_rx.cpp`, ported):

* Symbols go to the equaliser 8 symbols late. The channel of each grid point is a cubic through the two time-averaged estimates before
  the symbol and the two after it. The pilot above the band, which appears in every symbol, is the mean of the symbols m-1 to m+1.
* Pilot statistics: E|p - p(-4)|^2 = 2r + q and E|p - p(-8)|^2 = 2r + 4q. Here r is the pilot noise and q is the channel's change between
  two pilots of a grid point. The noise estimate is now r. The time-averaging gain is the one with the least noise-plus-lag error, at least
  1/10.
* The delay window is measured on the full-window estimate every 16 symbols (`delaySpan`, as DVB-T). After that the frequency
  interpolator passes only the measured span plus margin.
* The tracking (common phase and timing ramp) still compares each symbol with the raw pilot four symbols back, as before. `corrected()`
  (used for the TMCC) is still the current symbol.
* `DECT2_ISDBT_NOTI=1` restores the 3.9 path.

**Result** (`sens40x3`, 8 trials a point, 1 dB steps, 2 s trials; pass = at least half the clean units; GitHub run 43149217706; the TU6
3.9 figure is from the 2 dB run 43145529175, 6 trials, because the 1 dB run started at 25 dB):

| scenario | 3.9: 90 % point | 4.0: 90 % point | gain |
|---|---|---|---|
| AWGN | 18 dB (mean unit fraction 76 %) | 17 dB (100 %) | 1 dB |
| echo -6 dB at 10 us | 20 dB | 18 dB | 2 dB |
| TU6, 10 Hz Doppler | 30 dB (28 dB: 83 %; 25 dB: 0 % of 8) | 21 dB (20 dB: 88 %) | **9 dB** |
| Rician K = 10 dB | 19 dB | 18 dB (17 dB: 62 %) | 1 dB |
| phase noise (1 Hz, TV tuner) | 19 dB | 17 dB | 2 dB |
| combined (2 ppm + 20 ppm clock + echo + phase noise) | 20 dB | 18 dB | 2 dB |
| noise only, 16 runs | 0 packets | 0 packets | |

The TU6 penalty against AWGN drops from about 12 dB to 4 dB; DVB-T (no time interleaving) has about 7 dB in the same channel. CPU: +3 %
(process CPU of the same trials, for example TU6 23.3 s against 22.8 s for 8 trials). Latency: 8 symbols (9 ms) more.

The other ISDB-T tests (`isdbt_chain isdbt_rx isdbt_engine isdbt_bw isdbt_basic isdbt_impair isdbtsb ofdm_csi_isdbt`) pass.

## 2. CubeSat narrow paths (AX100 Mode 6, U482C, NGHam and the other 9600 / 4800 baud framings)

QA-4.0.md: one FM channel of +-12 kHz pass / 20 kHz stop serves every framing. A narrower shared filter gained 0.3 to 1 dB, but it was not
applied because the Doppler margin had not been measured.

**Change.** The wide paths are kept. Narrow paths run beside them (`cubesat_rx.cpp`):

* 9600 baud FSK / GMSK (G3RUH HDLC, AX100 Mode 6, every sync-word framing) and the U482C audio FSK run on a new optional third channel of
  the packet FM front: +-8.5 kHz pass, 13.5 kHz stop (`FmFront::chanPass3`, off by default, so every other receiver that uses the front is
  unchanged).
* 4800 baud runs on the existing +-4.5 kHz Bell 202 channel (`chanPass2`).
* The margin: the 9600 signal spans about +-7.8 kHz (Carson, 3 kHz deviation), the 4800 one about +-3.9 kHz. The AFC mixes out any error
  above 1.5 kHz, so the residual the narrow filters must pass is at most +-1.5 kHz.
* A frame decoded by both paths is reported once (the 50 ms duplicate check). Failures of the narrow paths are not counted a second time.
* `DECT2_CUBESAT_NONARROW=1` restores the 3.9 receiver.

**Result** (`cubesat_narrow`, GitHub macOS run 43146216842: 4 runs of 12 cycles of each framing per point, C/N in 25 kHz with the generator's
own noise; the 50 % point read between the 1 dB steps; same signals for both receivers):

| framing (baud) | 3.9 50 % point | 4.0 50 % point | gain | with +-1.4 kHz / drift +6 to -6 kHz |
|---|---|---|---|---|
| NGHam (4800 FSK) | 7.4 dB | 5.3 dB | **2.1 dB** | 2 dB in all three (at 6 dB: 96 to 100 % against 0 %) |
| Mobitex / BEESAT (4800 GMSK) | 8.1 dB | 6.0 dB | **2.1 dB** | same |
| AX100 Mode 5 at 4800 (half the Mode 5 frames) | 6.7 dB | 4.5 dB | **2.2 dB** | same |
| USP (9600 FSK) | 5.6 dB | 5.0 dB | 0.6 dB | 0.6 to 1 dB |
| CCSDS concatenated (9600 GMSK) | 5.9 dB | 5.0 dB | 0.9 dB | 0.9 dB, drift 0.9 dB |
| CCSDS RS 255/223 (9600 GMSK) | 8.4 dB | 8.0 dB | 0.4 dB | 0.4 to 0.6 dB |
| AX100 Mode 6 (9600 G3RUH) | 9.2 dB | 9.0 dB | 0.2 dB | 0.2 to 0.5 dB |
| AX.25 G3RUH (9600) | 10.1 dB | 10.0 dB | 0.1 to 0.3 dB | |
| U482C (4800 audio FSK on 3600 Hz) | 10.1 dB | 9.8 dB | 0.3 dB | **0 % in both at 14 dB** with a static 1.4 kHz error or the drift: see below |

CPU of the CubeSat receiver: +10 to +24 % (narrow 9600 and U482C paths, the third channel filter); +36 % in one cell, at the strongest signal.

The 4800 baud gain is the largest: the 4800 slicers used to sit on the +-12 kHz channel, and above the FM threshold the IF width does not
matter, but at these C/N values it does (clicks). The 9600 framings gain less because their signal already fills most of the channel.

**U482C with a carrier error.** The run also showed a 3.9 fault. U482C failed completely (0 % at 14 dB) whenever the carrier was off by
less than 1.5 kHz, which the AFC leaves alone, or drifting. Cause: the error is a DC level on the discriminator, and the audio mixer turns
it into a tone at -3600 Hz inside the low-pass filter's transition band, as strong as the FSK itself. Fix: a 20 ms DC block before the
mixer (`AudioFsk::dcBlock`). It is on with the 4.0 paths, for U482C and for every tracked satellite's audio-FSK path. The numbers after the
fix: section 2.1.

**False decodes.** Noise alone, 4 runs of 219 s: 3.9 gave 2 frames and the first 4.0 build gave 5. AX100 Mode 5 without Reed-Solomon has
no check at all (only the Golay length field), so a second hunter roughly doubles what that framing finds in noise. The narrow paths now take
Mode 5 frames only with Reed-Solomon. The `wrong` column counts frames whose bytes match no frame that was sent. It is non-zero in 3.9
too: these are Mobitex frames with damaged blocks (reported with the blocks marked) and frames cut by the start or end of a run, not frames
out of noise.

### 2.1 After the fixes

(run 43151674213; filled in below)

## 0. The scenario sweep before any change

`sens_sweep --full`, 6 trials a point, 1 dB steps, scenarios awgn/echo/tu6/rician/phasenoise/combined, branch base ead745f20 (GitHub run
43143235835). Threshold = lowest SNR with 90 % of trials passing; `-` = no 90 % point even at the top SNR. Penalty = scenario minus AWGN.

| mode | awgn | echo | tu6 | rician | phasenoise | combined | largest penalty |
|---|---|---|---|---|---|---|---|
| DVB-T2 | 14 | 17 | 20 | 16 | 15 | 18 | TU6 +6 |
| DVB-T | 15 | 16 | 22 | 16 | 15 | 16 | TU6 +7 |
| ATSC 1.0 | 15 | 17 | - | 18 | 15 | 17 | TU6 (no point) |
| DAB | 3 | 3 | 5 | 4 | 5 | 5 | +2 |
| ISDB-T | 18 | 20 | 30 | 19 | 19 | 20 | **TU6 +12** (fixed, section 1) |
| FM audio | 5 | 3 | - | 12 | 6 | 3 | TU6 (flat Rayleigh at 200 kHz: deep fades of the whole channel) |
| FM RDS | 16 | 18 | 26 | 17 | 15 | 14 | TU6 +10 (as above) |
| DTMB | 18 | 20 | 22 | 19 | 19 | 19 | TU6 +4 |
| DRM mode B | 16 | 20 | 24 | 17 | - | - | phase noise: no point (section 4) |
| ATSC 3.0 QPSK 8/15 | 2 | -1 (echo at 0 us, see section 3) | 18 | 3 | 3 | 1 | **TU6 +16** (section 3) |

## Not done / not changed

* **ATSC 1.0** (TU6 without a point, Rician +3). The field equaliser is solved once per 24 ms field, so it cannot follow a 10 Hz Doppler
  channel. A per-quarter-field decision feedback retry was written (commit d3b51c8ec) and reverted untested (38f8ef1cf): branch
  feat40-atsc adds an LMS equaliser that tracks the channel inside the field, plus own-symbol retraining, in the same function of
  atsc_rx.cpp. The two would conflict, so ATSC 1.0 stays with that branch.
* **DRM**: phase noise and combined have no 90 % point. The harness uses a 1 Hz Wiener linewidth. On 26.7 ms symbols that is about 0.4 rad of
  random-walk phase per symbol, far worse than an HF receiver's oscillator. A common-phase loop would have to share the three frequency
  reference tones with the frequency loop. Not changed. The TU6 row (+8) is flat Rayleigh fading over 10 kHz with 2 Hz Doppler, so it is
  mostly physics; the receiver already has a Wiener filter in time.
* **DVB-T TU6 +7, DVB-T2 TU6 +6, DTMB TU6 +4.** These receivers already have time interpolation, delay-window narrowing and per-carrier
  noise (DVB-T, ISDB-T now as well) or adaptive segmented time fits (DVB-T2). DVB-T has no time interleaver, so its TU6 penalty includes the
  diversity it lacks. No receiver gap was found by reading the code, and none was measured.
* **FM in TU6**: flat Rayleigh fading over the whole 200 kHz channel; physics, not a receiver gap.
* **DAB phase noise / combined +2**: DAB already corrects the common phase error (fourth power of the DQPSK cells, dab_rx.cpp); the rest is
  the inter-carrier interference of a 50 Hz linewidth on 1 kHz spacing.
* **ATSC 3.0 with the convolutional time interleaver**: the channel-state weights are not carried through the CTI de-interleaver
  (atsc3_receiver.cpp runCti); that path keeps one noise figure per block.
