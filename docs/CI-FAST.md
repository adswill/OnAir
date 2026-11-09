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
