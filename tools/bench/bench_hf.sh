#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Builds tools/bench/bench_hf.cpp with this tree's core/src/hfdig (and psk js8 cw hfdl ale) sources at -O3, linked ahead of an existing build's libdect2core.a, and runs it.
#   CORE_BUILD=<cmake build dir with libdect2core.a> tools/bench/bench_hf.sh [out dir] -- <bench_hf arguments>
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-/tmp/onair-bench-hf}; [ "$#" -gt 0 ] && shift; [ "$1" = "--" ] && shift
mkdir -p "$OUT/obj"; cd "$ROOT"
CXXFLAGS="-std=c++20 -O3 -DNDEBUG -Icore/include -Icore/src -Ithird_party/uat_fec -w $BENCH_CXXFLAGS"
for s in core/src/hfdig/*.cpp core/src/psk/*.cpp core/src/js8/*.cpp core/src/cw/*.cpp core/src/hfdl/*.cpp core/src/ale/*.cpp core/src/codec/codec_paths.cpp tools/bench/bench_hf.cpp; do
  o="$OUT/obj/$(echo "$s" | tr '/' '_').o"
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ -n "$(find core/src/hfdig core/include -name '*.h' -newer "$o" | head -1)" ]; then echo "$s $o"; fi
done | xargs -P "${JOBS:-2}" -n 2 sh -c 'nice -n 19 c++ '"$CXXFLAGS"' -c "$0" -o "$1" || echo "FAILED $0"'
OBJS=$(ls "$OUT"/obj/*.o)
LIBS="-lpthread -lz -lsqlite3 -framework Accelerate -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -framework Metal -framework Foundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework AVFoundation -framework Security -framework SystemConfiguration -framework IOKit -liconv $(pkg-config --libs libavcodec libavutil libswresample libavformat libswscale libusb-1.0 libhackrf hamlib librtlsdr libairspy SoapySDR 2>/dev/null)"
nice -n 19 c++ -o "$OUT/bench_hf" $OBJS $CORE_BUILD/libdect2core.a $(ls $CORE_BUILD/libdect2_*.a | tr '\n' ' ') $CORE_BUILD/libdect2core.a $CORE_BUILD/rust/release/libonair_rs.a $LIBS
[ -n "$NORUN" ] || "$OUT/bench_hf" "$@"
