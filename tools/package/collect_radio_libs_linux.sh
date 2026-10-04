#!/bin/bash
# Gathers the libraries of the radios OnAir can drive by itself (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP) with everything they need that
# a Linux system does not have anyway, into one folder. The packages ship that folder with the program, so nothing has to be installed next to it.
# Run inside the build container (tools/dev/Dockerfile.package installs the libraries).   collect_radio_libs_linux.sh <output folder>
set -e
OUT=$1
mkdir -p "$OUT"
SKIP='linux-vdso|ld-linux|/libc\.so|/libm\.so|/libdl\.so|/libpthread\.so|/librt\.so|/libutil\.so|/libresolv\.so|/libstdc\+\+|/libgcc_s|/libGL|/libEGL|/libOpenGL|/libX|/libxcb|/libwayland|/libdrm|/libasound|/libpulse|/libudev|/libdbus|/libsystemd|/libusb-1\.0|/libz\.so|/libffi|/libgbm|/libva'
copy_with_links() {   # a library, its symlinks and what the symlinks point to
  local f=$1 dir; dir=$(dirname "$f")
  local real; real=$(readlink -f "$f")
  [ -f "$OUT/$(basename "$real")" ] || cp "$real" "$OUT/"
  [ "$(basename "$f")" != "$(basename "$real")" ] && ln -sf "$(basename "$real")" "$OUT/$(basename "$f")"
  # the other names of the same file (soname links in the same folder)
  for l in "$dir"/$(basename "$real" | sed -E 's/\.so.*/.so/')*; do
    [ -L "$l" ] && [ "$(readlink -f "$l")" = "$real" ] && ln -sf "$(basename "$real")" "$OUT/$(basename "$l")"
  done
  return 0
}
collect() {
  ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r lib; do
    echo "$lib" | grep -Eq "$SKIP" && continue
    [ -e "$OUT/$(basename "$lib")" ] || { copy_with_links "$lib"; collect "$lib"; }
  done
}
for stem in rtlsdr airspy bladeRF iio LimeSuite uhd; do
  for f in /usr/lib/*-linux-gnu/lib$stem.so.* /usr/lib/lib$stem.so.*; do
    [ -e "$f" ] || continue
    copy_with_links "$f"
    collect "$(readlink -f "$f")"
  done
done
# the libraries find each other next to themselves
for l in "$OUT"/*.so*; do [ -L "$l" ] || patchelf --set-rpath '$ORIGIN' "$l" 2>/dev/null || true; done
ls "$OUT" | wc -l
du -sh "$OUT" | cut -f1
