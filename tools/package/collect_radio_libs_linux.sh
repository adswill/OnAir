#!/bin/bash
# Gathers the libraries of the radios OnAir can drive by itself (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP) with everything they need that
# a Linux system does not have anyway, into one folder. The packages ship that folder with the program, so nothing has to be installed next to it.
# RTL-SDR is built here from the RTL-SDR Blog fork (1.3.6, the version the Windows build ships) and not taken from the distribution: the
# librtlsdr0 of Ubuntu 22.04 is the osmocom 0.6.0, which cannot drive the RTL-SDR Blog V4.
# Run inside the build container (tools/dev/Dockerfile.package installs the libraries).   collect_radio_libs_linux.sh <output folder>
set -e
OUT=$1
REPO=$(cd "$(dirname "$0")/../.." && pwd)
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
build_rtlsdr_blog() {
  local T=rtl-sdr-blog-1.3.6.tar.gz W; W=$(mktemp -d)
  local tarball=
  for f in "$REPO/build-windows-deps/src/$T" "$REPO/tools/package/cache/$T"; do [ -f "$f" ] && tarball=$f && break; done
  if [ -z "$tarball" ]; then   # a copy on this computer is used when there is one, otherwise it comes from GitHub
    tarball=$W/$T
    curl -fsSL -o "$tarball" https://github.com/rtlsdrblog/rtl-sdr-blog/archive/refs/tags/v1.3.6.tar.gz
  fi
  mkdir "$W/src" && tar xf "$tarball" -C "$W/src" --strip-components=1
  sed -i 's/^set(VERSION_INFO_PATCH_VERSION git)/set(VERSION_INFO_PATCH_VERSION 6)/' "$W/src/CMakeLists.txt"   # (the tarball has no git history to take the version from)
  # DETACH_KERNEL_DRIVER: if the kernel's TV driver still holds a dongle (rules or blacklist not installed yet), the library takes it over
  cmake -S "$W/src" -B "$W/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$W/inst" -DVERSION=0.6.6 -DDETACH_KERNEL_DRIVER=ON -DINSTALL_UDEV_RULES=OFF >"$W/cmake.log" 2>&1 || { tail -20 "$W/cmake.log"; return 1; }
  cmake --build "$W/build" -j"$(nproc)" --target rtlsdr >>"$W/cmake.log" 2>&1 || { tail -20 "$W/cmake.log"; return 1; }
  local lib; lib=$(find "$W/build/src" -name 'librtlsdr.so.*' -type f | head -1)
  [ -n "$lib" ] || { echo "the RTL-SDR Blog library was not built"; return 1; }
  cp "$lib" "$OUT/"
  local base; base=$(basename "$lib")           # librtlsdr.so.0.6.6 (or .so.0): the names the program looks for are links to it
  for n in librtlsdr.so.0 librtlsdr.so; do [ "$n" = "$base" ] || ln -sf "$base" "$OUT/$n"; done
  collect "$OUT/$base"
  rm -rf "$W"
}
build_rtlsdr_blog
for stem in airspy bladeRF iio LimeSuite uhd; do
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
