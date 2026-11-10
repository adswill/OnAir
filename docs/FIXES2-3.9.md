# Fixes, round 2 (3.9, branch feat39-fixes2)

## specx_idmatrix failed on Linux and Windows, passed on macOS
Cause: the test seeded each case's frequency offset and noise from `std::hash<std::string>` of the case label. libc++ (macOS) and libstdc++
(Linux, MinGW) hash strings with different functions, so Linux and Windows drew a different set of offsets and noise from the macOS run that
the thresholds were measured on. Because of that, Linux and Windows showed the same failing figures (frequency known, top-1 at 10 dB: 40.6 %;
digital voice, top-1 at 20 dB: 72.2 %). The identifier itself was not the problem.

Fix: `dect2::stable_string_hash` (dect2/common/stable_random.h) gives libc++'s 64-bit std::hash value (CityHash64) on every platform for strings
of up to 64 bytes. It was checked against libc++ on 6500 random strings. The test seeds from it, so macOS keeps its numbers.

Test: `common_stablehash` checks fixed libc++ values and a seeded first draw (to 1e-12: the log / sqrt of the libm may differ in the last bit).
Result: specx_idmatrix passed on Linux (quick.yml run 42895257180, 683 s).

## dvbs_narrow: Auto missed two impaired cases (all platforms)
These cases failed: 25 ksym/s at 4 dB, +310 kHz, +40 ppm (read as 26.65 ksym/s), and 33 ksym/s at 3 dB, -480 kHz, -60 ppm (read as
34.6 ksym/s). The same failure shows on macOS, so it does not depend on the platform.

Cause: at a few dB of Es/N0, the noise widens the spectrum's -3 dB width by about 5 %. That is more than the hunt's +-3.6 % rate dither can
cover. The cyclic line should have corrected the width. It was measured on 2^18 samples, which is about 3000 symbols at 2 Msps, and it reached
only 5 to 7.6 times the median (the check needs 12).

Fix (dvbs_rx.cpp):
- A carrier found by the finer search keeps up to 2^21 samples of input.
- The carrier is mixed to 0 Hz and averaged down to about 5.4 samples per symbol. The line is then measured over all of that input, which
  gives 8 times as many symbols.
- The line must reach 8 times the median of the bins where the spectrum's width puts it. The carrier is already confirmed at that point.

Result: dvbs_narrow passes on macOS (local, and quick.yml run 42898650255). Extra Auto cases also decode: 20-50 ksym/s at 2.5-4 dB, with +-60 ppm
and up to 480 kHz offset. Simulated signals only.

## Pagers on HF with a HackRF (-200 dBFS)
New test `pager_hackrf` runs POCSAG 512 through the engine's radio path:
- a fake HackRF Pro (DeviceInfo::HackRF, real-time, minimum rate 2 Msps)
- rate on Auto (2 Msps)
- 6.1625 MHz and 466.075 MHz
- Whole span off and on, with automatic offset tuning on and off

The fake radio delivers the signal at the frequency the user tuned, as seen from the centre and rate the engine asked the driver for. Every run
decodes every page and gets a real input level (macOS run 42898650255, Linux run 42925057963).

This means the engine path does not reproduce the -200 dBFS report. That covers the radio centre (+50 kHz), the rate handed to the pager
receiver (2 Msps), the -50 kHz signal offset and the mode's ModeTuning. Not covered:
- the real HackRF driver (USB transfers at HF)
- the app's settings on the owner's machine
- the 3.7 build itself

Not fixed, because the cause was not found. What would locate it: the log lines "sample rate: ..." and "Pagers: listening ..." from that session,
and the pager panel's "input ... Msps, ... dBFS" line. Simulated signal only.

## iq_modes crash (heap corruption on Windows, malloc abort on Linux)
feat39-qa does not touch it.

The output stops in the middle of the "Inmarsat-C (centre)" row only because of stdout buffering. The receivers (Inmarsat-C among them) are not
the cause. The table is printed after every receiver run has finished. The abort comes later, in the engine-spectrum part.

How it was found:
- Run one spectrum mode per process (`IQ_MODES_SPECTRUM_ONLY=1`, `IQ_MODES_INDEX=<n>`, kept in the test). This points to DVB-T2 alone:
  7 of 12 parallel runs aborted.
- Twelve DVB-T2 runs under ASan on Linux. These gave a heap-buffer-overflow WRITE in `p2Gather`, called from `T2Receiver::p2Hypothesis`.

Cause: `p2Gather` took the L1-post size from a decoded L1-pre. A pre can pass its CRC under the wrong carrier-mode hypothesis, or arrive
damaged by the spike. It can then signal more cells than the P2 symbols hold, and they were written past the stream. The macOS allocator did not
notice. glibc and the Windows heap do.

Fix: t2l1.cpp bounds every write and read. Such a frame now fails its L1-post decode.

Test: `t2_p2gather` checks two things:
- the round trip with p2Distribute
- impossible sizes: too many post cells, negative, the pre larger than the symbols, missing or short symbols

Built with ASan on macOS, the test fails with the old code (heap-buffer-overflow) and passes with the fix.

On Linux, quick.yml run 42925057963 passed iq_modes in full (241 s), along with t2_p2gather, common_stablehash and pager_hackrf. Before the fix, 7 of 12
parallel DVB-T2 runs aborted.

Also fixed on the way (undefined behaviour reported by UBSan, not the corruption): memcpy from an empty FIB in the DAB generator, and memcpy from
empty bits in the DMR Rust bridge.
