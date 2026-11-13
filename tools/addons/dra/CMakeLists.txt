# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Optional add-on: the DRA+ decoder for CDR sound, built as libonair_dra (OnAir loads it at run time, docs/CODECS.md "DRA+ (CDR sound)").
# Not part of the OnAir build, never installed or packaged. The decoder source is NOT in this repository: get it with extract_dra.py.
#   python3 extract_dra.py                  (downloads maliwen2015's patch, writes dra_src/dradec.c and dradec.h)
#   cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build
# then copy the library (libonair_dra.dylib / libonair_dra.so / onair_dra.dll) into the OnAir codecs folder.
cmake_minimum_required(VERSION 3.16)
project(onair_dra C)
set(DRA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/dra_src" CACHE PATH "folder with dradec.c and dradec.h (extract_dra.py)")
if(NOT EXISTS "${DRA_SRC}/dradec.c" OR NOT EXISTS "${DRA_SRC}/dradec.h")
  message(FATAL_ERROR "dradec.c / dradec.h not found in ${DRA_SRC}: run  python3 extract_dra.py  first (see the comment at the top of this file)")
endif()
add_library(onair_dra SHARED "${DRA_SRC}/dradec.c")
target_include_directories(onair_dra PRIVATE "${DRA_SRC}")
if(NOT MSVC)
  target_compile_options(onair_dra PRIVATE -w -O2)
  target_link_libraries(onair_dra PRIVATE m)
endif()
set_target_properties(onair_dra PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON C_VISIBILITY_PRESET default)
if(WIN32)
  set_target_properties(onair_dra PROPERTIES PREFIX "")   # onair_dra.dll
endif()
