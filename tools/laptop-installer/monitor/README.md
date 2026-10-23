# OnAir laptop monitor

A live retro dashboard of what the laptop runner is doing. On Windows, double-click `OnAir-Monitor.cmd` (built from `monitor.py` by `../build_monitor.py`).

- `monitor.py` runs inside WSL (Ubuntu-24.04, user onair). Python 3 stdlib only, plain ANSI, no curses. It only reads files: `~/onair-monitor/state/` (written by the jobs), the runner's `_diag` logs in `~/actions-runner`, `/proc`, `/sys`, `ccache -s` and `nvidia-smi`. No token is needed.
- `onair_state.py` writes the state files. The jobs in adswill/OnAir-ci run their copy, `scripts/onair_state.py` (keep both the same); its `install` command copies these files to `~/onair-monitor/` unless the installed `monitor.py` has a higher `VERSION`.

State files: `current.json` (only while a job runs), `history.json` (last 100 jobs), `configure.log`, `build.log` (ninja with `NINJA_STATUS='[%f/%t %es] '`), `ctest.log`.

Try it on any machine: `python3 monitor.py --demo`. Snapshot: `python3 monitor.py --demo --dump out.txt --size 120x40 --demo-at 30`. Tests: `python3 tests/laptop_monitor/test_monitor_parsers.py`.

When you change `monitor.py`, raise `VERSION`, then run `python3 tools/laptop-installer/build_monitor.py`.
