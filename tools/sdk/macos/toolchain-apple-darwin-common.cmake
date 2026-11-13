# SPDX-License-Identifier: GPL-3.0-or-later
#
# The common part of the macOS CMake toolchain files (toolchain-arm64-apple-darwin.cmake and
# toolchain-x86_64-apple-darwin.cmake set MACOS_ARCH and include this one): cross-compile for macOS on Linux with
# clang and ld64.lld, against a target sysroot made by macos/fetch_sysroot.sh (Apple's SDK, with conda-forge's
# libraries in usr/local).
#
# In the SDK (make_sdk.sh) the toolchain files sit in the SDK's top folder and use only the SDK: the tools in
# sysroots/x86_64-sdk-linux/bin and the target sysroot sysroots/<arch>-apple-darwin. Outside an SDK they take clang,
# ld64.lld and llvm-* from PATH.
#
# The target sysroot, in this order: -DCMAKE_OSX_SYSROOT=..., the environment variable MAC_SYSROOT,
# sysroots/<arch>-apple-darwin next to the toolchain file (the SDK).
# The deployment target (oldest macOS): -DCMAKE_OSX_DEPLOYMENT_TARGET=..., else the sysroot's (its SYSROOT-MANIFEST.txt;
# older than that is not possible: the libraries need it).
# C++: conda-forge's libc++ (LLVM's, headers in usr/local/include/c++/v1, libc++.1.dylib in usr/local/lib), the one
# the sysroot's C++ libraries (SoapySDR ...) use, not the SDK's. Programs then load @rpath/libc++.1.dylib and the other
# @rpath libraries: ship them with the program (collect_dylibs.sh) and give it an rpath, e.g.
# -DCMAKE_INSTALL_RPATH=@executable_path/../Frameworks (an .app bundle) or @executable_path (a plain folder).
if(NOT MACOS_ARCH)
  message(FATAL_ERROR "include this file from toolchain-arm64-apple-darwin.cmake or toolchain-x86_64-apple-darwin.cmake")
endif()
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR ${MACOS_ARCH})
set(CMAKE_OSX_ARCHITECTURES ${MACOS_ARCH} CACHE STRING "the target architecture")
set(MACOS_TRIPLE ${MACOS_ARCH}-apple-darwin)

# --- the SDK's own tools, if this file is in an SDK -------------------------------------------------------------------
set(MACOS_TOOLS_DIR "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin/clang")
  set(MACOS_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin")
endif()
function(macos_tool var)
  if(${var})
    return()
  endif()
  if(MACOS_TOOLS_DIR)
    find_program(${var} NAMES ${ARGN} PATHS "${MACOS_TOOLS_DIR}" NO_DEFAULT_PATH)
  endif()
  find_program(${var} NAMES ${ARGN} REQUIRED)
endfunction()

# --- target sysroot -----------------------------------------------------------------------------------------------------
if(NOT CMAKE_OSX_SYSROOT)
  if(DEFINED ENV{MAC_SYSROOT} AND NOT "$ENV{MAC_SYSROOT}" STREQUAL "")
    set(CMAKE_OSX_SYSROOT "$ENV{MAC_SYSROOT}")
  else()
    set(CMAKE_OSX_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/sysroots/${MACOS_TRIPLE}")
  endif()
endif()
get_filename_component(CMAKE_OSX_SYSROOT "${CMAKE_OSX_SYSROOT}" ABSOLUTE)
set(CMAKE_OSX_SYSROOT "${CMAKE_OSX_SYSROOT}" CACHE PATH "the macOS target sysroot" FORCE)
if(NOT EXISTS "${CMAKE_OSX_SYSROOT}/SDKSettings.json" OR NOT EXISTS "${CMAKE_OSX_SYSROOT}/usr/local/include/c++/v1/vector")
  message(FATAL_ERROR "${CMAKE_OSX_SYSROOT} is not a macOS sysroot of macos/fetch_sysroot.sh (no SDKSettings.json or libc++ headers)")
endif()
set(_mac_min "")
if(EXISTS "${CMAKE_OSX_SYSROOT}/SYSROOT-MANIFEST.txt")
  file(STRINGS "${CMAKE_OSX_SYSROOT}/SYSROOT-MANIFEST.txt" _line REGEX "^deployment: +macOS ")
  string(REGEX REPLACE "^deployment: +macOS ([0-9.]+).*" "\\1" _mac_min "${_line}")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_mac_min}" CACHE STRING "the oldest macOS the program runs on")
elseif(_mac_min AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS _mac_min)
  message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} is older than the sysroot's libraries (macOS ${_mac_min})")
endif()

# --- compilers: clang, for <arch>-apple-macos<deployment target> -----------------------------------------------------------
macos_tool(MACOS_CLANG clang clang-23 clang-22 clang-21 clang-20 clang-19)
macos_tool(MACOS_CLANGXX clang++ clang++-23 clang++-22 clang++-21 clang++-20 clang++-19)
set(CMAKE_C_COMPILER "${MACOS_CLANG}")
set(CMAKE_CXX_COMPILER "${MACOS_CLANGXX}")
set(CMAKE_OBJC_COMPILER "${MACOS_CLANG}")
set(CMAKE_OBJCXX_COMPILER "${MACOS_CLANGXX}")
set(CMAKE_ASM_COMPILER "${MACOS_CLANG}")
foreach(_lang C CXX OBJC OBJCXX ASM)
  set(CMAKE_${_lang}_COMPILER_TARGET ${MACOS_ARCH}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET})
endforeach()
if(MACOS_TOOLS_DIR)
  set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${MACOS_TOOLS_DIR}/clang-scan-deps")
endif()
# libc++: conda-forge's headers instead of the SDK's (the libraries' C++ runtime, LLVM 23 like clang). Its
# availability markings (std::to_chars "introduced in macOS 13.3" ...) are about Apple's system libc++; the program
# loads conda-forge's libc++.1.dylib, which has everything on any macOS: they are switched off, as conda-forge does.
set(CMAKE_CXX_FLAGS_INIT "-stdlib++-isystem ${CMAKE_OSX_SYSROOT}/usr/local/include/c++/v1 -D_LIBCPP_DISABLE_AVAILABILITY")
set(CMAKE_OBJCXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT}")

# --- tools: LLVM's ------------------------------------------------------------------------------------------------------
macos_tool(CMAKE_AR llvm-ar)
macos_tool(CMAKE_RANLIB llvm-ranlib)
macos_tool(CMAKE_NM llvm-nm)
macos_tool(CMAKE_OBJDUMP llvm-objdump)
macos_tool(CMAKE_STRIP llvm-strip)
macos_tool(CMAKE_INSTALL_NAME_TOOL llvm-install-name-tool)
macos_tool(CMAKE_LIBTOOL llvm-libtool-darwin)
macos_tool(CMAKE_DSYMUTIL dsymutil)
macos_tool(CMAKE_LIPO llvm-lipo)
macos_tool(CMAKE_OTOOL llvm-otool)

# --- linker: ld64.lld; libraries from usr/local first (so -lc++ is conda-forge's libc++) --------------------------------
macos_tool(MACOS_LLD ld64.lld)
set(_ld "-fuse-ld=lld --ld-path=${MACOS_LLD} -L${CMAKE_OSX_SYSROOT}/usr/local/lib")   # (-fuse-ld=lld: lld's arguments)
# compiler-rt's builtins (__isPlatformVersionAtLeast for @available, __cpu_model for __builtin_cpu_supports ...): clang
# links its resource dir's lib/darwin/libclang_rt.osx.a into every macOS program, but the SDK's clang (LLVM's Linux
# release) has none; the sysroot has conda-forge's, for clang's major version (fetch_sysroot.sh). ld64.lld takes an
# archive anywhere on the command line.
file(GLOB _mac_rt "${CMAKE_OSX_SYSROOT}/usr/local/lib/clang/*/lib/darwin/libclang_rt.osx.a")
if(NOT _mac_rt)
  message(FATAL_ERROR "${CMAKE_OSX_SYSROOT} has no lib/clang/*/lib/darwin/libclang_rt.osx.a (compiler-rt): re-run macos/fetch_sysroot.sh")
endif()
list(GET _mac_rt 0 _mac_rt)
string(APPEND _ld " ${_mac_rt}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ld}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ld}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ld}")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES)

# --- Rust (OnAir's rust/ workspace): the SDK's cargo and rustc (with the aarch64/x86_64-apple-darwin std) ----------------
if(MACOS_TOOLS_DIR AND EXISTS "${MACOS_TOOLS_DIR}/cargo")
  set(ONAIR_CARGO "${MACOS_TOOLS_DIR}/cargo" CACHE FILEPATH "cargo (the SDK's)")
  set(ONAIR_RUSTC "${MACOS_TOOLS_DIR}/rustc" CACHE FILEPATH "rustc (the SDK's)")
endif()
if(MACOS_ARCH STREQUAL "arm64")
  set(MACOS_RUST_TARGET aarch64-apple-darwin)
else()
  set(MACOS_RUST_TARGET x86_64-apple-darwin)
endif()

# --- finding packages: only inside the sysroot (usr/local first, then the SDK) -----------------------------------------
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}/usr/local;${CMAKE_OSX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_FRAMEWORK LAST)

# pkg-config: only usr/local's .pc files. They are relocatable (prefix=${pcfiledir}/../..), so no PKG_CONFIG_SYSROOT_DIR.
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_OSX_SYSROOT}/usr/local/lib/pkgconfig:${CMAKE_OSX_SYSROOT}/usr/local/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
unset(ENV{PKG_CONFIG_SYSROOT_DIR})

# where the dylibs to ship with a program are (collect_dylibs.sh)
set(MACOS_SYSROOT_LIB "${CMAKE_OSX_SYSROOT}/usr/local/lib")
