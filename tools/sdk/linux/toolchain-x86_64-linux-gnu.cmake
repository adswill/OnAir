# SPDX-License-Identifier: GPL-3.0-or-later
#
# CMake toolchain file: build for Linux x86-64 (Ubuntu 26.04: glibc 2.43, GCC 16's libstdc++) with clang and lld, against
# the target sysroot made by linux/fetch_sysroot.sh (Ubuntu's packages, FFmpeg included).
#
#   cmake -S <src> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> [-DCMAKE_SYSROOT=<sysroot>] ...
#
# In the SDK (make_sdk.sh) this file sits in the SDK's top folder and uses only the SDK: the tools in
# sysroots/x86_64-sdk-linux/bin (clang, lld, llvm-ar ..., cargo/rustc) and the target sysroot sysroots/x86_64-linux-gnu.
# Outside an SDK it takes clang, ld.lld and llvm-* from PATH. The host's headers and libraries are never used, so the
# result is the same on any build machine and runs on Ubuntu 26.04 (and newer).
#
# The target sysroot, in this order: -DCMAKE_SYSROOT=..., the environment variable LINUX_SYSROOT,
# sysroots/x86_64-linux-gnu next to this file (the SDK).
# FFmpeg is Ubuntu 26.04's (8.0.1, the GPL build; libavcodec.so.62): on the target it comes from Ubuntu's packages
# (libavcodec62 ...), so nothing has to be shipped with the program.
# for a project that packages itself with the SDK's tools (OnAir's package target)
set(ONAIR_SDK_PLATFORM linux_amd64)
set(ONAIR_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(LINUX_TARGET x86_64-linux-gnu)

# --- the SDK's own tools, if this file is in an SDK -------------------------------------------------------------------
set(LINUX_TOOLS_DIR "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin/clang")
  set(LINUX_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-sdk-linux/bin")
endif()
# linux_tool(<var> <names...>): the SDK's tool if there is one, else the first of the names on PATH; required
function(linux_tool var)
  if(${var})
    return()
  endif()
  if(LINUX_TOOLS_DIR)
    find_program(${var} NAMES ${ARGN} PATHS "${LINUX_TOOLS_DIR}" NO_DEFAULT_PATH)
  endif()
  find_program(${var} NAMES ${ARGN} REQUIRED)
endfunction()

# --- target sysroot -----------------------------------------------------------------------------------------------------
if(NOT CMAKE_SYSROOT)
  if(DEFINED ENV{LINUX_SYSROOT} AND NOT "$ENV{LINUX_SYSROOT}" STREQUAL "")
    set(CMAKE_SYSROOT "$ENV{LINUX_SYSROOT}")
  else()
    set(CMAKE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/sysroots/x86_64-linux-gnu")
  endif()
endif()
get_filename_component(CMAKE_SYSROOT "${CMAKE_SYSROOT}" ABSOLUTE)
if(NOT EXISTS "${CMAKE_SYSROOT}/usr/include/stdio.h" OR NOT EXISTS "${CMAKE_SYSROOT}/usr/lib/x86_64-linux-gnu/crt1.o")
  message(FATAL_ERROR "${CMAKE_SYSROOT} is not an Ubuntu sysroot (no usr/include/stdio.h or crt1.o): make one with linux/fetch_sysroot.sh")
endif()

# --- compilers: clang; it takes GCC 16's libstdc++ and crt files from the sysroot (usr/lib/gcc/x86_64-linux-gnu/16) --------
linux_tool(LINUX_CLANG x86_64-linux-gnu-clang clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18)
linux_tool(LINUX_CLANGXX x86_64-linux-gnu-clang++ clang++ clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++-18)
set(CMAKE_C_COMPILER "${LINUX_CLANG}")
set(CMAKE_CXX_COMPILER "${LINUX_CLANGXX}")
set(CMAKE_ASM_COMPILER "${LINUX_CLANG}")
set(CMAKE_C_COMPILER_TARGET ${LINUX_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${LINUX_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${LINUX_TARGET})
if(LINUX_TOOLS_DIR)
  set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${LINUX_TOOLS_DIR}/clang-scan-deps")
  set(CMAKE_ASM_NASM_COMPILER "${LINUX_TOOLS_DIR}/nasm")
endif()

# --- binutils: LLVM's -------------------------------------------------------------------------------------------------------
linux_tool(CMAKE_AR llvm-ar)
linux_tool(CMAKE_RANLIB llvm-ranlib)
linux_tool(CMAKE_NM llvm-nm)
linux_tool(CMAKE_OBJDUMP llvm-objdump)
linux_tool(CMAKE_OBJCOPY llvm-objcopy)
linux_tool(CMAKE_STRIP llvm-strip)
linux_tool(CMAKE_READELF llvm-readobj)

# --- linker: lld ---------------------------------------------------------------------------------------------------------
linux_tool(LINUX_LLD ld.lld)
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${LINUX_LLD}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--ld-path=${LINUX_LLD}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--ld-path=${LINUX_LLD}")

# --- Rust (OnAir's rust/ workspace): the SDK's cargo and rustc ------------------------------------------------------------
if(LINUX_TOOLS_DIR AND EXISTS "${LINUX_TOOLS_DIR}/cargo")
  set(ONAIR_CARGO "${LINUX_TOOLS_DIR}/cargo" CACHE FILEPATH "cargo (the SDK's)")
  set(ONAIR_RUSTC "${LINUX_TOOLS_DIR}/rustc" CACHE FILEPATH "rustc (the SDK's)")
endif()

# --- finding packages: only inside the sysroot ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)

# pkg-config: only the sysroot's .pc files (usr/local first, for anything added there). The .pc files name /usr..., so the
# sysroot is put in front of every -I/-L (PKG_CONFIG_SYSROOT_DIR).
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/local/lib/pkgconfig:${CMAKE_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
