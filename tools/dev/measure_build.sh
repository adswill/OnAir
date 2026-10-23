#!/bin/bash
# Timed build+test profile for docs/BUILD-SPEED.md. Run from the source root.
#   tools/dev/measure_build.sh BUILD_DIR JOBS PHASES [cmake args...]
# PHASES: comma list of cfg,cold,inc,link,hdr,ctest.  ccache is OFF here (CCACHE_DISABLE=1) so compile numbers are true costs.
# Prints "T <phase> <seconds>" lines and the build_profile.py summary.
set -u
B="$1"; J="$2"; PH="$3"; shift 3
export CCACHE_DISABLE=1
. "$HOME/.cargo/env" 2>/dev/null || true
t() { local n="$1"; shift; local s=$(date +%s%N); "$@" > "$B.$n.log" 2>&1; local rc=$?; echo "T $n $(( ($(date +%s%N) - s) / 100000000 ))e-1 rc=$rc"; }
has() { [[ ",$PH," == *",$1,"* ]]; }
echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"
if has cfg; then rm -rf "$B"; t configure cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"; fi
if has cold; then
  t cold_build cmake --build "$B" -j "$J"
  tail -n 5 "$B.cold_build.log"
  python3 tools/dev/build_profile.py "$B"
fi
if has inc; then   # one changed core .cpp, then relink one test
  touch core/src/dvbt/dvbt_rx.cpp
  t inc_core_cpp cmake --build "$B" -j "$J" --target test_dvbt
fi
if has link; then  # only the link of one test
  rm -f "$B/test_dvbt"
  t link_test_dvbt cmake --build "$B" -j 1 --target test_dvbt
  ls -la "$B/test_dvbt" "$B/libdect2core.a" 2>&1 | awk '{print $5, $9}'
fi
if has hdr; then   # a widely included header
  touch core/include/dect2/engine.h
  t hdr_engine_h cmake --build "$B" -j "$J"
  python3 tools/dev/build_profile.py "$B" --since-ms 0 | head -3
fi
if has ctest; then
  ( cd "$B" && s=$(date +%s); CI=true timeout 5400 ctest -j "$J" -LE slow --timeout 300 > "$B.ctest.log" 2>&1; echo "T ctest_not_slow $(( $(date +%s) - s )) rc=$?"; tail -n 6 "$B.ctest.log" )
  python3 tools/dev/build_profile.py "$B" --ctest | sed -n '/^ctest/,$p'
fi
echo "load at end: $(cut -d' ' -f1-3 /proc/loadavg)"
