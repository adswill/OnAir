#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build_sources.sh - build OnAir's own libraries from source for one target and add them to that target's sysroot.
#
# These are the libraries that OnAir's tools/package/cross_deps_windows.sh builds, at the same versions
# (sources/sources.lock). Every target gets the same set, the same versions and the same static/shared split:
#   static (.a), linked into the program:  libusb, OpenSSL, GLFW, libhackrf, FFmpeg (LGPL: checked), librtlsdr,
#                                          libairspy, libxml2
#   shared (DLL / .so / .dylib):           SoapySDR (+ the SoapyRemote module), libiio, libbladeRF, LimeSuite, Hamlib,
#                                          libairspyhf; librtlsdr and libairspy also as shared libraries for OnAir's
#                                          native drivers, which load them by name (Windows: bin/librtlsdr.dll;
#                                          Linux: librtlsdr.so.0 without the librtlsdr.so link; macOS: librtlsdr.0.dylib
#                                          without librtlsdr.dylib, so -lrtlsdr still links the static library)
# The shared libraries carry libusb (and libiio libxml2) inside. The static libraries' .pc files list what they need
# in Libs/Requires (Libs.private and Requires.private are merged in), so a plain pkg-config link works.
#
# Each library is built with the SDK's own compiler and toolchain file for the target (clang 23), installed into a
# staging folder and copied into the sysroot:
#   Windows       <sysroot>/        (bin/ include/ lib/, like the MSYS2 packages; .pc files relocatable)
#   Linux, Pi 5   <sysroot>/usr/local   (prefix /usr/local; pkg-config adds the sysroot, PKG_CONFIG_SYSROOT_DIR)
#   macOS         <sysroot>/usr/local   (next to conda-forge's; install names @rpath/...; .pc files relocatable)
# A file that is already in the sysroot is an error: the downloaded package of the same library has to go (PACKAGES in
# windows/, linux/ and macos/fetch_sysroot.sh). The result is kept in cache/built/<triple>-<key>.tar.zst; the key is the
# sha256 of the source lock file, this script, the patches, the target's sysroot lock file and the toolchain lock file,
# so make_sdk.sh does not build again when nothing changed.
#
# Usage:  build_sources.sh --sdk SDKDIR --target windows|linux|rpi5|macos-arm64|macos-x86_64 [-j N] [--cache DIR]
#                          [--rebuild] [--keep-work]
#   --sdk DIR     the SDK: its native tools, environment-setup-<triple>, toolchain-<triple>.cmake and target sysroot
#                 (make_sdk.sh calls this script after it made the target sysroot)
#   --cache DIR   default: cache/ in the repository (cache/src: the source archives, cache/built: the results)
#   --rebuild     build even when cache/built has the result
#   --keep-work   keep the build folder (cache/work/<triple>) for looking at a failed build
# Needs on the build machine: cmake ninja-build make perl nasm patch unzip xz-utils (and libwayland-bin for GLFW's
# wayland-scanner on Linux).
set -Eeuo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
SDK="" TARGET="" CACHE="$ROOT/cache" JOBS=$(nproc) REBUILD=0 KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) SDK="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --cache) CACHE="$2"; shift 2;;
    -j) JOBS="$2"; shift 2;;
    -j*) JOBS="${1#-j}"; shift;;
    --rebuild) REBUILD=1; shift;;
    --keep-work) KEEP=1; shift;;
    -h|--help) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0;;
    *) echo "unknown argument $1 (see --help)" >&2; exit 2;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
trap '[ "$BASH_SUBSHELL" != 0 ] || echo "error: build_sources.sh line $LINENO: $BASH_COMMAND" >&2' ERR
say() { echo "==> $*"; }
[ -n "$SDK" ] && [ -d "$SDK" ] || die "--sdk SDKDIR (the SDK make_sdk.sh is making)"
SDK="$(cd "$SDK" && pwd)"
for t in cmake ninja make perl nasm patch unzip xz tar zstd curl sha256sum; do command -v $t >/dev/null || die "$t not found (apt install cmake ninja-build make perl nasm patch unzip xz-utils zstd curl)"; done

# --- the target ---------------------------------------------------------------------------------------------------------
case "$TARGET" in
  windows)      TRIPLE=x86_64-w64-mingw32  OS=windows HOSTT=x86_64-w64-mingw32 CPU=x86_64  OSSL=mingw64            LOCK=windows/sysroot.lock;;
  linux)        TRIPLE=x86_64-linux-gnu    OS=linux   HOSTT=x86_64-linux-gnu   CPU=x86_64  OSSL=linux-x86_64       LOCK=linux/sysroot.lock;;
  rpi5)         TRIPLE=aarch64-linux-gnu   OS=linux   HOSTT=aarch64-linux-gnu  CPU=aarch64 OSSL=linux-aarch64      LOCK=linux/sysroot-arm64.lock;;
  macos-arm64)  TRIPLE=arm64-apple-darwin  OS=macos   HOSTT=aarch64-apple-darwin CPU=aarch64 OSSL=darwin64-arm64-cc LOCK=macos/sysroot-arm64.lock;;
  macos-x86_64) TRIPLE=x86_64-apple-darwin OS=macos   HOSTT=x86_64-apple-darwin  CPU=x86_64  OSSL=darwin64-x86_64-cc LOCK=macos/sysroot-x86_64.lock;;
  *) die "--target windows, linux, rpi5, macos-arm64 or macos-x86_64";;
esac
SYSROOT="$SDK/sysroots/$TRIPLE" ENVSETUP="$SDK/environment-setup-$TRIPLE" TC="$SDK/toolchain-$TRIPLE.cmake"
[ -f "$SYSROOT/SYSROOT-MANIFEST.txt" ] || die "$SYSROOT is not a target sysroot of this SDK (no SYSROOT-MANIFEST.txt)"
[ -f "$ENVSETUP" ] && [ -f "$TC" ] || die "$SDK has no environment-setup-$TRIPLE or toolchain-$TRIPLE.cmake"
if [ "$OS" = windows ]; then LPFX=/ucrt64 FINAL="$SYSROOT"; else LPFX=/usr/local FINAL="$SYSROOT/usr/local"; fi
# LPFX: the prefix the libraries are configured for (what their .pc and CMake files name); FINAL: where it is in the sysroot

# --- the sources --------------------------------------------------------------------------------------------------------
SRCLOCK="$SCRIPT_DIR/sources.lock"
declare -A S_VER S_FILE S_URL S_SHA
while read -r kind n v f u s; do
  [ "$kind" = src ] || continue
  S_VER[$n]=$v S_FILE[$n]=$f S_URL[$n]=$u S_SHA[$n]=$s
done < "$SRCLOCK"
mkdir -p "$CACHE/src"
for n in "${!S_FILE[@]}"; do
  f="$CACHE/src/${S_FILE[$n]}"
  if [ ! -f "$f" ]; then
    say "downloading ${S_FILE[$n]}"
    curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$f.part" "${S_URL[$n]}" || { rm -f "$f.part"; die "download failed: ${S_URL[$n]}"; }
    mv "$f.part" "$f"
  fi
  [ "$(sha256sum "$f" | cut -d' ' -f1)" = "${S_SHA[$n]}" ] || die "$f: sha256 is not the one in $SRCLOCK"
done

KEY=$(cat "$SRCLOCK" "$0" "$SCRIPT_DIR"/patches/*.patch "$ROOT/$LOCK" "$ROOT/common/toolchain.lock" | sha256sum | cut -c1-16)
BUILT="$CACHE/built/$TRIPLE-$KEY.tar.zst"

write_manifest() {
  {
    echo "Libraries built from source by sources/build_sources.sh (key $KEY), installed in ${FINAL#"$SYSROOT"}/ (prefix $LPFX)"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    for n in libusb openssl glfw hackrf ffmpeg rtlsdr airspy soapysdr soapyremote libxml2 libiio bladerf limesuite hamlib airspyhf; do
      echo "$n ${S_VER[$n]} ${S_FILE[$n]} ${S_SHA[$n]}"
    done
  } > "$SYSROOT/SOURCES-MANIFEST.txt"
}

check_free() {   # every file of a tar (stdin: the list) must be new in FINAL: else a downloaded package still has it
  local clash
  clash=$(while IFS= read -r p; do p=${p#./}; [ -z "$p" ] || [ ! -e "$FINAL/$p" ] && [ ! -L "$FINAL/$p" ] || echo "$p"; done | grep -v '/$' || true)
  [ -z "$clash" ] || die "already in the sysroot (remove that library's package from fetch_sysroot.sh's PACKAGES): $(head -5 <<<"$clash" | tr '\n' ' ')"
}

# what an earlier run of this script put into the sysroot (its file list) goes first
RECORD="$SYSROOT/SOURCES-FILES.txt"
if [ -f "$RECORD" ]; then
  while IFS= read -r p; do [ -n "$p" ] && rm -f "$FINAL/$p"; done < "$RECORD"
  rm -f "$RECORD" "$SYSROOT/SOURCES-MANIFEST.txt"
fi

if [ "$REBUILD" = 0 ] && [ -f "$BUILT" ]; then
  say "$TRIPLE: the libraries built from source, from $BUILT"
  tar -I zstd -tf "$BUILT" | grep -v '/$' | check_free
  tar -I zstd -tf "$BUILT" | grep -v '/$' > "$RECORD"
  mkdir -p "$FINAL"; tar -I zstd -xf "$BUILT" -C "$FINAL"
  write_manifest
  exit 0
fi

# --- the build environment: the SDK's, for this target ------------------------------------------------------------------
# shellcheck disable=SC1090
. "$ENVSETUP" >/dev/null
unset CMAKE_TOOLCHAIN_FILE   # given on each cmake command line instead
WORK="$CACHE/work/$TRIPLE"
rm -rf "$WORK"; mkdir -p "$WORK/src" "$WORK/build" "$WORK/log"
DEST="$WORK/dest" FILES="$WORK/files"
# the sources are not git checkouts: git must not find this repository above them (SoapySDR, bladeRF ... would put its
# commit into their version strings)
export GIT_CEILING_DIRECTORIES="$WORK/src"
: > "$FILES"
[ "$KEEP" = 1 ] || trap 'rm -rf "$WORK"' EXIT

BASE_CFLAGS="-O2" BASE_LDFLAGS=""
case "$OS" in
  linux) BASE_CFLAGS="-O2 -fPIC" BASE_LDFLAGS="-Wl,--exclude-libs,libusb-1.0.a:libxml2.a";;   # libusb and libxml2 go into shared libraries: PIC, and their symbols kept private
  macos) BASE_CFLAGS="-O2 -fPIC" BASE_LDFLAGS="-framework IOKit -framework CoreFoundation -framework Security";;   # static libusb's
esac
CMAKE_COMMON=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  "-DCMAKE_INSTALL_PREFIX=$LPFX" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_SKIP_BUILD_RPATH=ON
  -DCMAKE_INSTALL_RPATH= "-DCMAKE_PREFIX_PATH=$FINAL")
[ "$OS" = macos ] && CMAKE_COMMON+=(-DCMAKE_INSTALL_NAME_DIR=@rpath -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON -DCMAKE_MACOSX_RPATH=ON)
if [ "$OS" = windows ]; then
  # some projects run ${CMAKE_RC_COMPILER} themselves, without CMAKE_RC_FLAGS: a windres that knows the target and sysroot
  mkdir -p "$WORK/bin"
  printf '#!/bin/sh\nexec llvm-windres --target=pe-x86-64 "--preprocessor-arg=--sysroot=%s" "$@"\n' "$SYSROOT" > "$WORK/bin/$TRIPLE-windres"
  chmod +x "$WORK/bin/$TRIPLE-windres"
  CMAKE_COMMON+=("-DCMAKE_RC_COMPILER=$WORK/bin/$TRIPLE-windres")
fi
CONF_COMMON=(--host="$HOSTT" --build=x86_64-pc-linux-gnu --prefix="$LPFX")
USB_INC="$FINAL/include/libusb-1.0" USB_LIB="$FINAL/lib/libusb-1.0.a"
# FFmpeg's compiler for the build machine (it only probes it: its host programs are test tools, not built here): the
# SDK's Linux clang if the SDK has the Linux target, else the machine's cc, else the target's
if [ -x "$SDK/sysroots/x86_64-sdk-linux/bin/x86_64-linux-gnu-clang" ] && [ -d "$SDK/sysroots/x86_64-linux-gnu/usr/include" ]; then
  HOSTCC=x86_64-linux-gnu-clang
else
  HOSTCC=$(command -v cc || command -v gcc || echo "$CC")
fi

# run "<log name>" <command...>: the output goes to the log, its end is shown when it fails
run() {
  local log="$WORK/log/$1.log"; shift
  echo "+ $*" >> "$log"
  "$@" >> "$log" 2>&1 || { echo "---- the end of $log:" >&2; tail -40 "$log" >&2; die "failed: $*"; }
}
# unpack <name> <dir>: a fresh copy of the source in $WORK/src/<dir>
unpack() {
  local f="$CACHE/src/${S_FILE[$1]}"
  rm -rf "${WORK:?}/src/$2"; mkdir -p "$WORK/src/$2"
  case "$f" in
    *.zip) (cd "$WORK/src" && unzip -q "$f" && [ -d "$2" ]) || die "$f did not unpack into $2";;
    *) tar -xf "$f" -C "$WORK/src/$2" --strip-components=1;;
  esac
  [ "$OS" = windows ] && win_include_case "$WORK/src/$2"
  return 0
}
# Windows: #include <Windows.h>, <Winsock2.h> ... work on Windows' and macOS's file systems, not on Linux (mingw-w64's
# headers are lowercase): such includes become the lowercase header when only that one is in the sysroot
win_include_case() {
  local h low fixed=0
  while IFS= read -r h; do
    low=$(tr 'A-Z' 'a-z' <<<"$h")
    [ ! -e "$SYSROOT/include/$h" ] && [ -e "$SYSROOT/include/$low" ] || continue
    grep -rlE --include='*.[ch]' --include='*.[ch]pp' --include='*.cc' --include='*.cxx' --include='*.hh' "#[[:space:]]*include[[:space:]]*<$h>" "$1" \
      | xargs -r sed -i -E "s#(\#[[:space:]]*include[[:space:]]*<)$h>#\1$low>#" || true
    fixed=$((fixed + 1))
  done < <(grep -rhoE --include='*.[ch]' --include='*.[ch]pp' --include='*.cc' --include='*.cxx' --include='*.hh' '#[[:space:]]*include[[:space:]]*<[^>]*[A-Z][^>]*>' "$1" 2>/dev/null \
             | sed -E 's/.*<([^>]*)>/\1/' | sort -u)
  [ "$fixed" = 0 ] || echo "    ($fixed Windows header names made lowercase)"
}
step() { echo; say "$TRIPLE: $1 ${S_VER[$1]}"; rm -rf "$DEST"; mkdir -p "$DEST$LPFX"; }
# license <name> <files...>: the library's licence texts into share/licenses/<name>/
license() { local n=$1; shift; mkdir -p "$DEST$LPFX/share/licenses/$n"; cp "$@" "$DEST$LPFX/share/licenses/$n/"; }
# pc_static <file.pc>: merge Libs.private / Requires.private into Libs / Requires (a static library needs them)
pc_static() {
  awk '
    /^Libs\.private:/     { lp = lp " " substr($0, 15); next }
    /^Requires\.private:/ { rp = rp (rp == "" ? "" : ", ") substr($0, 19); next }
    { line[++n] = $0 }
    END {
      gsub(/^ +| +$/, "", rp)
      for (i = 1; i <= n; i++) {
        l = line[i]
        if (l ~ /^Libs:/) l = l lp
        if (l ~ /^Requires:/ && rp != "") { l = l (l ~ /^Requires: *$/ ? " " : ", ") rp; done = 1 }
        print l
      }
      if (!done && rp != "") print "Requires: " rp
    }' "$1" > "$1.new" && mv "$1.new" "$1"
}
# write_pc <name> <version> <description> <Libs> <Cflags> [Requires]: a .pc file for a library installed by hand
write_pc() {
  mkdir -p "$DEST$LPFX/lib/pkgconfig"
  {
    echo "prefix=$LPFX"; echo 'libdir=${prefix}/lib'; echo 'includedir=${prefix}/include'
    echo "Name: $1"; echo "Description: $3"; echo "Version: $2"
    echo "Libs: -L\${libdir} $4"; echo "Cflags: $5"
    [ -z "${6:-}" ] || echo "Requires: $6"
  } > "$DEST$LPFX/lib/pkgconfig/$1.pc"
}

# merge <name> static|shared: tidy the staged install, make it relocatable, check it is new in the sysroot, copy it in
merge() {
  local name=$1 kind=$2 d="$DEST$LPFX" pc f rel up
  [ -d "$d" ] || die "$name installed nothing into $DEST$LPFX"
  local stray; stray=$(cd "$DEST" && find . -mindepth 1 -maxdepth 3 -type d | grep -v "^\.$LPFX" | grep -vE "^\.(/usr|/ucrt64)$" || true)
  [ -z "$stray" ] || echo "    (left out, outside $LPFX: $(tr '\n' ' ' <<<"$stray"))"
  rm -rf "${d:?}/share/doc" "${d:?}/share/man" "${d:?}/share/info" "${d:?}/share/gtk-doc" "${d:?}/share/aclocal" "${d:?}/libexec" "${d:?}/etc"
  find "$d" -name '*.la' -delete
  if [ -d "$d/bin" ]; then   # programs: none (Windows: only the DLLs stay in bin/)
    if [ "$OS" = windows ]; then find "$d/bin" -type f ! -iname '*.dll' -delete; else rm -rf "${d:?}/bin"; fi
  fi
  for pc in "$d"/lib/pkgconfig/*.pc "$d"/share/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    [ "$kind" = static ] && pc_static "$pc"
    # paths of this build machine that pkg-config handed to the build (zlib's -L for FFmpeg ...): the sysroot's own
    if [ "$OS" = linux ]; then sed -i -E "s#(-[IL])$SYSROOT/#\\1/#g" "$pc"
    else sed -i -E "s#(-[IL])$FINAL/lib/pkgconfig/\\.\\./\\.\\.#\\1\${prefix}#g; s#(-[IL])$FINAL(/|\$|[[:space:]])#\\1\${prefix}\\2#g" "$pc"; fi
    grep -q -- "$WORK\|$SDK" "$pc" && die "$pc names a folder of this build machine: $(grep -- "$WORK\|$SDK" "$pc")"
    if [ "$OS" != linux ]; then   # relocatable: the prefix is two folders above the .pc file
      sed -i -E -e "/^prefix=/!s#$LPFX(/|\$|[[:space:]\"])#\${prefix}\\1#g" -e 's#^prefix=.*#prefix=${pcfiledir}/../..#' "$pc"
    fi
    [ "$OS" = macos ] && sed -i -E 's#-framework[[:space:]]+([A-Za-z0-9_]+)#-Wl,-framework,\1#g' "$pc"   # one word for CMake
    sed -i -E 's#[[:space:]]+$##' "$pc"
  done
  # CMake package files that name the prefix: relative to the file instead
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    rel=$(dirname "${f#"$d"/}"); up=$(sed -E 's#[^/]+#..#g' <<<"$rel")
    sed -i -E "s#(^|[[:space:]=\"(;])$LPFX(/|\$|[[:space:]\")])#\\1\${CMAKE_CURRENT_LIST_DIR}/$up\\2#g" "$f"
  done < <(grep -rlE "(^|[[:space:]=\"(;])$LPFX(/|\$|[[:space:]\")])" "$d/lib/cmake" "$d/share/cmake" "$d/cmake" "$d"/share/*/cmake "$d"/lib/*/cmake --include='*.cmake' 2>/dev/null | sort -u || true)
  (cd "$d" && find . -mindepth 1 \( -type f -o -type l \) | sed 's#^\./##' | sort) > "$WORK/files.$name"
  check_free < "$WORK/files.$name"
  cat "$WORK/files.$name" >> "$FILES"; cat "$WORK/files.$name" >> "$RECORD"
  cp -a "$d/." "$FINAL/"
  rm -rf "$DEST"
  say "  $(wc -l < "$WORK/files.$name") files"
}

# cmake_build <log> <src> <build> [cmake arguments ...]: configure (+ CFLAGS/LDFLAGS from the environment)
cmake_configure() { local log=$1 src=$2 b=$3; shift 3; run "$log" cmake -S "$src" -B "$b" "${CMAKE_COMMON[@]}" "$@"; }

export CFLAGS="$BASE_CFLAGS" CXXFLAGS="$BASE_CFLAGS" LDFLAGS="$BASE_LDFLAGS"
S="$WORK/src" B="$WORK/build"

# ---- libusb (static) ---------------------------------------------------------------------------------------------------
step libusb; unpack libusb libusb
opts=(--enable-static --disable-shared); [ "$OS" = linux ] && opts+=(--disable-udev)   # no libudev: netlink hotplug
(cd "$S/libusb" && run libusb ./configure "${CONF_COMMON[@]}" "${opts[@]}" && run libusb make -j"$JOBS" && run libusb make install DESTDIR="$DEST")
license libusb "$S/libusb/COPYING"
merge libusb static

# ---- OpenSSL (static; Apache-2.0) --------------------------------------------------------------------------------------
step openssl; unpack openssl openssl
case "$OS" in
  windows) OSSLDIR=C:/OnAir-no-openssl-dir;;   # (cross_deps_windows.sh: the programs read the Windows root store)
  linux)   OSSLDIR=/usr/lib/ssl;;              # Ubuntu's (certificates in /etc/ssl through it)
  macos)   OSSLDIR=/etc/ssl;;                  # macOS has /etc/ssl/cert.pem
esac
(cd "$S/openssl" && run openssl ./Configure "$OSSL" no-shared no-tests no-docs no-apps no-module --prefix="$LPFX" --libdir=lib --openssldir="$OSSLDIR" \
    "CC=$CC" "AR=$AR" "RANLIB=$RANLIB" "CFLAGS=$BASE_CFLAGS" "LDFLAGS=" \
  && run openssl make -j"$JOBS" build_libs && run openssl make install_dev DESTDIR="$DEST")
license openssl "$S/openssl/LICENSE.txt"; [ -f "$S/openssl/NOTICE.md" ] && license openssl "$S/openssl/NOTICE.md"
merge openssl static

# ---- GLFW (static) -----------------------------------------------------------------------------------------------------
step glfw; unpack glfw glfw-3.4
cmake_configure glfw "$S/glfw-3.4" "$B/glfw" -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF
run glfw cmake --build "$B/glfw" -j"$JOBS"; run glfw env DESTDIR="$DEST" cmake --install "$B/glfw"
license glfw "$S/glfw-3.4/LICENSE.md"
merge glfw static

# ---- libhackrf (static) ------------------------------------------------------------------------------------------------
step hackrf; unpack hackrf hackrf
cmake_configure hackrf "$S/hackrf/host/libhackrf" "$B/hackrf" "-DLIBUSB_INCLUDE_DIR=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB" -DINSTALL_UDEV_RULES=OFF \
  "-DCMAKE_C_FLAGS=$BASE_CFLAGS -DLIBHACKRF_STATIC"
run hackrf cmake --build "$B/hackrf" -j"$JOBS" --target hackrf-static
mkdir -p "$DEST$LPFX/lib" "$DEST$LPFX/include/libhackrf"
cp "$(find "$B/hackrf" -name 'libhackrf*.a' -print -quit)" "$DEST$LPFX/lib/libhackrf.a"
cp "$S/hackrf/host/libhackrf/src/hackrf.h" "$DEST$LPFX/include/libhackrf/"
hl="-lhackrf"; [ "$OS" = windows ] && hl="-lhackrf -lsetupapi -lole32"
write_pc libhackrf "${S_VER[hackrf]}" "HackRF library (static)" "$hl" '-I${includedir}/libhackrf -DLIBHACKRF_STATIC' libusb-1.0
license hackrf "$S/hackrf/COPYING"
merge hackrf static

# ---- FFmpeg (static, LGPL) ---------------------------------------------------------------------------------------------
step ffmpeg; unpack ffmpeg ffmpeg
case "$OS" in windows) FFOS=mingw32;; linux) FFOS=linux;; macos) FFOS=darwin;; esac
ff=(--prefix="$LPFX" --enable-cross-compile --target-os=$FFOS --arch=$CPU --cc="$CC" --cxx="$CXX" --objcc="$CC" --ar="$AR" --nm="$NM"
    --ranlib="$RANLIB" --strip="$STRIP" --host-cc="$HOSTCC" --x86asmexe=nasm --pkg-config=pkg-config
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug)
# As OnAir's release builds (tools/package/cross_deps_windows.sh, build_ffmpeg_static.sh): the network with FFmpeg's own protocols
# (no TLS library) for the video generator's RTMP server input (core/src/tv/videogen_rtmp.cpp); on Windows libavdevice with only
# the DirectShow input, for the cameras and capture cards (core/src/audio/videoin_ffmpeg.cpp); no libavdevice on Linux and macOS.
if [ "$OS" = windows ]; then ff+=(--disable-indevs --enable-indev=dshow --disable-outdevs); else ff+=(--disable-avdevice --enable-pic); fi
mkdir -p "$B/ffmpeg"
(cd "$B/ffmpeg" && run ffmpeg "$S/ffmpeg/configure" "${ff[@]}" --extra-cflags="$BASE_CFLAGS" --extra-ldflags="$BASE_LDFLAGS" && run ffmpeg make -j"$JOBS")
for c in GPL VERSION3 NONFREE; do
  grep -qE "^#define CONFIG_$c 0" "$B/ffmpeg/config.h" || die "FFmpeg is not the LGPL build (CONFIG_$c is not 0): GPL, version3 and nonfree are not allowed"
done
want_ff=(RTMP_PROTOCOL TCP_PROTOCOL FLV_DEMUXER AC3_ENCODER); [ "$OS" = windows ] && want_ff+=(DSHOW_INDEV)
for c in "${want_ff[@]}"; do
  grep -qE "^#define CONFIG_$c 1" "$B/ffmpeg/config_components.h" || die "FFmpeg has no CONFIG_$c (OnAir needs it: RTMP input, AC-3 encoder, Windows cameras)"
done
(cd "$B/ffmpeg" && run ffmpeg make install DESTDIR="$DEST")
license ffmpeg "$S/ffmpeg/LICENSE.md" "$S/ffmpeg/COPYING.LGPLv2.1"
grep -E '^(#define CONFIG_(GPL|VERSION3|NONFREE) |#define FFMPEG_CONFIGURATION )' "$B/ffmpeg/config.h" > "$DEST$LPFX/share/licenses/ffmpeg/CONFIGURATION.txt"
merge ffmpeg static

# ---- librtlsdr (RTL-SDR Blog 1.4.0): static, and shared for the native driver ------------------------------------------
step rtlsdr; unpack rtlsdr rtlsdr
sed -i 's/^set(VERSION_INFO_PATCH_VERSION git)/set(VERSION_INFO_PATCH_VERSION 0)/' "$S/rtlsdr/CMakeLists.txt"   # (no git history in the tarball)
detach=OFF; [ "$OS" = linux ] && detach=ON   # Linux: take a dongle over from the kernel's DVB-T driver (as OnAir's Linux packages do)
CFLAGS="$BASE_CFLAGS -std=gnu11" cmake_configure rtlsdr "$S/rtlsdr" "$B/rtlsdr" -DVERSION=0.6.6 -DDETACH_KERNEL_DRIVER=$detach -DINSTALL_UDEV_RULES=OFF \
  "-DLIBUSB_INCLUDE_DIRS=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB"
run rtlsdr cmake --build "$B/rtlsdr" -j"$JOBS" --target rtlsdr_static rtlsdr
mkdir -p "$DEST$LPFX/lib" "$DEST$LPFX/include"
if [ -f "$B/rtlsdr/src/librtlsdr_static.a" ]; then cp "$B/rtlsdr/src/librtlsdr_static.a" "$DEST$LPFX/lib/librtlsdr.a"   # (Windows' name)
else cp "$B/rtlsdr/src/librtlsdr.a" "$DEST$LPFX/lib/librtlsdr.a"; fi
cp "$S/rtlsdr/include/rtl-sdr.h" "$S/rtlsdr/include/rtl-sdr_export.h" "$DEST$LPFX/include/"
# the shared library under its run-time name only (not the -l link name, so -lrtlsdr stays static)
shared_runtime() {   # <build dir> <base name, e.g. librtlsdr> <the name OnAir loads on Linux, e.g. librtlsdr.so.0>
  local b=$1 base=$2 so=$3 real   # (some projects put it into another folder of the build: looked for in all of $B)
  case "$OS" in
    windows) real=$(find "$B" -name "$base.dll" -print -quit); [ -n "$real" ] || die "no $base.dll in $B"
             mkdir -p "$DEST$LPFX/bin"; cp "$real" "$DEST$LPFX/bin/";;
    linux)   real=$(find "$B" -name "$so" -print -quit); [ -n "$real" ] || die "no $so in $B"
             real=$(readlink -f "$real")
             cp "$real" "$DEST$LPFX/lib/"; [ "$(basename "$real")" = "$so" ] || ln -s "$(basename "$real")" "$DEST$LPFX/lib/$so";;
    macos)   real=$(find "$B" -name "$base.*.dylib" -print -quit); [ -n "$real" ] || die "no $base.*.dylib in $B"
             real=$(readlink -f "$real"); cp "$real" "$DEST$LPFX/lib/"
             local id; id=$(llvm-otool -D "$real" | tail -1); id=$(basename "$id")
             [ "$id" = "$(basename "$real")" ] || ln -s "$(basename "$real")" "$DEST$LPFX/lib/$id";;
  esac
}
shared_runtime "$B/rtlsdr/src" librtlsdr librtlsdr.so.0
write_pc librtlsdr 0.6.6 "RTL-SDR library (static; RTL-SDR Blog ${S_VER[rtlsdr]})" -lrtlsdr '-I${includedir} -DRTLSDR_STATIC' libusb-1.0
license rtlsdr "$S/rtlsdr/COPYING"
merge rtlsdr static

# ---- libairspy 1.0.10 (with OnAir's close-order fix): static, and shared for the native driver ---------------------------
step airspy; unpack airspy airspy
patch -s -p1 -d "$S/airspy" < "$SCRIPT_DIR/patches/airspy-1.0.10-close-order.patch"
CFLAGS="$BASE_CFLAGS -std=gnu11" cmake_configure airspy "$S/airspy/libairspy" "$B/airspy" "-DLIBUSB_INCLUDE_DIR=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB" -DINSTALL_UDEV_RULES=OFF
run airspy cmake --build "$B/airspy" -j"$JOBS" --target airspy-static airspy
mkdir -p "$DEST$LPFX/lib" "$DEST$LPFX/include/libairspy"
cp "$B/airspy/src/libairspy.a" "$DEST$LPFX/lib/libairspy.a"
cp "$S/airspy/libairspy/src/airspy.h" "$S/airspy/libairspy/src/airspy_commands.h" "$DEST$LPFX/include/libairspy/"
shared_runtime "$B/airspy/src" libairspy libairspy.so.0
write_pc libairspy "${S_VER[airspy]}" "Airspy library (static)" -lairspy '-I${includedir}/libairspy' libusb-1.0
license airspy "$S/airspy/libairspy/LICENSE.md"
merge airspy static

# ---- SoapySDR (shared) -------------------------------------------------------------------------------------------------
step soapysdr; unpack soapysdr soapysdr
sed -i '0,/^#include/s//#include <iterator> \/\/ std::back_inserter (libc++ does not bring it along)\n#include/' "$S/soapysdr/lib/Factory.cpp"
# SOAPY_SDR_EXTVER: its version suffix (otherwise SoapySDR's CMake walks up to this repository's .git for a commit)
cmake_configure soapysdr "$S/soapysdr" "$B/soapysdr" -DSOAPY_SDR_EXTVER=onair -DENABLE_TESTS=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON=OFF -DENABLE_PYTHON3=OFF -DENABLE_DOCS=OFF
run soapysdr cmake --build "$B/soapysdr" -j"$JOBS"; run soapysdr env DESTDIR="$DEST" cmake --install "$B/soapysdr"
license soapysdr "$S/soapysdr/LICENSE_1_0.txt"
merge soapysdr shared

# ---- SoapyRemote client module (shared; BSL-1.0) -----------------------------------------------------------------------
step soapyremote; unpack soapyremote soapyremote
[ "$OS" = windows ] && patch -s -p1 -d "$S/soapyremote" < "$SCRIPT_DIR/patches/soapyremote-0.5.3-mingw.patch"   # (MSVC-only sources otherwise)
sed -i '0,/^#include/s//#include <cstdlib> \/\/ malloc, free (libc++ does not bring them along)\n#include/' "$S/soapyremote/common/SoapyURLUtils.cpp"
SOAPY_DIR=$(dirname "$(find "$FINAL/lib/cmake" "$FINAL/share/cmake" "$FINAL/cmake" -name SoapySDRConfig.cmake 2>/dev/null -print -quit)")
[ -f "$SOAPY_DIR/SoapySDRConfig.cmake" ] || die "no SoapySDRConfig.cmake in $FINAL"
extra=(); [ "$OS" = windows ] && extra+=("-DCMAKE_CXX_STANDARD_LIBRARIES=-liphlpapi -lws2_32 -lpthread")
cmake_configure soapyremote "$S/soapyremote" "$B/soapyremote" -DSOAPY_REMOTE_EXTVER=onair -DENABLE_AVAHI=OFF -DENABLE_APPS=OFF "-DSoapySDR_DIR=$SOAPY_DIR" "${extra[@]}"
run soapyremote cmake --build "$B/soapyremote" -j"$JOBS"; run soapyremote env DESTDIR="$DEST" cmake --install "$B/soapyremote"
license soapyremote "$S/soapyremote/LICENSE_1_0.txt"
merge soapyremote shared

# ---- libxml2 (static; for libiio) --------------------------------------------------------------------------------------
step libxml2; unpack libxml2 libxml2
cmake_configure libxml2 "$S/libxml2" "$B/libxml2" -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF \
  -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF
run libxml2 cmake --build "$B/libxml2" -j"$JOBS"; run libxml2 env DESTDIR="$DEST" cmake --install "$B/libxml2"
for pc in "$DEST$LPFX"/lib/pkgconfig/libxml-2.0.pc; do
  grep -q -- '-DLIBXML_STATIC' "$pc" || sed -i -E 's#^(Cflags:.*)$#\1 -DLIBXML_STATIC#' "$pc"
done
license libxml2 "$S/libxml2/Copyright"
merge libxml2 static

# ---- libiio 0.25 (shared; USB and network backends, libxml2 inside) ----------------------------------------------------
step libiio; unpack libiio libiio
dnssd=ON; [ "$OS" = linux ] && dnssd=OFF   # (Linux: DNS-SD would need Avahi; Windows and macOS have their own)
xmllibs="$FINAL/lib/libxml2.a"; [ "$OS" = windows ] || xmllibs="$xmllibs;m"
CFLAGS="$BASE_CFLAGS -DLIBXML_STATIC" cmake_configure libiio "$S/libiio" "$B/libiio" -DWITH_USB_BACKEND=ON -DWITH_NETWORK_BACKEND=ON -DWITH_XML_BACKEND=ON \
  -DHAVE_DNS_SD=$dnssd -DWITH_LOCAL_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DWITH_ZSTD=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DENABLE_PACKAGING=OFF \
  -DINSTALL_UDEV_RULE=OFF -DCPP_BINDINGS=OFF -DPYTHON_BINDINGS=OFF -DWITH_IIOD=OFF -DBUILD_SHARED_LIBS=ON -DOSX_PACKAGE=OFF -DOSX_FRAMEWORK=OFF \
  -DWITH_DOC=OFF "-DLIBUSB_INCLUDE_DIR=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB" "-DLIBXML2_LIBRARIES=$xmllibs" "-DLIBXML2_INCLUDE_DIR=$FINAL/include/libxml2"
run libiio cmake --build "$B/libiio" -j"$JOBS"; run libiio env DESTDIR="$DEST" cmake --install "$B/libiio"
license libiio "$S/libiio/COPYING.txt"
merge libiio shared

# ---- libbladeRF 2023.02 (shared; libusb inside) ------------------------------------------------------------------------
step bladerf; unpack bladerf bladeRF
mkdir -p "$S/bladeRF/thirdparty/analogdevicesinc/no-OS"
tar -xf "$CACHE/src/${S_FILE[no-os]}" -C "$S/bladeRF/thirdparty/analogdevicesinc/no-OS" --strip-components=1   # (a git submodule)
find "$S/bladeRF/host" \( -name CMakeLists.txt -o -name '*.cmake' \) -exec sed -i 's/-Werror//g' {} +   # (a newer compiler warns about more)
CFLAGS="$BASE_CFLAGS -std=gnu11" cmake_configure bladerf "$S/bladeRF/host" "$B/bladerf" -DENABLE_BACKEND_LIBUSB=ON -DENABLE_BACKEND_CYPRESS=OFF \
  -DBUILD_DOCUMENTATION=OFF -DENABLE_FX3_BUILD=OFF -DENABLE_HOST_BUILD=ON -DBUILD_BLADERF_CLI=OFF -DENABLE_UDEV_RULES=OFF -DINSTALL_UDEV_RULES=OFF \
  -DBUILD_NATIVE=OFF "-DLIBUSB_PATH=$FINAL" "-DLIBUSB_INCLUDE_DIRS=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB"
run bladerf cmake --build "$B/bladerf" -j"$JOBS" --target libbladerf_shared
mkdir -p "$DEST$LPFX/lib/pkgconfig" "$DEST$LPFX/include"
case "$OS" in
  windows) mkdir -p "$DEST$LPFX/bin"; cp "$B/bladerf/output/libbladeRF.dll" "$DEST$LPFX/bin/"
           imp=$(find "$B/bladerf" -name 'libbladeRF.dll.a' -print -quit); [ -z "$imp" ] || cp "$imp" "$DEST$LPFX/lib/";;
  *)       find "$B/bladerf/output" -maxdepth 1 \( -name 'libbladeRF.so*' -o -name 'libbladeRF*.dylib' \) -exec cp -P {} "$DEST$LPFX/lib/" \;;;
esac
[ -n "$(find "$DEST$LPFX" -iname 'libbladerf*' \( -name '*.dll' -o -name '*.so*' -o -name '*.dylib' \) -print -quit)" ] || die "libbladeRF: no shared library in $B/bladerf/output"
cp "$S/bladeRF/host/libraries/libbladeRF/include"/*.h "$DEST$LPFX/include/"
pcf=$(find "$B/bladerf" -name libbladeRF.pc -print -quit)
if [ -n "$pcf" ]; then cp "$pcf" "$DEST$LPFX/lib/pkgconfig/"; sed -i -E "s#^prefix=.*#prefix=$LPFX#; s#^libdir=.*#libdir=\${prefix}/lib#; s#^includedir=.*#includedir=\${prefix}/include#" "$DEST$LPFX/lib/pkgconfig/libbladeRF.pc"
else write_pc libbladeRF "${S_VER[bladerf]}" "bladeRF library" -lbladeRF '-I${includedir}'; fi
license bladerf "$S/bladeRF/COPYING" "$S/bladeRF/legal/licenses/LICENSE.LGPLv2.1"
merge bladerf shared

# ---- LimeSuite 23.11.0 (shared): the library only ----------------------------------------------------------------------
step limesuite; unpack limesuite LimeSuite
if [ "$OS" = windows ]; then
  for pf in "$SCRIPT_DIR"/patches/limesuite-*.patch; do patch -s -p1 -d "$S/LimeSuite" < "$pf"; done   # (MinGW)
  find "$S/LimeSuite" \( -name CMakeLists.txt -o -name '*.cmake' \) -exec sed -i -E 's/\bSetupAPI(\.lib)?\b/setupapi/g; s/\bWs2_32\.lib\b/ws2_32/g' {} +   # (mingw-w64's names; Linux is case-sensitive)
fi
simd=SSE3; [ "$CPU" = aarch64 ] && simd=none
fx3=ON; [ "$OS" = windows ] && fx3=OFF   # Windows: FX3 needs the MSVC-only Cypress SDK (cross_deps_windows.sh); elsewhere it is libusb
CFLAGS="$BASE_CFLAGS -std=gnu11" cmake_configure limesuite "$S/LimeSuite" "$B/limesuite" -DBUILD_SHARED_LIBS=ON -DENABLE_SIMD_FLAGS=$simd -DLIME_SUITE_EXTVER=onair \
  -DENABLE_FX3=$fx3 -DFTD3XX_STATIC=OFF -DENABLE_PCIE_XILLYBUS=OFF -DENABLE_LIMERFE=OFF -DENABLE_GUI=OFF -DENABLE_SOAPY_LMS7=OFF -DENABLE_UTILITIES=OFF \
  -DENABLE_EXAMPLES=OFF -DENABLE_LIME_UTIL=OFF -DENABLE_QUICKTEST=OFF -DENABLE_OCTAVE=OFF -DENABLE_DESKTOP=OFF -DENABLE_HEADERS=ON -DENABLE_SIMD_FLAGS=$simd \
  "-DLIBUSB_1_INCLUDE_DIRS=$USB_INC" "-DLIBUSB_1_LIBRARIES=$USB_LIB" "-Dlibusb-1.0_INCLUDE_DIRS=$USB_INC" "-Dlibusb-1.0_LIBRARIES=$USB_LIB"
run limesuite cmake --build "$B/limesuite" -j"$JOBS" --target LimeSuite
run limesuite env DESTDIR="$DEST" cmake --install "$B/limesuite"
if [ "$OS" = windows ]; then
  mkdir -p "$DEST$LPFX/bin"; cp "$S/LimeSuite/src/ConnectionFTDI/FTD3XXLibrary/x64/FTD3XX.dll" "$DEST$LPFX/bin/"
  for t in "$S/LimeSuite/src/ConnectionFTDI/FTD3XXLibrary"/*icen*; do [ -f "$t" ] && license limesuite "$t"; done
fi
license limesuite "$S/LimeSuite/COPYING"
merge limesuite shared

# ---- Hamlib 4.6.2 (shared; CAT rig control) ----------------------------------------------------------------------------
step hamlib; unpack hamlib hamlib
# lib/precise_time.c, Windows branch: assert() without <assert.h>, and its QueryPerformanceFrequency set-up is registered
# with MSVC's __declspec(allocate(".CRT$XCU")), which clang (and GCC) ignore, leaving the frequency 0: a constructor instead
sed -i -E -e 's|^#include <windows.h>$|#include <windows.h>\n#include <assert.h>|' \
  -e 's|^__declspec\(allocate\("\.CRT\$XCU"\)\) void \(__cdecl \*init_pcfreq_\)\(\) = init_pcfreq;$|static void __attribute__((constructor)) init_pcfreq_ctor(void) { init_pcfreq(); }|' \
  "$S/hamlib/lib/precise_time.c"
grep -q 'init_pcfreq_ctor' "$S/hamlib/lib/precise_time.c" || die "hamlib: lib/precise_time.c is not as expected (its Windows timer set-up)"
hl=()   # libtool takes the build machine's /usr/bin/ld otherwise
case "$OS" in
  windows) hl=(LD="$TRIPLE-ld" lt_cv_deplibs_check_method=pass_all);;   # (and libusb.a may go into the DLL)
  linux)   hl=(LD=ld.lld);;
  macos)   hl=(LD=ld64.lld NMEDIT=llvm-nm);;   # not GNU ld: libtool's Darwin way (-dynamiclib, -install_name)
esac
(cd "$S/hamlib" && run hamlib ./configure "${CONF_COMMON[@]}" "${hl[@]}" --enable-shared --disable-static --without-cxx-binding --without-perl-binding \
    --without-python-binding --without-tcl-binding --without-lua-binding --without-readline --without-xml-support --disable-winradio \
  && run hamlib make -j"$JOBS" && run hamlib make install DESTDIR="$DEST")
license hamlib "$S/hamlib/COPYING" "$S/hamlib/COPYING.LIB"
merge hamlib shared

# ---- libairspyhf 1.6.8 (shared; libusb inside) -------------------------------------------------------------------------
step airspyhf; unpack airspyhf airspyhf
CFLAGS="$BASE_CFLAGS -std=gnu11" cmake_configure airspyhf "$S/airspyhf/libairspyhf" "$B/airspyhf" "-DLIBUSB_INCLUDE_DIR=$USB_INC" "-DLIBUSB_LIBRARIES=$USB_LIB" -DINSTALL_UDEV_RULES=OFF
run airspyhf cmake --build "$B/airspyhf" -j"$JOBS"
run airspyhf env DESTDIR="$DEST" cmake --install "$B/airspyhf"
rm -f "$DEST$LPFX/lib/libairspyhf.a" "$DEST$LPFX/bin/libairspyhf.a"   # (the shared library only)
[ "$OS" = windows ] && { mkdir -p "$DEST$LPFX/lib"; cp "$B/airspyhf/src/libairspyhf.dll.a" "$DEST$LPFX/lib/"; }   # (its install has no import library)
license airspyhf "$S/airspyhf/LICENSE"
merge airspyhf shared

# ---- checks: macOS install names (@rpath), no build-machine paths in the shared libraries' load commands ----------------
if [ "$OS" = macos ]; then
  while IFS= read -r f; do
    p="$FINAL/$f"; [ -f "$p" ] && [ ! -L "$p" ] || continue
    id=$(llvm-otool -D "$p" | sed -n 2p)
    case "$id" in @rpath/*) ;; *) llvm-install-name-tool -id "@rpath/$(basename "$id")" "$p";; esac
    llvm-otool -L "$p" | awk 'NR > 1 {print $1}' | while read -r dep; do
      case "$dep" in "$LPFX"/lib/*|"$WORK"/*) llvm-install-name-tool -change "$dep" "@rpath/$(basename "$dep")" "$p";; esac
    done
  done < <(grep -E '\.(dylib|so)$' "$FILES")
  bad=$(while IFS= read -r f; do p="$FINAL/$f"; [ -f "$p" ] && [ ! -L "$p" ] || continue
          llvm-otool -L "$p" | awk 'NR > 1 {print $1}' | grep -vE '^(@rpath/|/usr/lib/|/System/Library/)' | sed "s#^#$f: #"; done < <(grep -E '\.(dylib|so)$' "$FILES") || true)
  [ -z "$bad" ] || die "dylibs loading from outside @rpath and the system: $bad"
  min=$(sed -n 's/^deployment: *macOS \([0-9.]*\).*/\1/p' "$SYSROOT/SYSROOT-MANIFEST.txt")
  python3 -I "$ROOT/macos/macho.py" check "$SYSROOT" "$min" || die "macos/macho.py: the sysroot's dylibs do not pass (deployment target macOS $min)"
elif [ "$OS" = linux ]; then
  bad=$(while IFS= read -r f; do p="$FINAL/$f"; [ -f "$p" ] && [ ! -L "$p" ] || continue
          llvm-readobj --dynamic-table "$p" 2>/dev/null | grep -E 'R(UN)?PATH' | sed "s#^#$f: #"; done < <(grep -E '\.so(\.|$)' "$FILES") || true)
  [ -z "$bad" ] || die "shared libraries with an RPATH/RUNPATH: $bad"
fi

# ---- keep the result ---------------------------------------------------------------------------------------------------
mkdir -p "$CACHE/built"
rm -f "$CACHE/built/$TRIPLE"-*.tar.zst
tar -C "$FINAL" --no-recursion --verbatim-files-from -T "$FILES" -I 'zstd -T0 -10' -cf "$BUILT.part" && mv "$BUILT.part" "$BUILT"
write_manifest
say "$TRIPLE: $(wc -l < "$FILES") files from source, kept in $BUILT"
