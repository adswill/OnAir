# SPDX-License-Identifier: GPL-3.0-or-later
#
# CMake toolchain file: cross-compile for Windows x64 (UCRT, the MSYS2 UCRT64 ABI) on Linux with clang, against a target
# sysroot made by fetch_sysroot.sh.
#
#   cmake -S <src> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> [-DCMAKE_SYSROOT=<sysroot>] ...
#
# In the SDK (make_sdk.sh) this file sits in the SDK's top folder and uses only the SDK: the tools in
# sysroots/x86_64-sdk-linux/bin (clang, lld, the mingw-w64 GNU binutils, cargo/rustc) and the target sysroot
# sysroots/x86_64-w64-mingw32. Outside an SDK it takes clang, llvm-windres and x86_64-w64-mingw32-* from PATH.
#
# The target sysroot, in this order: -DCMAKE_SYSROOT=..., the environment variable WIN_SYSROOT,
# sysroots/x86_64-w64-mingw32 next to this file (the SDK).
# clang compiles against the sysroot's headers (mingw-w64 UCRT headers, GCC's libstdc++ headers) and links its CRT,
# libgcc, libstdc++, winpthreads and libraries, so all of them come from the same MSYS2 build. GNU ld links (OnAir's
# FFmpeg delay-loading needs GNU dlltool, and was written for GNU ld).
#
# Options (-D...):
#   WIN_LINKER=bfd|lld   linker: GNU ld (default, x86_64-w64-mingw32-ld) or LLVM lld
#   WIN_STATIC=ON        link with -static: libgcc, libstdc++, winpthreads and every -lX library from its libX.a (default
#                        OFF: DLLs, as in a native MSYS2 build; keep it OFF when C++ DLLs such as SoapySDR's are used,
#                        they must share one C++ runtime DLL with the program)
# for a project that packages itself with the SDK's tools (OnAir's package target)
set(ONAIR_SDK_PLATFORM windows)
set(ONAIR_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)   # the binutils tools; OnAir's cmake/FfmpegDelayLoad.cmake looks for ${TOOLCHAIN_PREFIX}-dlltool
set(WIN_TARGET x86_64-w64-windows-gnu)

# --- the SDK's own tools, if this file is in an SDK -------------------------------------------------------------------
set(WIN_TOOLS_DIR "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin/clang")
  set(WIN_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin")
endif()
# win_tool(<var> <names...>): the SDK's tool if there is one, else the first of the names on PATH; required
function(win_tool var)
  if(${var})
    return()
  endif()
  if(WIN_TOOLS_DIR)
    find_program(${var} NAMES ${ARGN} PATHS "${WIN_TOOLS_DIR}" NO_DEFAULT_PATH)
  endif()
  find_program(${var} NAMES ${ARGN} REQUIRED)
endfunction()

# --- target sysroot -----------------------------------------------------------------------------------------------------
if(NOT CMAKE_SYSROOT)
  if(DEFINED ENV{WIN_SYSROOT} AND NOT "$ENV{WIN_SYSROOT}" STREQUAL "")
    set(CMAKE_SYSROOT "$ENV{WIN_SYSROOT}")
  else()
    set(CMAKE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-w64-mingw32")
  endif()
endif()
get_filename_component(CMAKE_SYSROOT "${CMAKE_SYSROOT}" ABSOLUTE)
if(NOT EXISTS "${CMAKE_SYSROOT}/include/windows.h" OR NOT EXISTS "${CMAKE_SYSROOT}/lib/libucrt.a")
  message(FATAL_ERROR "${CMAKE_SYSROOT} is not an MSYS2 UCRT64 sysroot (no include/windows.h or lib/libucrt.a): make one with fetch_sysroot.sh")
endif()

# --- compilers: clang (18 or newer; the sysroot's libstdc++ headers are GCC's) -------------------------------------------
win_tool(WIN_CLANG clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18)
win_tool(WIN_CLANGXX clang++ clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++-18)
set(CMAKE_C_COMPILER "${WIN_CLANG}")
set(CMAKE_CXX_COMPILER "${WIN_CLANGXX}")
set(CMAKE_ASM_COMPILER "${WIN_CLANG}")
set(CMAKE_C_COMPILER_TARGET ${WIN_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${WIN_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${WIN_TARGET})
if(WIN_TOOLS_DIR)
  set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${WIN_TOOLS_DIR}/clang-scan-deps")   # C++20 module scanning (Ninja)
endif()

# --- binutils: GNU (OnAir's FFmpeg delay-loading needs GNU dlltool's --output-delaylib) ----------------------------------
win_tool(CMAKE_AR ${TOOLCHAIN_PREFIX}-ar)
win_tool(CMAKE_RANLIB ${TOOLCHAIN_PREFIX}-ranlib)
win_tool(CMAKE_NM ${TOOLCHAIN_PREFIX}-nm)
win_tool(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}-objdump)
win_tool(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}-objcopy)
win_tool(CMAKE_STRIP ${TOOLCHAIN_PREFIX}-strip)
win_tool(CMAKE_DLLTOOL ${TOOLCHAIN_PREFIX}-dlltool)
if(WIN_TOOLS_DIR)
  set(DECT2_DLLTOOL "${CMAKE_DLLTOOL}" CACHE FILEPATH "GNU dlltool (OnAir's FFmpeg delay-loading)")
endif()

# --- resources (.rc): llvm-windres preprocesses with clang, so it needs the sysroot too -----------------------------------
win_tool(CMAKE_RC_COMPILER llvm-windres llvm-windres-23 llvm-windres-22 llvm-windres-21 llvm-windres-20 llvm-windres-19 llvm-windres-18)
set(CMAKE_RC_FLAGS_INIT "--target=pe-x86-64 --preprocessor-arg=--sysroot=${CMAKE_SYSROOT}")   # (it runs the clang next to itself)

# --- linker --------------------------------------------------------------------------------------------------------------
if(NOT WIN_LINKER)
  set(WIN_LINKER bfd)
endif()
if(WIN_LINKER STREQUAL "lld")
  win_tool(WIN_LLD ld.lld)
  set(_ld "--ld-path=${WIN_LLD}")
elseif(WIN_LINKER STREQUAL "bfd")
  win_tool(WIN_GNU_LD ${TOOLCHAIN_PREFIX}-ld)
  set(_ld "--ld-path=${WIN_GNU_LD}")   # named explicitly: on its own clang can take the host's ELF ld.bfd
else()
  message(FATAL_ERROR "WIN_LINKER=${WIN_LINKER}: use bfd or lld")
endif()
set(_static "")
if(WIN_STATIC)
  set(_static "-static")   # every -lX then takes libX.a: libgcc, libstdc++, winpthreads and the libraries
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ld} ${_static}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ld} ${_static}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ld} ${_static}")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WIN_LINKER WIN_STATIC)
# winpthreads in every link, as MSYS2's GCC does (see the rules file; it must run after CMake's platform defaults)
if(NOT CMAKE_USER_MAKE_RULES_OVERRIDE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/toolchain-x86_64-w64-mingw32-rules.cmake")
  set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/toolchain-x86_64-w64-mingw32-rules.cmake")
endif()

# --- Rust (OnAir's rust/ workspace): the SDK's cargo and rustc -----------------------------------------------------------
if(WIN_TOOLS_DIR AND EXISTS "${WIN_TOOLS_DIR}/cargo")
  set(ONAIR_CARGO "${WIN_TOOLS_DIR}/cargo" CACHE FILEPATH "cargo (the SDK's)")
  set(ONAIR_RUSTC "${WIN_TOOLS_DIR}/rustc" CACHE FILEPATH "rustc (the SDK's)")
endif()

# --- finding packages: only inside the sysroot ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config: only the sysroot's .pc files. They are relocatable (prefix=${pcfiledir}/../..), so no PKG_CONFIG_SYSROOT_DIR.
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/lib/pkgconfig:${CMAKE_SYSROOT}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
unset(ENV{PKG_CONFIG_SYSROOT_DIR})

# OpenSSL is a static library here (sources/build_sources.sh): CMake's FindOpenSSL then adds what libcrypto.a needs
# from Windows (crypt32, ws2_32) to OpenSSL::Crypto
if(NOT DEFINED OPENSSL_USE_STATIC_LIBS)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# where the DLLs to ship next to a program are (collect_dlls.sh; OnAir: packaging/windows/copy_runtime_dlls.cmake -DSEARCH_DIR=...)
set(WIN_SYSROOT_BIN "${CMAKE_SYSROOT}/bin")
