# DVB family performance

How much CPU the TV receivers need per second of signal, stage by stage, and what was changed to bring it down (perf-dvb-030).

## How it is measured

`bench_dvb` (tests/bench/bench_dvb.cpp, built with the tests, not run by ctest) generates each standard's built-in test signal with the
synthetic source at the radio rate a user runs it at (resampled when the generator works at the channel's native rate), keeps it in memory
as 8-bit I/Q and feeds it straight into the receiver as fast as it takes it. It prints CPU seconds per second of signal for

- **all threads** of the process,
- the **feed thread** (the engine's analysis thread: the one the radio's ring waits for; above 1.0 the ring overflows),
- every **stage** the receiver names with `DECT2_STAGE("...")` (dect2/common/stage_prof.h: thread CPU time, exclusive of the stages
  opened inside it, off unless enabled: one relaxed load per scope),

and a digest (FNV-1a) of the transport stream delivered, which must not change when an optimisation claims to be exact.

```
bench_dvb --list
bench_dvb --case dvbt,t2 --seconds 6            # one or more cases
bench_dvb --case dvbt --pipelined                # the receivers' live-radio threading (DVB-T decoder thread, T2 resampler thread)
bench_dvb --json out.json                        # all cases, machine-readable
DECT2_STAGE_PROF=1 <any program>                 # switch the stage accounting on elsewhere
```

All figures below are CPU time on an Apple Silicon Mac shared with other jobs, run with `nice -n 19` (so partly on efficiency cores):
compare rows with each other, not with another machine. Each "before/after" pair was run alternately several times and the minimum taken
(the machine's load moves single runs by 10-20%).

## Results (CPU s per s of signal)

| Case | Before: all threads | After: all threads | Before: feed thread | After: feed thread | TS digest |
|---|---|---|---|---|---|
| DVB-T 8K GI 1/8 64-QAM 2/3, radio at 10 Msps | 0.57 | 0.51 | 0.37 | 0.26 (pipelined) | unchanged |
| DVB-T2 32K ext GI 1/128 PP7 256-QAM 2/3, 10 Msps | 1.22 | 0.95 | 0.34 | 0.32 | unchanged |
| DVB-S2 32APSK 4/5 8 MBd, 20 Msps | 1.57 | 1.21 | 0.28 | 0.28 | unchanged |
| DVB-S2 8PSK 3/4 10 MBd, 20 Msps | 1.09 | 1.03 | 0.31 | 0.32 | unchanged |

Baseline only (not changed yet): DVB-C 256-QAM 6.95 MBd at 10 Msps 1.13, ATSC 8 Msps 0.90, ISDB-T 8 Msps 0.57, DVB-S QPSK 10 MBd 0.52,
DTMB 10 Msps 0.49, ATSC 3.0 10 Msps 0.40.

### DVB-T stages (10 Msps, synchronous, minimum of 4 runs)

| Stage | Before | After | Change |
|---|---|---|---|
| dvbt.viterbi | 0.198 | 0.185 | Viterbi chunks on the shared pool, at most 4 at a time, instead of 4 threads started per block |
| dvbt.resample (10 -> 9.14 Msps) | 0.111 | 0.111 | the shared RationalResampler (perf-dsp-030's area) |
| dvbt.chanest | 0.089 | 0.064 | phasor per 64 carriers instead of sin/cos per carrier; buffers and carrier roles kept |
| dvbt.demap | 0.045 | 0.046 | |
| dvbt.rs | 0.032 | 0.018 | decoded bits appended in one go |
| dvbt.fft | 0.031 | 0.031 | |
| dvbt.depuncture | 0.025 | 0.026 | |

With `setPipelined(true)` (the engine does it for live radios) demapping, Viterbi and Reed-Solomon run on their own thread behind a
bounded queue: the feed thread keeps only input, resampler, sync, FFT and channel estimate. The owner's "load 115%, ring overflow" came from
the feed thread waiting for the Viterbi inside feed(). A full queue (the decoder is too slow for the machine) drops the oldest symbols; the
decoder restarts at the next one, and DECT2_DEBUG logs it with the stage names.

### DVB-T2 stages (after)

t2.ldpc 0.27, t2.demap 0.21, t2.resample 0.10, t2.chanest+eq 0.09, t2.measure+constellation 0.05, ts.out 0.05, t2.fft 0.04, t2.bch 0.04.

- `t2LdpcDecode` (core/src/t2/t2ldpc.cpp): decodeFast's layered normalised min-sum, bit-identical (decisions and iteration counts, every
  rate, both frame sizes, QPSK and 256-QAM, 5-30 dB), without the per-block allocation and zeroing of the 1 MB message store, and with the
  parity check on sign bytes. -40% per block on a good signal, -20% at the waterfall.
- Demapper: exact max-log per axis, eight cells at a time, bit de-interleaver folded in.
- An 8-bit LDPC (decodeFast8) for every rate was tried and rejected: about 1 dB worse at 256-QAM 2/3.
- A "the input is already a codeword" pre-check was tried and rejected: at 256-QAM even 30 dB leaves a bit error in most blocks, and the check
  costs 7% of a decode.

### DVB-S2

`s2Demap` eight symbols at a time with the same arithmetic (bit-identical LLRs): 8PSK 23 -> 5 ns per symbol, 16APSK 28 -> 8, 32APSK 82 -> 36.

### Behind time

- DVB-T (pipelined): a decoder thread that cannot keep up drops the oldest symbols and starts again; the count is in
  `RxTelemetry::dvbt.fecDropped` and the engine logs "this computer is too slow for the error correction (stages dvbt.demap, dvbt.viterbi,
  dvbt.rs)" at most every 5 s. The queue is bounded (192 symbols), so it never grows without limit.
- `bench_dvb` also prints the busiest thread: above 1.0 that thread cannot keep up even when the total fits in the cores.

### Fixed on the way

`dvbs::SymbolThread::push` (recordings, blocking mode) could wait for room forever: test_dvbs_rx2 hung for 900 s on pub/main's code
on this machine and passes with the fix (a block goes in once the queue is empty, whatever its size).

## What is left (largest first, from the profiles)

- DVB-S2 front end: S2Rx::fastRun, SymbolTimer, s2RefinePhase, PolyResampler.
- DVB-C: onSymbol, process, smooth.
- ATSC: pfbBlock, equalize.
- ISDB-T: the shared Viterbi (fecp::viterbiRange, about 70 NEON instructions per step: near its limit without 8-bit metrics, which would
  break the bit-exact comparison with the Rust decoder).
- DTMB: its LDPC (dtmb::LdpcCode); ATSC 3.0: resampleExact.
- The 10 -> 9.14 Msps resampler of DVB-T and T2 (shared, perf-dsp-030).
