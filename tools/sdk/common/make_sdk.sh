#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make_sdk.sh - the inner step of make_tarball.sh (CMake: tools/sdk/CMakeLists.txt; OnAir.sdk's build_sdk.sh): make the cross SDK on Linux x86-64, laid out like a Yocto SDK: one set of native tools (clang/lld 23,
# Rust, ...) and a target sysroot per target, each with its environment-setup script and CMake toolchain file.
# Targets: Windows x64, Linux x86-64, Raspberry Pi 5 (Ubuntu 26.04 arm64), macOS arm64 (Apple silicon) and macOS x86_64 (Intel).
#
#   <sdk>/environment-setup-x86_64-w64-mingw32   Windows x64 (UCRT): . it, or -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain-x86_64-w64-mingw32.cmake
#   <sdk>/environment-setup-x86_64-linux-gnu     Linux x86-64 (Ubuntu 26.04): . it, or -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain-x86_64-linux-gnu.cmake
#   <sdk>/environment-setup-aarch64-linux-gnu    Raspberry Pi 5 (Ubuntu 26.04 arm64): . it, or -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain-aarch64-linux-gnu.cmake
#   <sdk>/environment-setup-arm64-apple-darwin   macOS arm64: . it, or -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain-arm64-apple-darwin.cmake
#   <sdk>/environment-setup-x86_64-apple-darwin  macOS x86_64 (Intel): likewise
#   <sdk>/collect_dlls.sh                        Windows: the DLLs a built program needs
#   <sdk>/collect_dylibs.sh                      macOS: the dylibs a built program needs
#   <sdk>/sysroots/x86_64-sdk-linux/             native tools (common/fetch_toolchain.sh; the GNU binutils for mingw-w64 only
#                                                with Windows) and the tools that make the packages, only for the targets'
#                                                platforms: makensis (Windows); mkfs.hfsplus, hfsplus, dmg, rcodesign (macOS)
#                                                (common/build_pkgtools.sh)
#   <sdk>/sysroots/x86_64-w64-mingw32/           Windows target: MSYS2 UCRT64 packages (windows/fetch_sysroot.sh)
#   <sdk>/sysroots/x86_64-linux-gnu/             Linux target: Ubuntu 26.04 packages (linux/fetch_sysroot.sh)
#   <sdk>/sysroots/aarch64-linux-gnu/            Raspberry Pi 5 target: Ubuntu 26.04 arm64 packages (linux/fetch_sysroot.sh --arch arm64)
#   <sdk>/sysroots/{arm64,x86_64}-apple-darwin/  macOS targets: Apple's SDK + conda-forge packages (macos/fetch_sysroot.sh)
#   + in every target sysroot: OnAir's own libraries built from source (sources/build_sources.sh):
#     libusb, OpenSSL, GLFW, libhackrf, FFmpeg (LGPL), librtlsdr, libairspy, libxml2 (static), SoapySDR + SoapyRemote,
#     libiio, libbladeRF, LimeSuite, Hamlib, libairspyhf (shared); the versions of OnAir's cross_deps_windows.sh
#   <sdk>/locks/, SDK-MANIFEST.txt, LICENSE (the scripts' licence)
#   <sdk>/LICENSES.txt                           every part of the SDK with its version, licence and licence text
#   <sdk>/licenses/common-licenses/              the standard licence texts (common/make_licenses.py)
#
# The runtimes and system libraries are binary packages of MSYS2, Ubuntu or conda-forge (Ubuntu 26.04's versions are
# the minimum: common/check_versions.py); OnAir's own libraries are compiled from pinned source archives with the SDK's
# clang. Everything is pinned by the lock files in common/, windows/, linux/, macos/ and sources/; the downloads and
# the built libraries are kept in the cache folder (--cache). The SDK folder can be moved.
#
# Usage:  ./make_sdk.sh [-o SDKDIR] [--target windows|linux|rpi5|macos|macos-arm64|macos-x86_64|all] [--update] [--tar]
#   -o SDKDIR   default: ./sdk next to this script (replaced if it holds SDK-MANIFEST.txt)
#   --target    which target sysroots, comma separated: --target windows,rpi5 (default: all)
#   --update    re-resolve the lock files (newest packages; the versions set at the top of the scripts)
#   --tar       also write SDKDIR.tar.zst, to carry the SDK to other machines
#   --keep-work keep the libraries' build folders in <cache>/work
#   --cache DIR the downloads and the built libraries (default: \$ONAIR_SDK_CACHE, else ./cache next to this script's folder)
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # (tools/sdk of OnAir)
OUT="$SCRIPT_DIR/sdk" TARGET=all UPDATE=() TAR=0 WORK=() CACHE=${ONAIR_SDK_CACHE:-$SCRIPT_DIR/cache}
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --update) UPDATE=(--update); shift;;
    --tar) TAR=1; shift;;
    --keep-work) WORK=(--keep-work); shift;;
    --cache) CACHE="$2"; shift 2;;
    -h|--help) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0;;
    *) echo "unknown argument $1 (see --help)" >&2; exit 2;;
  esac
done
for t in ${TARGET//,/ }; do case "$t" in windows|linux|rpi5|macos|macos-arm64|macos-x86_64|all) ;; *) echo "--target windows, linux, rpi5, macos, macos-arm64, macos-x86_64 or all, comma separated" >&2; exit 2;; esac; done
want() { local t; for t in ${TARGET//,/ }; do if [ "$t" = all ] || [ "$t" = "$1" ] || { [ "$t" = macos ] && [ "${1%-*}" = macos ]; }; then return 0; fi; done; return 1; }
say() { echo; echo "=== $*"; }

if [ -e "$OUT" ]; then
  # (.made-by-make_sdk: a run that failed halfway has no manifest yet, and must not stop the next run)
  if [ -f "$OUT/SDK-MANIFEST.txt" ] || [ -f "$OUT/.made-by-make_sdk" ] || [ -z "$(ls -A "$OUT")" ]; then rm -rf "$OUT"
  else echo "error: $OUT exists and was not made by this script: remove it or choose another -o" >&2; exit 1; fi
fi
mkdir -p "$OUT/sysroots" "$OUT/locks"; OUT="$(cd "$OUT" && pwd)"
: > "$OUT/.made-by-make_sdk"
mkdir -p "$CACHE"; CACHE="$(cd "$CACHE" && pwd)"

TARGETS=() PLATFORMS=()   # the targets, one by one; the platforms whose packaging tools go in
for t in windows linux rpi5 macos-arm64 macos-x86_64; do want $t && TARGETS+=($t); done
want windows && PLATFORMS+=(windows); { want linux || want rpi5; } && PLATFORMS+=(linux); { want macos-arm64 || want macos-x86_64; } && PLATFORMS+=(macos)
MINGW=(); want windows || MINGW=(--without-mingw)

say "native tools"
"$SCRIPT_DIR/common/fetch_toolchain.sh" -o "$OUT/sysroots/x86_64-sdk-linux" --cache "$CACHE" "${UPDATE[@]}" "${MINGW[@]}"
install -m 644 "$SCRIPT_DIR/common/toolchain.lock" "$SCRIPT_DIR/../../LICENSE" "$OUT/"; mv "$OUT/toolchain.lock" "$OUT/locks/"   # (LICENSE: OnAir's, GPL-3.0, the scripts' licence)
install -m 644 "$SCRIPT_DIR/sources/sources.lock" "$OUT/locks/sources.lock"
say "packaging tools"
"$SCRIPT_DIR/common/build_pkgtools.sh" -o "$OUT/sysroots/x86_64-sdk-linux" --cache "$CACHE" --platform "$(IFS=,; echo "${PLATFORMS[*]}")"
install -m 644 "$SCRIPT_DIR/common/pkgtools.lock" "$OUT/locks/pkgtools.lock"

if want windows; then
  say "Windows target sysroot"
  "$SCRIPT_DIR/windows/fetch_sysroot.sh" -o "$OUT/sysroots/x86_64-w64-mingw32" --cache "$CACHE/msys2" "${UPDATE[@]}"
  install -m 644 "$SCRIPT_DIR/windows/toolchain-x86_64-w64-mingw32.cmake" "$SCRIPT_DIR/windows/toolchain-x86_64-w64-mingw32-rules.cmake" \
    "$SCRIPT_DIR/windows/environment-setup-x86_64-w64-mingw32" "$OUT/"
  install -m 755 "$SCRIPT_DIR/windows/collect_dlls.sh" "$OUT/"
  install -m 644 "$SCRIPT_DIR/windows/sysroot.lock" "$OUT/locks/windows-sysroot.lock"
  "$SCRIPT_DIR/sources/build_sources.sh" --sdk "$OUT" --cache "$CACHE" --target windows "${WORK[@]}"
fi
if want linux; then
  say "Linux target sysroot"
  "$SCRIPT_DIR/linux/fetch_sysroot.sh" -o "$OUT/sysroots/x86_64-linux-gnu" --cache "$CACHE/ubuntu" "${UPDATE[@]}"
  install -m 644 "$SCRIPT_DIR/linux/toolchain-x86_64-linux-gnu.cmake" "$SCRIPT_DIR/linux/environment-setup-x86_64-linux-gnu" "$OUT/"
  install -m 644 "$SCRIPT_DIR/linux/sysroot.lock" "$OUT/locks/linux-sysroot.lock"
  python3 -I "$SCRIPT_DIR/common/make_shlibs.py" "$SCRIPT_DIR/linux/sysroot.lock" "$CACHE/ubuntu" > "$OUT/deb-shlibs.txt"
  "$SCRIPT_DIR/sources/build_sources.sh" --sdk "$OUT" --cache "$CACHE" --target linux "${WORK[@]}"
fi
if want rpi5; then
  say "Raspberry Pi 5 target sysroot (Ubuntu 26.04 arm64)"
  # with --update: the same Ubuntu snapshot as the x86-64 sysroot, so both have the same versions
  SNAP=(); [ ${#UPDATE[@]} -gt 0 ] && SNAP=(--snapshot "$(sed -n 's/^snapshot //p' "$SCRIPT_DIR/linux/sysroot.lock")")
  "$SCRIPT_DIR/linux/fetch_sysroot.sh" --arch arm64 -o "$OUT/sysroots/aarch64-linux-gnu" --cache "$CACHE/ubuntu" "${UPDATE[@]}" "${SNAP[@]}"
  install -m 644 "$SCRIPT_DIR/linux/toolchain-aarch64-linux-gnu.cmake" "$SCRIPT_DIR/linux/environment-setup-aarch64-linux-gnu" "$OUT/"
  install -m 644 "$SCRIPT_DIR/linux/sysroot-arm64.lock" "$OUT/locks/linux-sysroot-arm64.lock"
  python3 -I "$SCRIPT_DIR/common/make_shlibs.py" "$SCRIPT_DIR/linux/sysroot-arm64.lock" "$CACHE/ubuntu" > "$OUT/deb-shlibs.txt"
  "$SCRIPT_DIR/sources/build_sources.sh" --sdk "$OUT" --cache "$CACHE" --target rpi5 "${WORK[@]}"
fi
MAC_ARCHS=()
LLVM_MAJOR=$(sed -n 's/^LLVM_VERSION=\([0-9]*\)\..*/\1/p' "$SCRIPT_DIR/common/fetch_toolchain.sh")   # compiler-rt's lib/clang/<major>
for arch in arm64 x86_64; do
  want macos-$arch || continue
  MAC_ARCHS+=($arch)
  say "macOS $arch target sysroot"
  "$SCRIPT_DIR/macos/fetch_sysroot.sh" --arch $arch -o "$OUT/sysroots/$arch-apple-darwin" --cache "$CACHE" "${UPDATE[@]}"
  install -m 644 "$SCRIPT_DIR/macos/toolchain-$arch-apple-darwin.cmake" "$SCRIPT_DIR/macos/environment-setup-$arch-apple-darwin" "$OUT/"
  install -m 644 "$SCRIPT_DIR/macos/sysroot-$arch.lock" "$OUT/locks/macos-sysroot-$arch.lock"
  # clang run as <arch>-apple-darwin-clang(++) reads llvm/bin/<arch>-apple-darwin.cfg: this sysroot, its deployment target,
  # conda-forge's libc++, ld64.lld and compiler-rt's builtins (paths relative to the file, so the SDK can move)
  min=$(sed -n 's/^macos-min //p' "$SCRIPT_DIR/macos/sysroot-$arch.lock")
  N="$OUT/sysroots/x86_64-sdk-linux"
  cat > "$N/llvm/bin/$arch-apple-darwin.cfg" <<CFG
# clang reads this when run as $arch-apple-darwin-clang or $arch-apple-darwin-clang++ (make_sdk.sh's SDK layout): the
# SDK's macOS $arch sysroot, macOS $min or newer, conda-forge's libc++ (see toolchain-apple-darwin-common.cmake) and ld64.lld
--sysroot=<CFGDIR>/../../../$arch-apple-darwin
-mmacos-version-min=$min
-stdlib++-isystem <CFGDIR>/../../../$arch-apple-darwin/usr/local/include/c++/v1
-D_LIBCPP_DISABLE_AVAILABILITY
-L<CFGDIR>/../../../$arch-apple-darwin/usr/local/lib
-fuse-ld=lld
-Wl,<CFGDIR>/../../../$arch-apple-darwin/usr/local/lib/clang/${LLVM_MAJOR}/lib/darwin/libclang_rt.osx.a
CFG
  ln -sf ../llvm/bin/clang "$N/bin/$arch-apple-darwin-clang"; ln -sf ../llvm/bin/clang "$N/bin/$arch-apple-darwin-clang++"
  install -m 644 "$SCRIPT_DIR/macos/toolchain-apple-darwin-common.cmake" "$OUT/"   # (the toolchain files read it)
  "$SCRIPT_DIR/sources/build_sources.sh" --sdk "$OUT" --cache "$CACHE" --target macos-$arch "${WORK[@]}"
done
if [ ${#MAC_ARCHS[@]} -gt 0 ]; then
  install -m 644 "$SCRIPT_DIR/macos/toolchain-apple-darwin-common.cmake" "$SCRIPT_DIR/macos/macho.py" "$OUT/"
  install -m 755 "$SCRIPT_DIR/macos/collect_dylibs.sh" "$OUT/"
fi
say "minimum versions: Ubuntu 26.04's (Windows and macOS may be newer)"
python3 -I "$SCRIPT_DIR/common/check_versions.py" "$SCRIPT_DIR/linux/sysroot.lock" "$SCRIPT_DIR/windows/sysroot.lock" \
  "$SCRIPT_DIR/linux/sysroot-arm64.lock" "$SCRIPT_DIR/macos/sysroot-arm64.lock" "$SCRIPT_DIR/macos/sysroot-x86_64.lock" | tee "$OUT/VERSIONS.txt"

say "licences"
ONAIR_SDK_CACHE="$CACHE" python3 -I "$SCRIPT_DIR/common/make_licenses.py" "$OUT" "$(IFS=,; echo "${TARGETS[*]}")"

{
  echo "Cross SDK for Linux x86-64 build machines, made by make_sdk.sh"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  sed 's/^/  /' "$OUT/sysroots/x86_64-sdk-linux/TOOLCHAIN-MANIFEST.txt"
  for t in x86_64-w64-mingw32 x86_64-linux-gnu aarch64-linux-gnu arm64-apple-darwin x86_64-apple-darwin; do
    [ -f "$OUT/sysroots/$t/SYSROOT-MANIFEST.txt" ] || continue
    echo; sed -n '1,7p' "$OUT/sysroots/$t/SYSROOT-MANIFEST.txt" | sed 's/^/  /'
  done
  for t in x86_64-w64-mingw32 x86_64-linux-gnu aarch64-linux-gnu arm64-apple-darwin x86_64-apple-darwin; do
    [ -f "$OUT/sysroots/$t/SOURCES-MANIFEST.txt" ] && { echo; echo "  $t:"; sed 's/^/  /' "$OUT/sysroots/$t/SOURCES-MANIFEST.txt"; }
  done
  echo; sed 's/^/  /' "$OUT/VERSIONS.txt"
  echo
  echo "The binaries in sysroots/ are third-party software under their own licences: LICENSES.txt lists every part with its"
  echo "licence and the folder of its licence text. Apple's macOS SDK (<arch>-apple-darwin) is under the macOS SDK and Xcode"
  echo "Agreement: not for redistribution, for use on Apple-branded computers."
  echo "FFmpeg is built as LGPL (no --enable-gpl, --enable-version3 or --enable-nonfree: checked by build_sources.sh)."
  echo "The scripts are GPL-3.0-or-later (LICENSE)."
} > "$OUT/SDK-MANIFEST.txt"

echo; echo "SDK: $OUT ($(du -sh "$OUT" | cut -f1))"
want windows && echo "use:  . $OUT/environment-setup-x86_64-w64-mingw32   (Windows)"
want linux && echo "      . $OUT/environment-setup-x86_64-linux-gnu    (Linux)"
want rpi5 && echo "      . $OUT/environment-setup-aarch64-linux-gnu   (Raspberry Pi 5)"
for arch in "${MAC_ARCHS[@]}"; do echo "      . $OUT/environment-setup-$arch-apple-darwin   (macOS $arch)"; done
if [ "$TAR" = 1 ]; then
  tar -C "$(dirname "$OUT")" -I 'zstd -T0 -10' -cf "$OUT.tar.zst" "$(basename "$OUT")"
  echo "      $OUT.tar.zst ($(du -sh "$OUT.tar.zst" | cut -f1))"
fi
