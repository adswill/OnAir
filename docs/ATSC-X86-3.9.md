# ATSC 1.0 and ATSC 3.0 CPU on x86 (3.9, feat39-atscx86)

This branch follows feat39-atscperf (docs/ATSC-PERF-3.9.md). That work brought ATSC 1.0 plus ATSC 3.0 to 0.79-0.83 cores on Apple silicon, but they still needed about 2.1 cores together on the Linux x86 CI runner. This round targets x86. Three changes also apply on ARM, none of them to the arithmetic: the thread splits that now depend on load, the ATSC 3.0 idle refresh, and the non-finite pre-check. The numeric code paths on ARM are unchanged.

## How it was measured

- The test is `atsc_cpu`: the synthetic test signals, 4 s each, CPU seconds per second of signal over all threads. Its last line gives the per-stage figures of ATSC 1.0 and ATSC 3.0 at 10 Msps.
- The runner was GitHub's `ubuntu-24.04` (GCC, Release). It has 4 vCPUs, which are 2 cores with 2 threads each. Each run gets a different processor: AMD EPYC 7763 (Zen 3), 9V74 (Zen 4), 9V45 (Zen 5) or Intel Xeon 8573C. So figures from different runs cannot be compared.
- **Fair comparison:** `quick.yml` got a `compare` input. It builds the same tests from a second ref (`feat39-atscx86-base`: feat39-atscperf plus the workflow) and runs them after this branch's, on the same runner.
- **Other new `quick.yml` inputs:**
  - `verbose` runs ctest with -V, so the output of passing tests is kept.
  - `env` passes environment variables to the tests.
  - `perf` runs one test under `perf record` and prints the hottest functions and instructions.
- **No x86 desktop was available**, so every x86 figure here comes from these shared runners.

## Totals (CPU s per s; each row is one runner, base then new)

| Runner | ATSC 1.0 8 Msps | ATSC 1.0 10 Msps | ATSC 3.0 10 Msps | both together |
|---|---|---|---|---|
| EPYC 7763 (Zen 3), final code | 1.761 → 0.793 | 1.737 → 0.906 | 0.679 → 0.615 | 2.636 → **1.720** |
| EPYC 9V74 (Zen 4) | 1.421 → 0.695 | 1.478 → 0.725 | 0.567 → 0.510 | 2.179 → **1.499** |
| EPYC 9V45 (Zen 5) | 1.179 → 0.522 | 1.241 → 0.536 | 0.472 → 0.353 | 1.802 → **1.102** |

- The Zen 4 and Zen 5 rows are from one or two commits before the final code. Those commits are without the AVX2 data-SNR pass and the telemetry fix; the other changes are in.
- A second Zen 4 run had a busy neighbour: base 2.957, new 1.904 together. It shows the same ratio.
- **Under one core together was not reached on the CI runner.** The best runner gives 1.10 and the slowest 1.72; before this round they gave 1.80 and 2.64.
- **Why "both" is higher than the sum of the two alone:** the two receivers run 7 threads on 2 physical cores. Threads that share a core run slower, and every thread's CPU time counts in full.
- **Desktop x86:** a desktop with more cores than busy threads should come in well under the Zen 5 runner's 1.10. This was not measured.

## Stages, ATSC 1.0 at 10 Msps (EPYC 7763, same runner)

| Stage | Base | New | What changed (x86 only) |
|---|---|---|---|
| atsc.filter | 0.567 | 0.183 | The shared-window matched filter is eight lanes wide in AVX2 + FMA (`pfbPadAvx2`). The four outputs' lane sums come from two rounds of hadd, and their positions and phases are worked out side by side in one vector. perf shows the loop's fused multiply-adds take most of the time now: 56 per four outputs, the filter's own size. The filter's helper thread is only started when the sample thread is busy more than 60 % of real time (stopped below 45 %). Before, it started for every block (`ATSC_SPLIT=1/0` forces it). |
| atsc.eq.dfe | 0.273 | 0.090 | The decision feedback loop over a field is AVX2 + FMA (`dfeFieldAvx2`). Its level decision is one `roundss` plus clamps. It was a call to `floorf`: the SSE2 baseline has no rounding instruction, so GCC calls libm. The accumulator pass is eight lanes wide. |
| atsc.eq.fit | 0.177 | 0.066 | `syrkLowerT` keeps 4 x 16 tiles in registers over blocks of 256 rows. Each element still adds the rows in order with one fused multiply-add, the same bits as the AVX2 copy of the axpy loop (checked against `std::fma` under Rosetta). |
| atsc.symclock | 0.175 | 0.089 | The symbol interpolator uses SSE2 (the same additions in the same order, so the same bits). The floor is exact and needs no libm call (`dect2::ifloor`). The buffer limits are converted to double once per call, not per symbol. |
| atsc.trellis | 0.120 | 0.089 | The twelve Viterbi lanes are one 256-bit vector and one 128-bit vector (AVX2), where SSE2 used three 128-bit ones. The adds, compares and minimums are the same, so the result is bit-exact. |
| atsc.eq.filter | 0.113 | < 0.012 | The data SNR pass (`measureData`) is eight lanes wide in AVX2, with the sum in double lanes. The levels sampled for the display come from a precomputed index list. |
| atsc.eq.ff | 0.108 | 0.036 | The feed-forward equaliser (65 or 129 taps) computes 32 outputs at a time with a broadcast tap (`corrAvx2`). The portable `convCorr` ran one 8-lane chain and a reduction for every output. |
| atsc.pll | 0.099 | 0.087 | The 16-sample block uses SSE2 in the NEON arrangement; it ran scalar on x86 before. The loop is latency-bound (one phase update per block), so the gain is small. |
| atsc.input | 0.021 | 0.012 | A vectorised test for non-finite samples runs first. The per-sample fix-up only runs when it finds one. |
| rs, stats, agc | 0.089 | 0.088 | unchanged |

## Stages, ATSC 3.0 at 10 Msps (EPYC 7763, same runner)

| Stage | Base | New | What changed (x86 only) |
|---|---|---|---|
| a3.resample | 0.214 | 0.117 | The shared-window resampler is in AVX2 (`resPadAvx2`), with the lane sums of four outputs formed together. The output position's floor and round need no libm call. The resampler only splits a block over threads when it uses more than a third of real time (`ATSC3_SPLIT=1/0` forces it). |
| a3.search | 0.113 | 0.091 | The bootstrap search compares its metrics squared. On x86, `std::abs` of a complex is a `hypot` call. The template products are written out, because the complex operator's NaN check kept that loop scalar. |
| a3.ofdm.fft+est | 0.096 | 0.105 | unchanged (noise) |
| a3.ofdm.equalise, ldpc, others | ~0.17 | ~0.18 | unchanged |

ATSC 3.0 gained least. Most of what is left there is in the OFDM FFTs: GCC builds keep the one-stage FFT loop for bit-exactness.

I tried a GCC version of the AVX2 multi-stage FFT passes, with the second product fused. It did not reproduce GCC's bits (`fft_bits` failed), so it was reverted.

## Denormals

The ATSC 1.0 sample thread (inside `feed()`, with MXCSR restored on return), its worker and filter helper, and the ATSC 3.0 worker and frame pool now run with flush-to-zero and denormals-are-zero on x86 (`dect2::DenormalsOff` in `dect2/dsp/simd.h`). On ARM it does nothing.

## Quality (same runner, base against new)

- **Tests:** atsc_rx, atsc_fec, atsc_realworld, atsc3_sync, atsc3_frame, atsc3_stream, atsc3_receiver, atsc3_engine, atsc3_realworld, fft_bits and atsc_cpu pass on the final code (run 42937781102).
- **atsc_rx, 12 seeds, x86** (`ATSC_RX_SEEDS=12`; `ATSC_RX_CASE` now takes comma-separated names). Good packets, base → new:

  | Case | Base | New |
  |---|---|---|
  | SNR 16.5 dB | 234503 | 234297 (-0.09 %) |
  | -8 dB echo at 2 us | 229068 | 229068 |
  | -3 dB echo at 5 us | 228136 | 228248 |
  | three echoes + CFO + SRO | 232111 | 232115 |

  Wrong packets: 0 in every case. The x86 base gives exactly the same counts on Intel and AMD runners.
- **ATSC 1.0 at 10 Msps, test signal:** 49868 packets on both builds. 431 are flagged, against 409 on base, all within the first 933, before lock-up.
- **ARM:** see the macOS comparison below.

## atsc3_engine on Linux

The failure seen before (frames counted, 0 baseband packets, no service selected) was a race. The ATSC 3.0 worker selected the service and published its telemetry only after a block of samples. Frames that were still being decoded when the last block of a file arrived committed afterwards, so their services were never selected and the counts stayed stale.

The worker now also does both on its idle 100 ms wake-ups. With the fix the test shows 5 frames and 39 baseband packets; base showed 4 and 16. The telemetry now also drops "locked" 1.5 s after the signal stops, where it used to stay at its last value.

## Left

- **Under one core together on the CI runner:** not reached (1.10-1.72).
- **The filter:** it is near its multiply-add bound for a 52-tap complex filter at 21.5 Msps out. Going further needs a different filter structure, which would change the receiver's response.
- **ATSC 3.0 OFDM FFTs** on GCC: see above.
- **The bootstrap search** runs a 32K FFT pair for each carrier offset hypothesis. Hypotheses 3 kHz apart are exact 16-bin shifts of one spectrum, so one forward FFT could serve them all. Not done.
- **Not measured:** an x86 desktop, and Windows. The AVX2 paths need a GCC-compatible compiler (MinGW); an MSVC build takes the baseline paths.
- Validated on simulated signals only (the app's generators). No real ATSC capture was available.
