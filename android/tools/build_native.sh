#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Builds libonair.so (the desktop receiver library + the JNI bridge, android/native/CMakeLists.txt) for arm64-v8a, strips it and puts
# it where Gradle packages it (android/app/src/main/jniLibs/arm64-v8a/, gitignored).
#
# Needs: the Android NDK (27.2.12479018), CMake >= 3.22, make, pkg-config with the libhackrf, libusb-1.0 and FFmpeg development headers
# of the build machine (only their headers are used; the Android library brings its own stand-ins, native/src/stubs_ext.c), and Rust
# stable with the Android target: rustup target add aarch64-linux-android.
#
# Environment (all optional):
#   ONAIR_SRC     OnAir repository root to build the core from (default: this checkout, android/..)
#   ANDROID_NDK   NDK folder (else ANDROID_NDK_HOME, ANDROID_NDK_ROOT, $ANDROID_HOME/ndk/<NDK_VERSION>, ~/Library/Android/sdk/ndk/<NDK_VERSION>)
#   NDK_VERSION   default 27.2.12479018
#   CMAKE         cmake binary (else the SDK's cmake/3.31.5, else cmake on PATH)
#   JOBS          parallel jobs (default: 4)
#   BUILD_TYPE    Release (default) or RelWithDebInfo / Debug
#   LIBUSB_SRC    a libusb 1.0.24+ source tree (git clone https://github.com/libusb/libusb, tag v1.0.27): builds libusb1.0.so and turns
#                 on the HackRF (and the Airspy libraries below). Without it only the RTL-SDR works over USB (docs/USB.md).
#   AIRSPY_SRC    airspyone_host source tree (needs LIBUSB_SRC): libairspy.so for the Airspy R2 / Mini
#   AIRSPYHF_SRC  airspyhf source tree (needs LIBUSB_SRC): libairspyhf.so for the Airspy HF+
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"          # android/
ONAIR_SRC="${ONAIR_SRC:-$(cd "$HERE/.." && pwd)}"
NDK_VERSION="${NDK_VERSION:-27.2.12479018}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-4}"
ABI=arm64-v8a
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"

NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then NDK="$SDK/ndk/$NDK_VERSION"; fi
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "Android NDK not found (tried $NDK): set ANDROID_NDK" >&2; exit 1; }

CM="${CMAKE:-}"
if [ -z "$CM" ]; then
  if [ -x "$SDK/cmake/3.31.5/bin/cmake" ]; then CM="$SDK/cmake/3.31.5/bin/cmake"; else CM="$(command -v cmake || true)"; fi
fi
[ -n "$CM" ] || { echo "cmake not found: set CMAKE" >&2; exit 1; }
MAKE_BIN="$(command -v make || true)"
[ -n "$MAKE_BIN" ] || { echo "make not found" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) HOST_TAG=darwin-x86_64 ;;   # the NDK's macOS tools are universal binaries under this name
  Linux)  HOST_TAG=linux-x86_64 ;;
  *) echo "unsupported build host $(uname -s)" >&2; exit 1 ;;
esac
STRIP="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip"

command -v cargo >/dev/null || { echo "cargo not found: install Rust stable (rustup) and put it on PATH" >&2; exit 1; }
if command -v rustup >/dev/null && ! rustup target list --installed | grep -qx aarch64-linux-android; then
  echo "Rust target aarch64-linux-android is missing: rustup target add aarch64-linux-android" >&2; exit 1
fi

B="$HERE/native/build/$ABI"
echo "OnAir sources: $ONAIR_SRC"
echo "NDK:           $NDK"
"$CM" -S "$HERE/native" -B "$B" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_MAKE_PROGRAM="$MAKE_BIN" -DONAIR_SRC="$ONAIR_SRC" \
  -DLIBUSB_SRC="${LIBUSB_SRC:-}" -DAIRSPY_SRC="${AIRSPY_SRC:-}" -DAIRSPYHF_SRC="${AIRSPYHF_SRC:-}"
python3 "$HERE/tools/gen_modes.py" "$ONAIR_SRC" "$HERE"          # the mode list comes from the desktop table: new modes need no Android change
python3 "$HERE/tools/gen_results_map.py" "$ONAIR_SRC"            # and so do their results
TARGETS=(onair)
[ -n "${LIBUSB_SRC:-}" ] && TARGETS+=(usb1.0)
[ -n "${LIBUSB_SRC:-}" ] && [ -n "${AIRSPY_SRC:-}" ] && TARGETS+=(airspy)
[ -n "${LIBUSB_SRC:-}" ] && [ -n "${AIRSPYHF_SRC:-}" ] && TARGETS+=(airspyhf)
"$CM" --build "$B" --target "${TARGETS[@]}" -j "$JOBS"

OUT="$HERE/app/src/main/jniLibs/$ABI"
mkdir -p "$OUT"
"$STRIP" --strip-unneeded -o "$OUT/libonair.so" "$B/libonair.so"
for t in "${TARGETS[@]:1}"; do "$STRIP" --strip-unneeded -o "$OUT/lib$t.so" "$B/lib$t.so"; done
[ -n "${LIBUSB_SRC:-}" ] || rm -f "$OUT/libusb1.0.so" "$OUT/libairspy.so" "$OUT/libairspyhf.so"   # no stale USB libraries from an earlier build
ls -la "$OUT"/*.so
