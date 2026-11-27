# Fast CI and the one-command release

Goal for 3.8: from "release" to published in about 2 hours instead of 7. This page says what exists, what has been measured and what is only expected.

## The quick gate

`tools/gate/run.sh` runs in a few minutes with no build of the receiver library. It is `.github/workflows/gate.yml` on every branch and pull request, and the `Lint (seconds)` job of `ci.yml` (name unchanged) on main, 0.2.8 and integ-*, where every other job waits for it. It runs on a laptop too (`--no-compile` skips the three small C++ programs).

| check | how | what it catches |
|---|---|---|
| shell, workflow YAML, Python syntax, FFI widths | as before, now also tools/gate, tools/release, tools/package, android/tools | |
| mode numbers | `gate_check.py modes`: Python reading of `tests/app/test_mode_numbers.cpp` sections 1 and 3 | a family or poll slot used twice |
| web_policy classification | `gate_check.py web_policy`: Python reading of `tests/server/test_web_safety.cpp` section 4 | a new mode not classified in `server/web/web_policy.cpp` |
| Android native link | `gate_check.py android`: every FFmpeg, libusb, HackRF call of the core has a stand-in in `stubs_ext.c` | the missing stand-in of 3.7 |
| credits generator | `tools/gen_credits.py`, then `git diff --exit-code` | credits not regenerated |
| diagnostic codes | `tools/lint/diag_check.py --self-test`, then `tools/lint/diag_check.py` (docs/DIAGNOSTICS.md) | an invalid, reused or vanished code, a message that is not one literal sentence, a duplicate message |
| Android JNI table, generated mode list and results | the `android.yml` check step | stale generated files |
| gui_rules, credits table, i18n | the real test programs, compiled alone (`tests/app/test_*.cpp`) | |

The C++ tests stay the reference. They are in the smoke set of `tools/ci_select.py`, so each change runs them whatever its area. The gate only fails earlier. What it cannot see: engine standards clashing in the tuning registry (section 2 of the mode-numbers test needs the library), and an Android link error that is not one of those libraries (only the real link in `android.yml` shows that). `tools/gate/android_unreached.txt` lists calls in code the Android link drops (`--gc-sections`), taken from v3.7.0; a new name goes there only after a real link has shown it is not reached.

## Test selection and shards (`ci.yml`, `tools/ci_select.py`)

- `tools/ci_select.py` maps the changed files to test areas (`core/src/<mode>/`, `app/<mode>/`, `tools/<mode>tool.cpp`, `tests/<mode>/`). A shared file (dsp, engine, fec, session, CMake, third_party, core headers, tests/common, web, relay) selects every non-slow test. Docs, android and packaging select the smoke set.
- Scopes: `auto` (default), `smoke`, `wide`, `full` (adds the slow tests). Tags `v*` run `full`; dispatch takes `scope` and `base` inputs.
- Only the selected targets are built, per shard: Linux 4, macOS 2, Windows 4. Linux and macOS configure with the app on, so the app tests (gui_rules, mode_numbers, i18n, ...) stay in the list; the first version of this file had them only on Windows.
- Test timeouts are as before the shards: 600 s, 1500 s only for the full list.
- ccache per OS and shard, restored from the newest older run (any shard of that OS when the shard has none), saved also when a run fails, in `ci.yml`, `ci-full.yml`, `release.yml` and `android.yml`.
- The required checks "Linux x86_64", "macOS arm64", "Windows x64 (MinGW-w64)" are gather jobs that pass when their shards pass.
- 3.9: shards balanced by measured time (`tools/ci_times.tsv`), shared files followed through the includes, shorter default runs of the slowest tests with `ONAIR_TEST_LONG=1` for the whole sweeps, and the cache policy: see `docs/CI-SPEED-3.9.md`.
- Locally: `tools/fast_check.sh [--scope wide|smoke|full] [--base REF]` does the same with `nice` and -j2.

## Release: `tools/release/release.sh <x.y.z> [--publish]`

Each platform is a run of its own (`release.yml` with `platform=windows|macos|linux`, `android.yml`), started together and waited for in parallel, so a failed one can be run again at once: GitHub only reruns failed jobs once their whole run has ended. A failed run is run again automatically once. Files go on a draft; `--publish` makes it public only after every expected asset is on the draft with the right size. The script never pushes a branch or tag and never publishes without `--publish`.

- Windows: `release.yml` cross-builds the zip on macOS with Homebrew mingw-w64, then a Linux job makes the installer with NSIS. It builds on macOS because the last Linux cross builds (`package.yml` of OnAir-ci, run 39996949637) failed `check_win_dlls.sh`: the DLLs of the prefix need the libstdc++ they were built with. The prefix comes from a draft release: **once**, on the maintainer's Mac, and again when `cross_deps_windows.sh` or the toolchain file changes, run `tools/release/upload_win_prefix.sh` (the file name holds a hash of those two files). Without the prefix the Windows jobs only warn, and `release.sh` stops with what to do, or takes `--windows DIR`.
- Android: signed only when the `ANDROID_KEYSTORE_*` secrets exist; otherwise the APK is unsigned and `release.sh` stops unless `--apk FILE` is given. The keystore never goes in a log or artifact, is decoded only for the one Gradle step and is removed whatever happens; the passwords reach only that step.
- Downloads and installs in the release retry: `tools/release/retry.sh` around brew, apt, rustup, the NDK and libusb clone; curl with `--retry 5 --retry-all-errors`.
- `release.sh` runs the gate first. Before everything is merged, `release.sh X --no-draft` on an integration branch rehearses the whole build without touching a release, so packaging problems surface early.

## Measured and expected

Measured (before this work):

- 3.7 release cycle about 7 hours. Fast tests about 63 min Linux, 55 min macOS, 97 min Windows, with a Windows build of about 100 min (run 42608104759 and earlier).
- The first shard run on this design (run 42368655094): Linux shards 17-23 min, macOS shards 31-39 min (the second started only when the first ended: runner limit), Windows shard 1 92 min (about 55 build, 30 fast tests).

Measured here, without CI:

- `gate_check.py` on v3.7.0 passes in about a second. On the commit before `av_guess_frame_rate` got its stand-in it reports that missing stand-in and the missing BGAN classification; on the commit before the BGAN fix it reports the classification. A mutated mode table with a family used twice fails.

Expected, not proven (needs CI runs once the 3.7 runners are free):

- the gate at a few minutes including the three C++ programs; Windows at 4 shards under 45 min cold and much less with a warm ccache; a whole release near 2 hours.
- the Windows release job: same Homebrew compiler as the prefix on `macos-15` (a newer Homebrew GCC than the prefix was built with would fail `check_win_dlls.sh`; the fix is to rebuild and upload the prefix). The `/tmp` step for the Windows shards: the file-writing tests failed on run 42368655094 because `D:\tmp` only existed when an earlier test happened to create it.

Known and not fixed here: `tsout_net` failed once on Windows (http stream); `mesh_engine` on macOS failed in the check after a retune (`seq 113, 27 frames`), the frame count was not reset after 800 ms, reason unknown; both need a CI run to tell if they repeat.

## CI rules after 4.4 (what runs where)

- Pushes to `main` run Lint and the Linux shards only; docs-only pushes (`*.md`, `docs/`, `website/`) build nothing. macOS and Windows shards run on `integ-*` branches, tags, manual dispatch and the nightly Full CI (`ci.yml`).
- Lint also configures CMake with the release options (app and tests on), so a duplicate target or bad glob fails in about a minute.
- `android.yml` job `link` builds the core and the JNI bridge with the NDK on Linux, nightly and by hand (input `link`); `gate_check.py android` only compares names. Linux hosts stage the headers with `android/tools/stage_host_headers.sh` so glibc's headers do not shadow the NDK's.
- `win-prefix.yml` runs after a push to `main` that changes `cross_deps_windows.sh` or the toolchain file; with no `from` it starts from the newest prefix on the draft release and does nothing when the current id is already there. A `v*` tag fails its Windows job when the prefix is missing.
- Looked at and left alone: sharing ccache between the release jobs (each compiles different targets with a different compiler, so nothing would hit), and moving the Windows cross build to Linux (docs above: the Linux cross build fails `check_win_dlls.sh`, and the prefix would have to be rebuilt there first).

## 4.7: a full run in minutes, not hours (CI speed)

Goal (owner, 2026-11-27): the full CI on GitHub's free runners under 20 minutes, without testing less. Every test still runs in the
full scope; what changed is where and how often the work happens.

### What was measured before (2026-11-14 to 2026-11-27)

- The nightly Full CI (`ci-full.yml`) had not finished once since 2026-11-14: its test jobs ran into GitHub's 6-hour job limit. It ran the
  slow tier and the long forms (`ONAIR_TEST_LONG=1` for the whole workflow) once per platform in one job each, and held 13 runners meanwhile.
  Run 45804890762 (Linux arm64, the one job that finished its tests): slow tier 51,049 s plus long forms 15,295 s of test time on one 4-core
  runner. Thirteen tests were 68 % of it: `fuzz_engine_modes` (timeout at 7,200 s: with the long form every mode had every other mode as a
  switch partner), the eight `qa_sweep` shards (35-70 min each, long form: 8/32/120 s signals), `sens_full` (6,764 s, a measurement with
  JSON output and a 10-hour timeout), `sens39c`/`sens40b` (timeouts at 3,000 s), the GNSS sweeps.
- A push run of `ci.yml` took 7-20 hours from start to end although its jobs ran 35-100 min: jobs waited for runners (one shard 18 h in run
  45136260924). The free plan runs 20 jobs at once (5 macOS); the nightly took 13 of them for 6 hours.
- Builds, not tests, were the longest step on macOS and Windows: 55 min (macOS shard) and 63 min (Windows shard) in run 45136260924. A
  branch can restore only its own caches and main's, pushes to main run only Linux, and the warm-up built Linux only, from the newest
  `integ-*` branch by name (`integ-46-undock`, last commit 2026-11-23) instead of main. The cache store was at 10.7 GB of 10 GB: two copies
  of most families, so the newest caches were evicted.
- With a warm cache the whole Linux build is 3.5 min (run 45804890762); with 165 misses after a header change, 12.5 min (run 45343895120);
  linking 800 programs took about 1.5 min of it. 5,095 files compile (core 2,221, tests 1,995); a typical engine or test file is 130-180
  thousand preprocessed lines, three quarters of them system/STL headers.
- Fast tier: 18.7-22.7 thousand test-seconds per platform (under ctest -j4), about 3 tests running on average out of 4 slots.

### What changed

| change | where | effect |
|---|---|---|
| shard count from the selected tests' measured seconds, about 10 min of tests per shard (Linux up to 14, macOS 4, Windows 5) | `tools/ci_plan.py`, lint job of `ci.yml` | a big selection gets enough shards; a small change gets one per platform instead of 4 that each pay set-up and build |
| the nightly full run is `ci.yml` (schedule): every test, the slow tier on Linux | `ci.yml` | replaces the 6-hour jobs |
| the slow tier runs on Linux only; macOS and Windows run every other test | `tools/ci_select.py --no-slow` | the sweeps are the same C++ on every platform; platform differences show in the fast tier |
| `qa_sweep` 8 shards -> 32, `fuzz_engine_modes` 1 test -> 8 (`ONAIR_FUZZ_SHARD=k/N`) | `CMakeLists.txt`, `tests/fuzz/fuzz_engine_modes.cpp` | no single test longer than a shard |
| `sens_full` labelled `measure`: never a CI tier (like `snr`) | `CMakeLists.txt`, `tools/ci_select.py` | 6,764 s less; run it with `ctest -L sens-full` |
| the long forms (`ONAIR_TEST_LONG=1`) weekly on Linux x86_64; platform variants (arm64, scalar, portable, Intel) weekly, fast tier | `ci-full.yml` | |
| compile jobs: when the selection is wide or full, Linux 6 and macOS 4 jobs each compile a part of the objects into a ccache file store; the shards read all parts | `tools/ci_objs.py`, `ci.yml` job `compile` | a cold build is split instead of done again by every shard |
| ccache warm-up of main on Linux, macOS and Windows, after every push to main and nightly | `ccache-warm.yml` | every new branch starts warm on every platform |
| older copies of each ccache family deleted | `tools/ci_cache_prune.py` (last job of `ccache-warm.yml`) | stays under 10 GB, the newest caches stay |
| jobs end at 90 min (Windows 120) | `ci.yml` | a hang never holds a runner for hours |
| wall and CPU seconds of every test | `tools/ci_testtime.py` (ctest launcher), `testtime.log` in the shard artifacts | finds the tests that wait instead of compute |
| wall seconds and tries per QA mode | `QA_TIMING_LOG`, `qatime.log` in the shard artifacts | |

### Workers (agents) and CI

A worker's check should cost one or a few runners, and the full run happens once per batch, not once per worker:

1. Worker branch, ordinary change: `gh workflow run quick.yml -R adswill/OnAir --ref <branch> -f tests=auto`. One job builds and runs every
   test the change can affect: the changed areas, every test whose sources reach a changed header (the include graph), and the smoke set.
   Nobody picks tests by hand, so a test of another area that the change breaks is in the set when that area's code reaches the change.
2. Worker branch whose change reaches shared code (quick.yml stops with this command): `gh workflow run ci.yml -R adswill/OnAir --ref
   <branch> -f platforms=linux`: the Linux shards of the wide scope (every non-slow test).
3. Integration branch, after a batch of merges: the push runs `ci.yml` (auto scope; a newer push cancels the older run, so a batch of
   merges costs one run). By hand: `-f scope=wide`, every non-slow test on Linux, macOS and Windows.
4. The slow tier (the deep sweeps: QA sweep of every mode, GNSS skies, sensitivity sweeps; 55,000 of 81,000 test-seconds on Linux)
   runs in the nightly full run (`ci.yml` schedule) and must pass before a release (`-f scope=full`, and every `v*` tag). Owner's
   decision 2026-11-27: it no longer blocks each integration batch.
5. A failed run prints the failed tests and one `quick.yml` command that reruns only them (`tools/ci_failed.py`, the gather jobs'
   summary): fix, rerun those in minutes, then the full scope once.
6. Wait with one `gh run watch <id> -R adswill/OnAir --exit-status` in the background; never poll.

### Measured after

(filled in from the runs on branch ci-speed below)
