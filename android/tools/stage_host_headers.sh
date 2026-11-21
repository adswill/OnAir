#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# For building libonair.so (build_native.sh) on a Debian/Ubuntu host: copies the FFmpeg, libusb and HackRF headers the core is configured with into one
# folder and writes pkg-config files for them that name only that folder. The distribution's own files add -I/usr/include/x86_64-linux-gnu, which
# puts the host's glibc headers in front of the NDK's and breaks every compile; on macOS (Homebrew) nothing like that happens.
#   android/tools/stage_host_headers.sh <folder>      then: export PKG_CONFIG_LIBDIR=<folder>/lib/pkgconfig
set -eu
STAGE=$(mkdir -p "${1:?usage: stage_host_headers.sh <folder>}" && cd "$1" && pwd)
mkdir -p "$STAGE/include" "$STAGE/lib/pkgconfig"
for m in libavformat libavcodec libavutil libswscale libswresample libusb-1.0 libhackrf; do
  ver=$(pkg-config --modversion "$m")
  dir=$(pkg-config --variable=includedir "$m")
  cflags="-I$STAGE/include"
  case "$m" in
    libusb-1.0|libhackrf) cp -r "$dir/$m" "$STAGE/include/"; cflags="$cflags -I$STAGE/include/$m" ;;   # the sources include <libusb.h>, <hackrf.h>
    *)                    cp -r "$dir/$m" "$STAGE/include/" ;;
  esac
  printf 'Name: %s\nDescription: headers only (stage_host_headers.sh)\nVersion: %s\nCflags: %s\n' "$m" "$ver" "$cflags" > "$STAGE/lib/pkgconfig/$m.pc"
done
echo "staged the headers of FFmpeg, libusb and HackRF in $STAGE"
