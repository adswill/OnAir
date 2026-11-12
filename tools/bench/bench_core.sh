#!/bin/sh
# Builds tools/bench/bench_core.cpp with only the DSP sources it needs (no other build needed), at the lowest priority, and runs it.
#   tools/bench/bench_core.sh [out dir] [-- bench_core arguments]
# DECT2_PORTABLE=1 builds the portable kernels instead of Accelerate (what Windows and Linux run).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-/tmp/onair-bench-core}
[ "$#" -gt 0 ] && shift
[ "$1" = "--" ] && shift
DEFS="-DNDEBUG $BENCH_CXXFLAGS"
[ -n "$DECT2_PORTABLE" ] && DEFS="$DEFS -DDECT2_PORTABLE" && OUT="$OUT-portable"
mkdir -p "$OUT"
cd "$ROOT"
LIBS="-lpthread"
[ "$(uname)" = Darwin ] && LIBS="$LIBS -framework Accelerate"
nice -n 19 c++ -std=c++20 -O3 $DEFS -Icore/include -Icore/src -w -o "$OUT/bench_core" tools/bench/bench_core.cpp core/src/dsp/dsp_compat.cpp \
  core/src/dsp/spectrum.cpp core/src/dsp/zoom_spectrum.cpp core/src/dsp/resampler.cpp core/src/dsp/exact_resampler.cpp core/src/common/threadpool.cpp $LIBS
[ -n "$NORUN" ] || nice -n 19 "$OUT/bench_core" "$@"
