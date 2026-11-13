#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Builds tools/bench/bench_dsp.cpp on its own (only the core sources it needs, not the whole project), at the lowest priority, and runs it.
#   tools/bench/bench_dsp.sh [build dir] [-- bench_dsp arguments]
# The objects are cached in the build dir (default /tmp/onair-bench-dsp): a second run only compiles what changed.
# Everything else the receivers need (audio output, feeds, the Rust layers ...) comes from an existing build: CORE_BUILD=<a cmake build dir>
# (its libdect2core.a, helper libraries and rust/release/libonair_rs.a are linked after the freshly compiled objects, so the sources measured
# are always this tree's).
# DECT2_PORTABLE=1 in the environment builds the portable kernels instead of Accelerate (what Windows and Linux run).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-/tmp/onair-bench-dsp}
[ "$#" -gt 0 ] && shift
[ "$1" = "--" ] && shift
JOBS=${JOBS:-4}
DEFS="-DNDEBUG $BENCH_CXXFLAGS"
[ -n "$DECT2_PORTABLE" ] && DEFS="$DEFS -DDECT2_PORTABLE" && OUT="$OUT-portable"
mkdir -p "$OUT/obj"
CXXFLAGS="-std=c++20 -O3 $DEFS -Icore/include -Icore/src -Ithird_party/uat_fec -w $(pkg-config --cflags libavcodec libavutil libswresample 2>/dev/null)"
SRCS=$(cd "$ROOT" && ls core/src/dsp/*.cpp core/src/common/*.cpp core/src/engine/channelizer.cpp core/src/fec/fec_proto.cpp \
  core/src/fm/*.cpp core/src/general/*.cpp core/src/adsb/*.cpp core/src/ais/*.cpp core/src/vdl4/*.cpp core/src/dmr/*.cpp \
  core/src/p25/*.cpp core/src/tetra/*.cpp core/src/dvbt/*.cpp core/src/dtmb/*.cpp core/src/dab/*.cpp core/src/atsc/*.cpp core/src/ltr/*.cpp core/src/edacs/*.cpp core/src/qo100/*.cpp \
  core/src/t2/t2fec.cpp core/src/t2/t2fec_tables.cpp core/src/t2/t2_tables.cpp core/src/t2/t2interleave.cpp 2>/dev/null \
  | grep -v -e gpu_ldpc -e gpu_viterbi -e _rs.cpp -e wxstudio -e imgproc -e wx_vectors) $EXTRA_SRCS
cd "$ROOT"
for s in $SRCS tools/bench/bench_dsp.cpp; do
  o="$OUT/obj/$(echo "$s" | tr '/' '_').o"
  stale=""
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then stale=1
  elif [ -f "$o.d" ]; then   # a header it includes changed (the dependency list the compiler wrote)
    for h in $(sed -e 's/^[^:]*://' -e 's/\\$//' "$o.d"); do [ "$h" -nt "$o" ] && { stale=1; break; }; done
  else stale=1; fi
  [ -n "$stale" ] && echo "$s $o"
done | xargs -P "$JOBS" -n 2 sh -c 'nice -n 19 c++ '"$CXXFLAGS"' -MMD -MF "$1.d" -c "$0" -o "$1" || echo "FAILED $0"'
OBJS=$(for s in $SRCS tools/bench/bench_dsp.cpp; do echo "$OUT/obj/$(echo "$s" | tr '/' '_').o"; done)
LIBS="-lpthread $(pkg-config --libs libavcodec libavutil libswresample 2>/dev/null)"
[ "$(uname)" = Darwin ] && LIBS="$LIBS -framework Accelerate -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -liconv"
FALLBACK=""
if [ -n "$CORE_BUILD" ]; then
  FALLBACK="$CORE_BUILD/libdect2core.a $(ls "$CORE_BUILD"/libdect2_*.a 2>/dev/null | tr '\n' ' ') $CORE_BUILD/libdect2core.a $CORE_BUILD/rust/release/libonair_rs.a"
  LIBS="$LIBS -framework Metal -framework Foundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework AVFoundation -framework Security -framework SystemConfiguration -framework IOKit $(for p in libavformat libswscale libusb-1.0 libhackrf hamlib librtlsdr libairspy SoapySDR; do pkg-config --libs $p 2>/dev/null; done) -lz -lsqlite3"
fi
nice -n 19 c++ -o "$OUT/bench_dsp" $OBJS $FALLBACK $LIBS
[ -n "$NORUN" ] || nice -n 19 "$OUT/bench_dsp" "$@"
