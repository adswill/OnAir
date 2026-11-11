# ATSC 1.0 and ATSC 3.0 CPU (3.9, feat39-atscperf)

Goal: ATSC 1.0 and ATSC 3.0 under one CPU core in total at full real-time rate, each alone and both together, with decode quality unchanged.

## How it was measured

- The tool is `bench_dvb --case atsc,atsc-10m,atsc3,atsc3-256qam --seconds 6` (Release build).
- The signals are the app's synthetic test signals at the radio rates. The figures are CPU seconds per second of signal, for all threads, per stage. The stage accounting comes from `dect2/common/stage_prof.h`, now also in the ATSC receivers.
- The Mac was an Apple M2 Pro running under `nice -n 15` alongside other builds (load 20-30). Compare the columns with each other only: an idle performance core needs less.
- "After" is the lower of two runs.
- `atsc3-256qam` (new case) asks the generator for 256QAM 10/15 64K. The ATSC 3.0 test signal decodes the same packet count either way, so treat it as a second ATSC 3.0 run. ATSC 3.0 transport-stream digests vary from run to run even with unchanged code (the generator is not deterministic there). ATSC 1.0 digests are stable.

## Totals

| Case | Before | After | Change |
|---|---|---|---|
| ATSC 1.0, radio at 8 Msps | 0.681 | 0.556 | -18 % |
| ATSC 1.0, radio at 10 Msps | 0.830 | 0.579 | -30 % |
| ATSC 3.0, radio at 10 Msps | 0.335 | 0.219 | -35 % |
| ATSC 3.0 "256QAM" case, 10 Msps | 0.398 | 0.266 | -33 % |
| ATSC 1.0 8 Msps + ATSC 3.0 (sum) | 1.016 | 0.775 | |
| ATSC 1.0 10 Msps + ATSC 3.0 256QAM case (sum) | 1.228 | 0.845 | |
| test_atsc_cpu, both receivers running at once | 0.972 | 0.826 | |

The feed thread (the one that must keep up with the radio) for ATSC 1.0 at 10 Msps went from 0.351 to 0.236.

## Stages, ATSC 1.0 at 10 Msps (8 Msps in brackets)

| Stage | Before | After | What changed |
|---|---|---|---|
| atsc.filter (matched filter + resampler to 2 samples/symbol) | 0.246 (0.180) | 0.161 (0.150) | Four outputs share one window of input: their taps sit at their offsets in zero-padded rows, so each data vector is loaded once for four outputs. This halves the loads (the loop was load-bound) for 8 % more multiply-adds. Written with GNU vector types: NEON on ARM, the AVX2 body on x86. ATSC_NOPAD=1 restores the old loop. |
| equaliser (atsc.eq.filter before; now eq.dfe + eq.ff + eq.filter) | 0.184 (0.162) | 0.127 (0.124) | The decision feedback (128 taps) still runs in transposed form, but adds four decisions per pass over its accumulators. Inside a group of four the later outputs get the earlier decisions' terms directly. A quarter of the accumulator stores, and the same decisions (identical ATSC 1.0 digest). A block direct form was tried and was slower (0.21 against 0.12): its chain inside the block is latency-bound. |
| atsc.pll | 0.085 (0.075) | 0.071 (0.072) | unchanged (noise) |
| atsc.trellis | 0.068 (0.065) | 0.065 (0.064) | unchanged |
| atsc.symclock | 0.065 (0.048) | 0.046 (0.046) | unchanged (noise) |
| atsc.mix | 0.040 (0.030) | 0.005 (0.004) | The conjugate and the mixer to the channel centre run in the pass that splits the input. They use a phasor table per 1024-sample block, so there is no serial chain of complex products. |
| atsc.stats | 0.039 (0.038) | 0.015 (0.015) | The gain correction and the path noise come from one pass, with sums of l·l, l·t and t·t per segment. |
| atsc.rs, agc, eq.fit, input, deinterleave | ~0.08 | ~0.07 | unchanged |

## Stages, ATSC 3.0 at 10 Msps

| Stage | Before | After | What changed |
|---|---|---|---|
| a3.resample (exact-phase resampler to the frame rate) | 0.130 | 0.067 | The bank has 512 phases instead of 2048. The 400 kB bank, read at a random row for every output, missed the cache. The timing is still within 1/1024 sample: about -65 dB over a 6 MHz channel and -70 dB on the test's 0.9 MHz tone, against the receiver's own -33 dB. NEON now uses fused multiply-adds (as AVX2 already did), and a shared-window path was added (no measurable gain here; ATSC3_NOPAD=1 turns it off). |
| a3.derotate | 0.036 | 0.001 | The carrier offset is removed in the resampler's input pass (`resampleExactRot`), with no copy of the frame. The remaining derotate() uses a 256-sample phasor table instead of a complex<double> chain. |
| a3.ofdm.fft+est, equalise, demap, search, ldpc, bch | ~0.15 | ~0.13 | unchanged; LDPC was already 8-bit layered min-sum with a syndrome early stop |

## Quality

Base (ec3e8061a) and new were compared on the same machine.

- These tests pass on both builds with no wrong packets anywhere: atsc_rx, atsc_realworld, atsc_fec, atsc3_sync, atsc3_frame, atsc3_stream and atsc3_receiver.
- **atsc_rx, single seed.** Lock times are the same. Flagged counts move a little either way (clean 71 → 60, SNR 16.5 dB 151 → 202, -3 dB echo 1561 → 1666). Any change of float rounding moves which marginal packets fail: the old filter loop alone gives 165 and 1668.
- **atsc_rx, 12 seeds** (new `ATSC_RX_SEEDS` / `ATSC_RX_CASE` switch; default run unchanged). Good packets, base → new:

  | Case | Base | New |
  |---|---|---|
  | SNR 16.5 dB | 234218 | 234109 (-0.05 %) |
  | -3 dB echo at 5 us | 228193 | 228426 |
  | -8 dB echo at 2 us | 229068 | 229188 |
  | three echoes + CFO + SRO | 231888 | 231915 |

  Wrong packets: 0 in every case.
- **ATSC 1.0 at 10 Msps locks a few fields earlier on the test signal.** The first fields come out with some packets flagged (transport error indicator set) rather than not at all. The test signal gives 50180 packets, 578 flagged, all within the first 1243. The base build gave 48932, none flagged. So there are 670 more good packets, and none flagged after lock-up.
- **atsc3_sync resampler accuracy** went from -80 to -70 dB (the limit is -50 dB), as predicted for 512 phases.
- **atsc3_realworld** (label slow) fails on a loaded Mac with both builds. Its frame counts swing between 11 and 28 for the same input from run to run: the receiver sheds frames by wall clock. In every case and on both builds, failed frames and bad baseband packets are 0.

## CPU budget test

`atsc_cpu` (tests/atsc/test_atsc_cpu.cpp, label timing) uses the synthetic test signals: ATSC 1.0 at 8 and 10 Msps and ATSC 3.0 at 10 Msps, 4 s each. It runs each receiver alone, then ATSC 1.0 and ATSC 3.0 at once on two feeder threads.

- **Limits on a developer machine** (CPU s per s, all threads): ATSC 1.0 < 0.9, ATSC 3.0 < 0.5, both together < 1.0.
- **Limits on CI** (set from the measured runner figures): ATSC 1.0 < 2.0, ATSC 3.0 < 1.0, together < 3.0. They are not applied under sanitizers or with ATSC_CPU_NOLIMIT=1.
- **Stage output:** the test also prints the per-stage figures of one more run of each receiver, so the CI logs show where x86 spends its time.
- **Quality checks:**
  - ATSC 1.0 delivers at least 90 % of the possible packets good, with none flagged after the first eight fields.
  - ATSC 3.0 run beside ATSC 1.0 delivers at least 90 % of what it delivers alone.
- **Output:** CPUCAL lines for calibration.
- **Figures on this Mac (loaded):** 0.54 / 0.56 / 0.24, and 0.83 together. Base: 0.67 / 0.68 / 0.32, and 0.97 together.

Validated on simulated signals only (the app's generators). No real ATSC capture was available.

## Left

- ATSC 1.0 pll, symclock and trellis (about 0.18 together) are unchanged.
- The filter helper thread is still created per input block (about 150 a second).
- The ATSC 3.0 OFDM FFT, channel estimate, equaliser and demapper are unchanged.
- **x86:** feat39-atscx86 continues this on x86 (docs/ATSC-X86-3.9.md): about half the CPU on the same runner, and the atsc3_engine race found and fixed. Earlier state: **x86 is not under one core yet.** The first CI run (Linux x86_64 runner, Intel Xeon 6973P-C, 4 threads, the tests running 4 at a time) measured:

  | Case | CPU s per s |
  |---|---|
  | ATSC 1.0, 8 Msps | 1.22 |
  | ATSC 1.0, 10 Msps | 1.30 |
  | ATSC 3.0, 10 Msps | 0.55 |
  | both together | 2.13 |

  That is about 2.2 times the loaded M2 Pro. The next step is the per-stage line the test now prints there. The ATSC 1.0 decision-feedback loop and the stats are plain loops, which the baseline x86 build vectorises only with SSE2 (no AVX2 dispatch yet).
- **atsc3_engine on Linux x86 is unresolved.** It failed once in that CI run: 4 frames, 0 failed, 0 baseband packets, and the test ran into its 30 s limit. It passed on Windows x64 and macOS in the same run, and the other ATSC 3.0 chain tests passed on the same Linux runner: stream, receiver, frame, synth_rx and synth_basic. So the cause looks like that runner's environment, but it is not established.

## CI

- **Run 42914702868:** atsc_cpu passed on Linux x86_64, Windows x64 and macOS arm64, and atsc3_engine passed on all three, including Linux x86_64. Two tests failed, neither ATSC code: tsout_net (a paced-arrival timing check on macOS) and kaws_synth (a 499 s timeout on Windows).
- **x86 stage figures:** the CI logs keep no output of tests that pass, so the per-stage line from x86 has to be read with `ctest -R atsc_cpu -V` on such a machine.
