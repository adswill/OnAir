# SPDX-License-Identifier: GPL-3.0-or-later
#
# CMake toolchain file: cross-compile for macOS on arm64 (Apple silicon) on Linux with clang and ld64.lld, against the
# target sysroot sysroots/arm64-apple-darwin (macos/fetch_sysroot.sh --arch arm64).
#
#   cmake -S <src> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> [-DCMAKE_OSX_SYSROOT=<sysroot>] ...
#
# Everything else is in toolchain-apple-darwin-common.cmake (next to this file).
# for a project that packages itself with the SDK's tools (OnAir's package target)
set(ONAIR_SDK_PLATFORM macos_arm64)
set(ONAIR_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(MACOS_ARCH arm64)
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-apple-darwin-common.cmake")
