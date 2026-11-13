# Sensitivity 4.1, TV and DRM pass (feat41-sensTV)

Branch feat41-sensTV on integ-41. Every figure comes from simulated signals: each mode's own generator, complex white noise and the
sens_sweep channel chain (tools/sens/sens_impair.h: echo, COST 207 TU6 Rayleigh / Rician K = 10 dB with Jakes Doppler, Wiener phase
noise, carrier error, windowed-sinc clock error). No recording and no radio was used. Each before / after pair ran in one binary on one
GitHub runner with the same noise seeds; the 4.0 path of a changed receiver stays behind an A/B switch.

New tests:

* `test_sens41tv` (ctest `sens41tv`, tests/sens): seeded trials through the channel chain into a fresh receiver, the 4.1 and the 4.0
  path side by side, with the lock time of every trial (seconds of signal to the first valid unit, polled every 50 ms). A trial passes at
  half the units of a clean run (as sens_sweep). Without environment it is a guard: noise alone gives no unit in ATSC, DRM and ISDB-T,
  and DRM 64-QAM passes at 26 dB through 1 Hz phase noise (4.0: no trial). `ONAIR_SENS_CALIBRATE=1` gives the tables (`S41_MODES`,
  `S41_SCEN`, `S41_TRIALS`, `S41_SNR` / `S41_SNR_<id>`, `S41_SECS`, `S41_FD` / `S41_FD_<id>`, `S41_PN`, `S41_ONE=<snr>` one diagnosis
  trial, `S41_ALTS="A=1,B=2/C=3"` the same in child processes with other receiver settings).
* `test_atsc3_cticsi` (ctest `atsc3_cticsi`, tests/atsc3): FEC blocks through the convolutional time interleaver (512 rows), a Rayleigh
  channel in transmit order and noise, into `Atsc3Receiver::commitFrame`, with and without the per-cell weights.

## Summary

| item | result |
|---|---|
| 1. 50 ppm clock penalty, DVB-T / DVB-T2 / DTMB / ATSC | **harness artefact, no receiver loss.** 50 ppm = AWGN in all four with the trial lengths of sens_sweep: ATSC 15 / 15, DVB-T 15 / 15, DVB-T2 14 / 14, DTMB 18 / 18 dB, same lock time. Baseline rows refreshed. No receiver changed. |
| 2a. ATSC 1.0 in TU6 | no gain. Cause found (segment sync lost in every fade, real-valued equaliser after a pilot PLL); two attempts measured and reverted. |
| 2b. ATSC 3.0 CTI path | per-cell channel state through the convolutional time interleaver: about **1 to 2 dB** in a Rayleigh channel (7 dB: 63 of 63 blocks against 0; 8 dB: 63 against 50) |
| 3. DRM under phase noise | common-phase-error correction, **opt-in (`DECT2_DRM_CPE=1`)**: 1 Hz linewidth, 64-QAM no 90 % point at 28 dB -> 24 dB; 16-QAM 12 -> 10 dB, combined 18 -> 12 dB; 0.1 Hz: 64-QAM 18 -> 16 dB. Off by default because `drm_smooth` fails with it (mode E, ITU channels 5 and 6) |
| 4. ISDB-T at 70 Hz Doppler | first-order ICI removal written, measured, and **not kept**: the limit at 50 to 70 Hz is the time interpolation of the scattered pilots, not ICI |

False decodes: every row above was run noise-only next to it (16 runs per mode and path): 0 runs with a unit in all of them.

## 1. The 50 ppm clock penalty

The 3.6 baseline (tests/sens/baseline/sens-baseline.json, measured 2026-10-23) listed a 50 ppm sample clock as costing ATSC +9 dB, DVB-T
+4 dB, DTMB +4 dB and DVB-T2 +3 dB. Those rows predate the windowed-sinc clock error of the harness (2026-10-30, sens_impair.h
`clockError`): the 4-point cubic it replaced was a time-varying filter on the wide TV signals (ATSC 5.4 MHz at 8 Msps). 4.0 C1 then
showed with 6 s trials that the receivers follow the clock; here the question was whether anything is left with the short trials.

`sens41tv` (8 trials a point, 1 dB steps, trial lengths of sens_sweep, quick run 43363616797) and `sens_sweep --full` itself
(12 trials, run 43370655669) on the unchanged receivers:

| mode | AWGN 90 % | +50 ppm | -50 ppm | lock time (median) |
|---|---|---|---|---|
| ATSC 1.0 | 15 dB | 15 dB | 15 dB | 0.17 s in all three |
| DVB-T 8K 64-QAM 2/3 | 15 dB | 15 dB | 15 dB | 0.15 to 0.20 s |
| DVB-T2 8K 64-QAM 2/3 | 14 dB | 14 dB | 14 dB | 0.20 to 0.25 s |
| DTMB 64-QAM 0.6 | 18 dB | 18 dB | 18 dB | 0.26 s |

The penalty was the harness. No receiver was changed. The baseline's awgn and sro50ppm rows of these four modes now hold the new
sens_sweep figures (the other scenarios are unchanged); DTMB's AWGN row reads 18 dB instead of 16 dB, which is its lock-time plateau in
2 s trials (4.0 C1: 73 to 90 % of units at 16 dB with 6 s trials), not a change of the receiver.

## 2. ATSC

### 2.1 ATSC 3.0: channel state through the convolutional time interleaver

4.0 gave the data, L1 and hybrid-interleaver paths a weight per cell (|h|^2 / mean |h|^2); the CTI path (atsc3_receiver.cpp `runCti`)
kept one noise figure per block. Now `decodeFrame` hands the cells' weights with the CTI cells (`PlpResult::cellW`), `runCti` runs them
through a second de-interleaver of the same shape (`CtiPlp::deintW`, weights as real parts), cuts them with the blocks and passes them to
`Bicm::decodeBatch`. Cells the equaliser blew up and the start-up cells (the de-interleaver's zeros) get weight 0. `DECT2_ATSC3_NOCSI=1`
gives the 4.0 path (no weights anywhere).

The engine's ATSC 3.0 test signal has no CTI (buildFrame always signals time-interleaving mode 0), so this path is not in the sweep.
`atsc3_cticsi` drives it directly: QPSK 8/15 16K blocks, 4 per frame, 24 frames, CTI of 512 rows, Rayleigh gain held for 400 cells,
the receiver given y / h and |h|^2 (run 43372918479):

| SNR | blocks decoded after the start-up, with weights | without (4.0) |
|---|---|---|
| 9 dB and up | 63 of 63 | 63 of 63 |
| 8 dB | 63 | 50 |
| 7 dB | 63 | 0 |
| 6 dB and below | 0 | 0 |

(The 33 blocks of the start-up "decode" in both paths because their cells are the de-interleaver's zeros; they are left out above.)
About 1 to 2 dB in this channel. Clean signal: every block on both paths. Not checked against a real CTI broadcast.

### 2.2 ATSC 1.0 in TU6: not improved

The fading pass was done now that feat40-atsc (LMS tracking inside the field) is merged. TU6 (Rayleigh, no direct path, 10 Hz) still
decodes nothing at 30 dB; Rician K = 10 dB reaches 18 dB. Measured (macOS run 43364107790, 3 s trials, child processes per setting):

* the LMS tracking step (`ATSC_TRACKMU` 0.01, 0.04 default, 0.12) and tracking off: Rician 18 dB in all, TU6 nothing in all;
* a wider pilot loop (`ATSC_PA` 0.008, `ATSC_PAN` 0.0035): worse everywhere (the clean reference itself lost lock).

A single-trial diagnosis (`S41_ONE=26`, `ATSC_DBG=1`, run 43376738965) shows the cause: in TU6 the segment sync is lost every 20 to
100 ms (the pilot and the sync pattern fade by 20 dB and more), and each loss restarts the symbol clock, the field sync and the
equaliser. Holding the clock through up to 31 ms of weak segments once a field sync was known was tried (run 43378744111): TU6 still
nothing, and the echo and Rician rows lost a little (Rician 18 dB: 75 % of trials against 100 %). Reverted. What remains is structural:
the receiver takes the real part after a pilot-only carrier loop and equalises it with a real filter, so a channel without a direct path
that turns the carrier phase faster than the pilot loop can follow leaves no field to decode. A decision-directed complex equaliser and
carrier loop would be the fix; that is a rework of atsc_rx.cpp, not done here.

## 3. DRM: common phase error

**Harness linewidth.** The phase-noise rows use a Wiener (Lorentzian) linewidth of 1 Hz for DRM: L(f) = 1 / (pi f^2), -45 dBc/Hz at
100 Hz and -65 dBc/Hz at 1 kHz, 0.41 rad rms of phase walk per 26.7 ms mode B symbol. An HF SDR's oscillator is about -90 to -110 dBc/Hz
at 1 kHz (an equivalent linewidth of 0.003 to 0.03 Hz). So 1 Hz is a stress case 25 dB or more worse than a receiver's oscillator; 0.1 Hz
(-75 dBc/Hz at 1 kHz, 0.13 rad per symbol) is a poor one. (The comment on the TV modes' 1 Hz says -75 dBc/Hz at 1 kHz; it is -65.)
The harness was not changed; the 0.1 Hz rows are measured below as well.

**Change** (drm_rx.cpp `commonPhase`). The common phase of each symbol is measured from its gain references against those of the symbol
gy earlier (the same carriers, already turned back) and from the three frequency reference tones against the symbol before; it holds
through a fade. The raw pilot estimates turned back by it (Hc) are what the time interpolation (Wiener or linear) combines, and the
symbol's own phase is put back on its interpolated channel. Both the raw and the corrected estimates feed noise and Doppler statistics;
the corrected path is used only while the corrected estimates are still and the raw ones move (corrected Doppler under 0.15 Hz, raw over
1 Hz; left over 0.3 Hz): oscillator phase noise. In a fading channel the common phase is the channel's own and the receiver stays as in
4.0 (on the corrected estimates, 64-QAM in TU6 lost 4 dB in an earlier version: runs 43363930336, 43369202392).

**Result** (8 s trials, 8 trials a point, 2 dB steps; 90 % points; `-` no point at the top of the sweep):

Run 43387245360 (Linux, final code). 4.0 / 4.1, 90 % point (lowest SNR from the top with 90 % of the trials passing):

| scenario | 64-QAM (mode B, 10 kHz) | 16-QAM protection 0 |
|---|---|---|
| AWGN | 16 / 16 dB | 10 / 10 dB |
| echo -6 dB at 1 ms | 18 / 18 | 10 / 10 |
| TU6 (flat Rayleigh, 2 Hz) | 22 / 22 (at 20 dB: 88 % / 25 % of the trials) | 16 / 16 |
| Rician K = 10 dB | 18 / 18 | 10 / 10 |
| phase noise 1 Hz | - (1 % of the units at 28 dB) / **24** | 12 / **10** |
| combined (2 ppm carrier, 20 ppm clock, echo, 1 Hz) | - / - (0 % / 40 % of the units at 28 dB) | 18 / **12** |
| phase noise 0.1 Hz | 18 / **16** | 10 / 10 |
| combined with 0.1 Hz | 20 / **18** | 12 / **10** |
| noise only (16 runs each) | 0 / 0 runs with a unit | 0 / 0 |

The one cost is below the TU6 threshold of 64-QAM: at 20 dB 2 of 8 trials pass against 7 of 8 (the 90 % point is the same). Process
CPU was not measured separately; the correction is a sum over the pilots and one rotation per carrier and symbol. The other DRM tests
pass with it on (`drm_chain`, `drm_engine`, `drm_modee`, `drm_known`, `drm_find`, `drm_impair`, `drm_mrr`, `drm_rx`), but
**`drm_smooth` does not** (run 43398851923): mode E 16-QAM in AWGN at 12 dB (35 s a minute made up against at most 0.1), the 0.5 s hole
case of mode E, ITU channel 5 at 20 and 26 dB, channel 6 at 20 dB and 2 Hz Rayleigh at 22 dB; with it off the same run passes
(43398844895). So the correction is **off by default** and switched on with `DECT2_DRM_CPE=1` (`DECT2_DRM_NOCPE=1` still forces it off);
`sens41tv` sets it. What is left to make it the default: no correction for mode E (13 pilots a symbol, no frequency references), and a
check of what the Doppler-spread ITU channels lose (the common phase there is not common: each path has its own Doppler).

An AddressSanitizer run found a read outside the symbol ring for symbol indices before the first (the Wiener taps reach 2 gy back);
`recIf` now returns nothing for them. That fix is on by default.

## 4. ISDB-T at 70 Hz Doppler

`sens41tv` with `S41_FD_isdbt` (2 s trials, 8 a point): TU6 at 30 Hz reaches 90 % at 24 dB; at 50 and 70 Hz nothing decodes up to 34 dB
(1 % of the units) in either path; Rician K = 10 dB at 70 Hz 22 dB (as 4.0 X3).

A first-order ICI canceller was written (slope of each carrier's channel from the time-interpolated channel of the symbols before and
after, first hard decisions of every carrier, the leakage of 12 neighbours each side taken off), switched on when the measured channel
speed puts the leakage above a tenth of the noise. Its A/B rows were identical to 4.0 at 10, 30, 50 and 70 Hz (runs 43369202392,
43372918479, 43376738965) and the diagnosis at 30 Hz showed why: the channel's change between two pilots of a grid point (4 symbols,
4.5 ms) is three times the noise estimate and the equalised cells read -9 to -17 dB EVM at 30 dB SNR. The error of the time
interpolation of the scattered pilots (one pilot per carrier every 4 symbols) is the limit, about -12 dB, well above the -21 dB of the
ICI at 70 Hz. Removing the ICI cannot help until that estimate is better (a Doppler-matched Wiener filter over more pilots in time, or
a decision-directed estimate), so the canceller was not kept.

## Not done

* ATSC 1.0 in TU6: needs a complex decision-directed equaliser and carrier loop (section 2.2).
* ISDB-T above 30 Hz Doppler in TU6: needs a better time-direction channel estimate first (section 4).
* ATSC 3.0 CTI on a real broadcast or an end-to-end CTI test signal (the synthetic transmitter has no CTI).
