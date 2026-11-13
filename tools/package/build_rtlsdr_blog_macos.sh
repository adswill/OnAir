#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Build RTL-SDR Blog librtlsdr 1.4.0 (commit aed0ea1: V4 Lite support, HF tracking filter bypass) as a dylib for bundling into OnAir.app (stock Homebrew librtlsdr cannot drive the V4 HF upconverter).
# Usage: build_rtlsdr_blog_macos.sh <output directory>
set -euo pipefail
OUT=${1:?output directory}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
T=rtl-sdr-blog-1.4.0.tar.gz
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
tarball=
for f in "$REPO/build-windows-deps/src/$T" "$REPO/tools/package/cache/$T"; do [ -f "$f" ] && tarball=$f && break; done
if [ -z "$tarball" ]; then
  tarball=$W/$T
  curl -fsSL --retry 5 --retry-all-errors --retry-delay 10 -o "$tarball" https://github.com/rtlsdrblog/rtl-sdr-blog/archive/aed0ea1.tar.gz
fi
mkdir "$W/src" && tar xf "$tarball" -C "$W/src" --strip-components=1
sed -i '' 's/^set(VERSION_INFO_PATCH_VERSION git)/set(VERSION_INFO_PATCH_VERSION 0)/' "$W/src/CMakeLists.txt"
CMAKE_EXTRA=()
if command -v brew >/dev/null 2>&1; then
  prefix=$(brew --prefix libusb 2>/dev/null || brew --prefix)
  CMAKE_EXTRA+=(-DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_EXE_LINKER_FLAGS="-L$prefix/lib" -DCMAKE_SHARED_LINKER_FLAGS="-L$prefix/lib")
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
cmake -S "$W/src" -B "$W/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$W/inst" -DVERSION=0.6.6 -DDETACH_KERNEL_DRIVER=ON -DINSTALL_UDEV_RULES=OFF "${CMAKE_EXTRA[@]}"
cmake --build "$W/build" -j"$(sysctl -n hw.ncpu)" --target rtlsdr
lib=$(find "$W/build/src" -name 'librtlsdr.*.dylib' -type f | head -1)
[ -n "$lib" ] || { echo "Blog librtlsdr dylib not built"; exit 1; }
mkdir -p "$OUT"
base=$(basename "$lib")
cp "$lib" "$OUT/$base"
for n in librtlsdr.0.dylib librtlsdr.dylib; do [ "$n" = "$base" ] || ln -sf "$base" "$OUT/$n"; done
echo "RTL-SDR Blog librtlsdr -> $OUT/$base"
