# Fuzzing every mode (3.9)

Branch `feat39-fuzz`. This work looks for crashes, hangs, memory errors and undefined behaviour in every decoder and parser.

## The sanitizer build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDECT2_SANITIZE=ON -DDECT2_BUILD_APP=OFF \
  -DDECT2_WITH_WHISPER=OFF -DDECT2_BUILD_PLUGIN_EXAMPLES=OFF \
  "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -gline-tables-only" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O1 -gline-tables-only"
cmake --build build --target dect2core fuzz_engine_modes fuzz_parsers   # and the fuzz_*_proto targets
```

- `DECT2_SANITIZE` enables ASan and UBSan with `-fno-sanitize-recover`, so any undefined behaviour stops the program.
- Asserts stay on.
- The core build dir is about 1.5 GB.

## Fuzzers

- **`fuzz_engine_modes`** (`tests/fuzz/fuzz_engine_modes.cpp`, ctest label `slow`). It runs every mode through the whole engine, each in its own child process. A source in memory (`setTestSourceFactory`) feeds these inputs:
  - random IQ
  - impulsive noise
  - the mode's own test signal: clipped, with a DC offset, cut into pieces (spans dropped and repeated), and with bit flips in the 8-bit samples
  - zeros
  - full-scale squares and chirps
  - huge, tiny and denormal floats (no NaN)

  It then runs this lifecycle:
  - retunes
  - a sample-rate change while running, which restarts the engine
  - mode switches A -> B -> A while running
  - six start/stop cycles, with ASan's live heap compared across cycles to catch leaks

  A watchdog reports a hung engine with its stacks. Environment variables:

  | Variable | Effect |
  |---|---|
  | `ONAIR_FUZZ_ONLY=id,...` | run only these modes |
  | `ONAIR_FUZZ_NEW=1` | also run the 3.9 block (engine codes 602 and up); these are reported, never failed |
  | `ONAIR_FUZZ_PARTNERS=n` | switch partners per mode |
  | `ONAIR_FUZZ_SEED` | the random seed |
  | `ONAIR_FUZZ_JOBS` | children run at the same time |
  | `ONAIR_TEST_LONG=1` | longer signals, and every other mode as a switch partner (all A -> B pairs) |

- **`fuzz_parsers`** (`tests/fuzz/fuzz_parsers.cpp`). It mutates the input of the C++ byte parsers that had no `fuzz_<mode>_proto` of their own:
  - ATSC PSIP, with half the sections given a correct CRC
  - CEA-608/708 captions
  - the ATSC 3.0 ALP reassembler
  - IPv4/UDP reassembly
  - LLS, AEAT and S-TSID
  - ROUTE/LCT
  - MIME multipart
  - deflate and gzip
  - the XML reader
  - RDS groups and bits
  - DVB-T2 BB frames

  It runs 4000 iterations per parser by default and 100000 with `ONAIR_TEST_LONG=1`. It also builds as a libFuzzer target with `-DONAIR_LIBFUZZER`.
- **The 24 existing `fuzz_<mode>_proto` targets** (ACARS, ADS-B, AIS, ANT, DAB FIC/MOT/PAD, DMR, DRM, D-STAR, DVB SI, FEC, files, HFDL, mesh/LoRa, net, NXDN, P25, packet, pager, SAME, TETRA, TS, UAT, VDL2, Wi-SUN) were run under the sanitizers.
- **libFuzzer** is not available with Apple clang, which has no fuzzer runtime. The local runs therefore use the fixed-seed loops. The libFuzzer entry points remain for Linux clang.

## Findings and fixes

Each fix has a short, deterministic regression test.

| Where | Bug | Fix | Test |
|---|---|---|---|
| `core/src/atsc3/atsc3_route.cpp` | A ROUTE start offset near 4 GB wrapped the 32-bit payload end, so the copy wrote before the buffer (heap overflow). Any large offset or EXT_TOL allocated gigabytes. | The end is computed in 64 bits. Objects over 64 MB are refused. Open objects are limited to 128 MB, and the oldest go first. | `atsc3_route` |
| `core/src/t2/t2l1.cpp` `p2Gather` / `p2Distribute` | A DVB-T2 L1-pre post size that is not a multiple of N_P2 wrote past the end of the P2 cell stream (heap overflow). | Every index is bounded. A valid signal gives the same cells as before. | `test_l1` |
| `core/src/hfdig/hfdig_ftx.cpp` | The FTX auto-search results (bool, int, double arrays) were read by `telemetry()` before they were written: an uninitialised bool (UB). | The arrays are value-initialised. | `test_hfdig_ftx_init` (the heap is filled with 0xBE, so the bug shows in any build) |
| `core/src/dvbs/dvbs_dsp.cpp` `SymbolTimer` | The output was sized for 1.5 samples per symbol, but the loop allows a step just over 1 sample. A pulled loop wrote past the end. This hit DVB-S/S2, ISDB-S, Himawari, FY-4 LRIT and GOES GRB. | The output is sized for the shortest step. | `test_dvbs_timer_bound` (heap overflow on the old code) |
| `core/include/dect2/dsp/sample_convert.h` `dropNonFinite` | Float samples of 1e30 or 3.4e38 passed the NaN guard. They overflowed to infinity and reached float-to-int conversions (GGAK, Wi-Fi, ITS-G5, iono, uplink, tetrapol). | Samples of 2^20 and more are also silenced. A radio never delivers them (full scale is 1, or 32768 for unscaled 16-bit samples). | `sample_guard` |
| `core/src/goes/goes_rx.cpp`, `core/src/elektro/elektro_ggak.cpp`, `core/src/fygeo/fygeo_rx.cpp` | The shared Gardner timing loop took any step. A spike far above the settled AGC moved `pos` far below zero: GOES (also used by Elektro and GK-2A) hung for good, and GGAK read before its buffer. A NaN position also reached an int cast. | The step stays between half a symbol and two. Non-finite errors are dropped, and the drop cast is guarded. | `test_goes_bursts` (GGAK crash on the old code) |
| `third_party/ssdv/ssdv.c` | A CRC or callsign byte of 0x80 or more was shifted into the sign bit of an int (UB). | The byte is widened to `uint32_t`. The change is noted in the file header (GPL-3 modification notice). | `test_ssdv_highbytes` |
| `core/src/kaal/kaal_tx.cpp` resampler | The buffer end was checked before the phase rounded up to the next input sample, so the kernel read one sample past the end. This is in the KAAL test-signal generator. | The bound is checked after rounding. | `test_kaal_tx_resample` (container overflow on the old code) |
| `core/src/wifi/wifi_ofdm.cpp` `viterbiDecode` | NaN soft bits (zeros equalised by a zero channel estimate) made every branch lose, so the int cast of a -1e30 metric was UB. | NaN or infinite soft bits count as erasures. Huge finite ones are held to 1e9, and the metric cast is clamped. | `test_wifi_viterbi_nan` |
| `core/src/iono/iono_dsp.cpp`, `core/src/tetrapol/tetrapol_rx.cpp` | A NaN frequency or timing error reached an int cast. The cause was infinite input, now removed by the input guard. | NaN-proof comparisons. | covered by `sample_guard` and `fuzz_engine_modes` |
| `tests/adsb/fuzz_adsb_proto.cpp` | The harness itself cast NaN results to `int`. | The results are folded as bits. | - |

## Results

Default sizes, seed 20261110, 3 switch partners per mode:
- **First full pass:** 296 modes, about 75 minutes. 19 modes failed, and the bugs above account for all of them.
- **After the fixes:** all 19 pass, and so do the five 3.9-block modes (wfb-ng, NPR, WINMOR, mioty, DECT NR+).
- **Second full pass** (seed 777, 5 switch partners per mode): 0 of 296 modes failed.
- **Leaks:** none found. The heap stays flat over the start/stop cycles; the largest change was about 2 MB per cycle in one mode, with no steady growth.
- **Parser fuzzers:** `fuzz_parsers` and all 24 `fuzz_*_proto` targets are clean after the fixes.

Validation: the inputs are synthetic (generators, noise and mutations), not recordings. The checks are for crashes, hangs, UB and leaks, not decode quality.

## Not done

- `ONAIR_TEST_LONG=1`, which runs every A -> B mode pair (about 300 x 300 switches), has not been run. It takes many hours.
- Real libFuzzer coverage-guided runs need Linux clang.
- ThreadSanitizer (`DECT2_SANITIZE_THREAD`) was not part of this pass.
