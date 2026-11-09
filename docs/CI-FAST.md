# Fast CI

`ci.yml` builds only the test programs a change can affect and runs them on parallel shards.

- `tools/ci_select.py` maps the changed files (git diff against the base) to test areas: `core/src/<mode>/`, `app/<mode>/`,
  `tools/<mode>tool.cpp`, `tests/<mode>/` -> the tests built from `tests/<mode>/`. A smoke set always runs.
  A shared file (dsp, engine, fec, session, CMake, third_party, core headers, tests/common) selects every non-slow test ("wide").
  Docs, android, packaging and similar select the smoke set only.
- Scopes: `auto` (default), `smoke`, `wide`, `full` (adds the slow tests). Tags `v*` run `full`; dispatch takes `scope` and `base` inputs.
- Only the selected targets are built (`cmake --build --target ...`), per shard: Linux 4 shards, macOS 2, Windows 2. ccache per os+shard is
  restored from the newest older run and saved even when a run fails; Linux links with lld and compiles with -g0 (disk).
- The required checks "Linux x86_64", "macOS arm64", "Windows x64 (MinGW-w64)" are gather jobs that pass when their shards pass.
- A newer push cancels the older run on the same ref. `ci-full.yml` (every platform, all tests, nightly slow tier) is unchanged apart from that.
- Locally: `tools/fast_check.sh [--scope wide|smoke|full] [--base REF]` does the same with `nice` and -j2.
