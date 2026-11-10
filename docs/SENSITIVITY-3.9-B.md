# Sensitivity 3.9, part B (feat39-sensB): ham, voice, pagers, HF utility

Branch feat39-sensB on integ-39. Every figure here is from simulated signals: generator, white Gaussian noise (`PortableNormal`), and the
impairments named. No real recording was used. Threshold = the lowest SNR at which at least 90 % of the units decode. Measured on the Mac
(Apple M-series, -O2, one thread, other workers loading the machine), so the CPU figures are rough.

## Results

| mode | unit | noise bandwidth | threshold before -> after | CPU before -> after (s per s of signal) | change |
|---|---|---|---|---|---|
| NAVTEX / SITOR-B, short messages (70 characters), FSK slicer + decoder at 24 kHz | message with every character right | 300 Hz | 6 -> 3 dB | 0.0016 -> 0.0016 | soft DX/RX decision, phasing lock |
| NAVTEX, long messages (420 characters), same chain | same | 300 Hz | 7 -> 4 dB | 0.0016 -> 0.0016 | same |
| NAVTEX, whole MarineReceiver at 2 Msps, long messages | same | 300 Hz | 7 -> 4 dB (3 dB: 50 %) | 0.013 to 0.03 both (machine load; the code path did not change cost) | the above plus the front-end clip |
| NAVTEX, +250 Hz carrier offset | same | 300 Hz | 6 -> 4 dB | - | |
| NAVTEX, -400 Hz carrier offset | same | 300 Hz | 6 -> 4 dB | - | |
| NAVTEX, drift 60 Hz/min | same | 300 Hz | 6 -> 4 to 5 dB (4 dB: 83 %) | - | |
| BPSK31, whole PskReceiver at 250 kHz | word of the text, in order | baud (31.25 Hz), so SNR = Eb/N0 | 10 -> 9 dB | 0.0065 -> 0.0060 | decision against an averaged phase reference |
| BPSK31, 20 Hz error drifting 30 Hz/min, 9 dB | same | same | 75 -> 89 % of the words | - | same |

Per point: NAVTEX 10 trials of 4 messages (40 messages) at base rate, 3 trials of 4 at 2 Msps; BPSK31 8 trials of 33 words.

Before -> after per point, NAVTEX long messages, base rate: 6 dB 88 -> 100 %, 5 dB 75 -> 100 %, 4 dB 29 -> 92 %, 3 dB 0 -> 55 %.
BPSK31: 10 dB 95.1 -> 95.5 %, 9 dB 86.7 -> 95.5 %, 8 dB 65.9 -> 74.2 %, 7 dB 40.5 -> 45.8 %.

Noise only, NAVTEX: 3 runs of about 10 minutes each, before and after: 0 characters and 0 messages (the easier lock makes no false text).

## What changed

1. **NAVTEX soft decision over both copies** (`core/src/marine/marine_navtex.cpp`, `NavtexDecoder::combine`). Each character goes out twice (DX and
   RX). The decoder used to take a copy as soon as it had four marks, even when the other copy disagreed strongly. A copy with two swapped bits
   still has four marks. Now the soft values of both copies are added and the closest of the 35 code words with four marks wins (maximum
   likelihood with time diversity). Used by NAVTEX in the marine mode and by the SITOR-B mode (`sitor_live.cpp`), which share the decoder.
2. **NAVTEX phasing lock** (`phaseGood`). The decoder needed 16 good words in a row before it locked. Below about 5 dB that comes late, after the
   ZCZC of the message, so the whole message was lost. It now also locks when 14 of the last 16 words are good and 12 of them are the phasing signals
   (alpha, beta, repeat). The old rule stays. In the noise-only runs there were no false locks.
3. **Front-end clip** (`core/src/marine/marine_dsp.cpp`, `marine::Front`). This is a bug fix. The fixed-point CIC clipped its input at 8 units.
   That clipped float recordings with a larger scale, and wide captures whose noise alone is several units: at 2 Msps the noise is 6667 times the
   power in a 300 Hz channel. The limit now follows the int64 headroom of the CIC (dec^4 x 2^20 per unit, half the range kept free, at most 4096).
   `llrintf` replaces `lrintf`, because long is 32 bits on Windows. Inputs under 8 units give exactly the old samples.
   `marine::Front` is shared, so the fix reaches every receiver that uses it: marine (NAVTEX, DSC, fax), SITOR, PSK, hfdig (RTTY and keyboard
   modes), PACTOR-II..IV, ham science, ATIS, QO-100, ALE. Real radios deliver |x| <= 1.4, so live reception is unchanged. The sens_sweep harness
   (signal at 0.2 rms, 2 Msps) fed these modes clipped noise, so its old figures for them were pessimistic. That explains the 3 to 4 dB NAVTEX gap
   left open in SENSITIVITY-3.6.md: the measured chain is at about 4 dB now, against the 5 dB noted there as about theory for this criterion.
4. **BPSK31/63 decision** (`core/src/psk/psk_rx.cpp`, `symbol`). Each symbol is now decided against a carrier reference: the decision-directed
   average of the last symbols (weight 0.25 for the new symbol). The receiver used to compare it with the previous symbol alone. A reversal is still a change
   of decision, so a slip of the reference costs one bit. QPSK is unchanged (its Viterbi path). `PSK_NOCOH=1` turns the change off for measuring.

## Tests

* `marine_sens39` (tests/marine/test_marine_sens39.cpp, about 4 s). It checks 4 dB short messages, 5 dB with +250 Hz, 5 dB with -150 Hz drifting
  60 Hz/min, noise only, and the whole receiver at 250 kHz with the input scaled by 4. The old code decoded nothing in that last case. With
  ONAIR_TEST_LONG=1 it runs the full sweep, 8 to 2 dB for both message lengths.
* `psk_sens39` (tests/psk/test_psk_sens39.cpp, about 3 s). It checks 9 dB plain, and 9 dB with 20 Hz drifting 30 Hz/min. ONAIR_TEST_LONG=1 runs the sweep, 10 to 6 dB.
* Benches: `tests/marine/navtex_bench.h`, `tests/psk/psk_bench.h`, and the driver `tools/sens/sens39b_bench.cpp`, which is not built by CMake.
  The driver takes the mode, the SNR range, trials, offset and drift. The env variable LINES sets the message length and RATE runs the whole
  receiver at that input rate.
* Run locally against the unchanged prebuilt core: marine_sens39, psk_sens39, psk_rx, psk_wide, marine_ccir476, marine_rf_navtex,
  marine_slow_navtex_snr, marine_dsc_bands and sitor_live pass. marine_basic needs the engine, so it was left to GitHub.

## Looked at, not changed

* **BPSK31 remaining gap.** Noncoherent DBPSK theory for this word criterion is about 7 to 8 dB. The receiver needs 9, and part of that is
  the PSK31 pulse shape (reversals carry less energy). The squelch (`PSK_NOSQL`) and the ISI removal were checked: turning either off made it worse or did not change it.
* **POCSAG, DMR, P25, D-STAR, NXDN, dPMR, HFDL, RTTY, FT8/FT4.** Earlier passes put these within about 1 dB of theory (SENSITIVITY-3.6.md, 3.7.md;
  FT8 has OSD and a-priori passes, tests/hfdig/test_hfdig_ftxsens.cpp). No cheap gain, so no change.
* **APRS 1200.** It sits at the FM discriminator threshold, as SENSITIVITY-3.6.md found.
* **CW.** The beam decoder is about 8 dB over the theory for a perfect clock, which is the usual margin for machine copy. A real gain needs new
  timing work, so it was not done in this pass.
* **Not measured in this pass.** Olivia, MFSK, THOR, Domino, SSTV, HamDRM, the FreeDV modem, M17 (5 dB, already in the harness), Fusion, TETRA,
  TETRAPOL, EDACS, SmartNet, MPT1327, FLEX, ALE, STANAG 4285, MIL-STD-188-110, ARQ and weather fax. STANAG and 188-110 are on feat39-hfmil.
* `core/src/goesdcs/goesdcs_live.cpp` has the same clip at 8 units. It is not in this branch's decoder list, so it was left alone and is noted for its owner.

## Merge notes

feat39-misc also edits `PskReceiver::Impl::symbol` (8PSK). The hunks are next to each other: keep both. This branch's change is only the BPSK
`else` branch.
