# CI speed (3.9)

Branch `feat39-cispeed`. Aim: make `ci.yml` much cheaper for the ~20 workers and the integrator without losing what it catches.

## What the time went into (measured before the change)

Runs on integ-38, both `scope=wide` (every non-slow test). Per test: `tools/ci_times.py --report <run>`.

| run | wall | runner minutes | Linux shard (tests) | macOS shard (tests) | Windows shard (tests) |
|---|---|---|---|---|---|
| 42820553355 (warm ccache) | 55 min | 381 | 26-46 min (21-29) | 43-55 min (33-46) | 32-43 min (22-36) |
| 42785576823 (cold ccache) | 102 min | 548 | 36-54 min (21-30), build 12-19 | 57-101 min (30-48), build 20-48 | 22-64 min (31-38), build 19-31 |

- Tests are most of the time: 359 test-minutes on Linux, 318 on macOS, 466 on Windows over ~1450 tests. The slowest 35 tests are a third of it, the slowest 100 tests 58 %. 700 tests take under a second each (90 s together).
- Shards were split by a hash of the test name, so one shard got 29 min of tests and another 21.
- Overhead: `apt-get install` took 10.8 min once (72 MB from a mirror at 115 kB/s, run 42834606833), usually about a minute. Removing the preinstalled toolchains took 1-2 min. msys2 setup 2 min. The Rust workspace builds in 10-15 s, so it needs no cache.
- ccache: the repository's Actions cache was full (10.7 GB of 10 GB, 45 entries, 2026-11-10). Every branch that ran ci.yml saved its own 11 entries (150-390 MB each), and a branch can only read its own caches and main's. main had none, so a new branch (and quick.yml) always built cold: 20-48 min on macOS.
- Link time is small next to the tests (warm build 1.4-2.6 min on Linux with lld, 4-5 min on macOS), so merging test programs into bundles would save little and conflict with every worker adding tests: not done.

## What changed

1. **Shorter slow tests.** 65 of the slowest tests (46 in round 1, 19 in round 2) run a shorter default: fewer seconds of signal, fewer sweep points, seeds and offsets, keeping each mode/rate/impairment class and the point each threshold is checked at. No threshold changed. `ONAIR_TEST_LONG=1` (`tests/common/test_long.h`: `testLong()`, `testLongPick(quick, full)`) runs exactly the old sweep. It is on for release tags, for `ci.yml` dispatched with `long=true`, and for the nightly Full CI, which now runs the long form of these tests after the slow tier (`ci_select.py --scope long` finds them by the include).
2. **Shards balanced by measured time.** `tools/ci_times.tsv` holds the seconds per test and platform; `ci_select.py --times` puts the longest test on the least loaded shard. A test missing from the table counts 5 s. It also writes `build/Testing/Temporary/CTestCostData.txt`, so `ctest -j` starts the longest tests first instead of leaving one long test to the end. Refresh the table with `python3 tools/ci_times.py <run id>...` after big test changes.
3. **macOS gets a third shard** (it was the longest leg).
4. **Caches.**
   - ccache is saved only by main and integ-*; feature-branch runs restore but do not save. quick.yml saves one entry per branch instead of one per commit.
   - A nightly warm-up job in `ci-full.yml` (scheduled, so its cache belongs to main and every branch can read it) builds every test of the newest integ-* branch with ci.yml's flags. ci.yml and quick.yml fall back to it (`ccache-<os>-warm-`). Linux only for now; macOS and Windows feature runs are rare.
   - `ccache -z` after the restore, so the statistics step shows the run's own hit rate (it showed the cache's lifetime total).
   - `.github/actions/linux-deps`: the apt `.deb` files are cached per package set and month.
5. **Less overhead.** Freeing disk on Linux runs in the background. Homebrew does not auto-update. Windows with lld was tried and dropped: in that run `native_kiwisdr` failed and `audio_engine` hung, though the kiwisdr failure came back with GNU ld (it passes on the retry of the timing run), so lld was not proven guilty.
6. **Wall-clock tests in the timing run.** `time_machine` and `pskdsp_engine` failed next to the longest tests once ctest started those first, so they now carry the `timing` label (run apart, -j 2, retried).
7. **Precise auto scope.** A changed file in a shared area (dsp, fec, common, core headers, tests/common ...) used to select every non-slow test. `ci_select.py` now follows it through the `#include` graph: it chooses the tests whose own sources reach it and the mode areas whose code reaches it. A library `.cpp` passes the change on to its own header, and through that to the header's includers. The change stays wide when:
   - it reaches the engine, session, source or app other than through a mode's header;
   - a reached `.cpp` has no header of its own area;
   - it reaches an area with no tests;
   - it hits more than half of the areas.

   Measured over the 448 shared source files: 93 now choose a focused set (median: one area), and the rest are still wide. Mode-area changes were already precise. `tools/test_ci_select.py` checks the selector without a build (it is part of the quick gate).

Not changed, by agreement: `custom_bandwidth_all`, `iq_modes` and the `tests/tx/` tests belong to the QA worker (feat39-qa). `custom_bandwidth_all` is the slowest test of all (688 s Linux, 516 macOS, 986 Windows in run 42820553355; 1200 s in 42785576823), so it is the next one to shorten. Tests of families 601-789 and the new-mode files were not touched.

## Measured after

Both after-runs are `scope=wide` like the before-runs (there is no full-scope run to compare with: the v3.8.0 tag run was cancelled), dispatched on feat39-cispeed. They built **cold**: a feature branch cannot read integ-38/39 caches and main had none, which is the cache problem itself. So compare test and overhead minutes; the build minutes of the after-runs are cold-build minutes.

| | before 42820553355 (warm) | before 42785576823 (cold) | after 42886049629 (round 1) | after 42900861790 (round 2) |
|---|---|---|---|---|
| runner minutes | 381 | 548 | 547 | 522 |
| of it: build / tests / other | 37 / 305 / 39 | 256 / 254 / 37 | 268 / 257 / 21 | 283 / 216 / 23 |
| longest test step, Linux | 29.3 min | | | 23.8 min |
| longest test step, macOS | 47.7 min | | | 23.4 min |
| longest test step, Windows | 36.8 min | | | 26.1 min |
| test steps per shard | L 22-29, M 37-48, W 24-37 | | | L 16-24, M 18-23, W 14-26 |

- Test minutes: 305 -> 216 (-29 %). Linux 96 -> 76, macOS 84 -> 61, Windows 124 -> 79.
- The 46 tests of round 1, measured alone in quick run 42881392517 (Linux): 145 -> 45 test-minutes. In a full run they count less because the remaining tests slow down when they share the runner with the longest ones. Per platform, the summed test seconds went 359 -> 313 (Linux), 318 -> 222 (macOS) and 466 -> 373 (Windows) after round 1.
- Overhead other than build and tests: 39 -> 23 min. The cached .debs remove the apt outliers.
- Wall time: the longest test leg is down from 48 to 26 min. With a warm cache (integ-*, which saves) a run should be about 5 min of setup and build plus 26 min, so roughly 30 min instead of 55, and about 280 runner minutes instead of 381. That is expected, not measured: it needs the first integ-39 run after the merge, and the one after it (the first one re-fills the shard caches).

Failures seen and handled. Round 1 cut some signals below what lock needs (aero_crowded 6 dB, Inmarsat-C, inmsat_manager), and p25_short and nxdn_short lost seeds; all of these were restored. secondary_modes now keeps its old order. `autoparam_kbd`'s live relock (real-time paced, unchanged code) failed once in the quick run. `native_kiwisdr` on Windows fails its first try in both after-runs and passes on the retry.

What's left:
- shorten `custom_bandwidth_all` (QA worker);
- refresh `tools/ci_times.tsv` after the integ merge (`python3 tools/ci_times.py <run>`);
- a macOS/Windows warm-up if feature branches start running those platforms often;
- bundling test programs was measured as not worth it (builds are 1-5 min warm).
