# Optional add-on: the Inmarsat Aero voice plug-in "aeroambe" (OnAir loads it at run time, docs/CODECS.md "AMBE (Inmarsat Aero voice)").
# Not part of the OnAir build, never installed or packaged. The decoder is NOT in this repository: it is mbelib as shipped inside libaeroambe.
#   git clone https://github.com/jontio/libaeroambe
#   cmake -S . -B build -DMBELIB_DIR=/path/to/libaeroambe/mbelib-master && cmake --build build
# then copy the library (libonair-codec-aeroambe.dylib / .so / onair-codec-aeroambe.dll) into the OnAir codecs folder, named onair-codec-aeroambe.<ext>.
cmake_minimum_required(VERSION 3.16)
project(onair_codec_aeroambe C)
set(MBELIB_DIR "" CACHE PATH "libaeroambe's mbelib-master folder (mbelib.c, ambe4800x3600.c, ecc.c, ...)")
if(NOT EXISTS "${MBELIB_DIR}/mbelib.h")
  message(FATAL_ERROR "mbelib.h not found in MBELIB_DIR='${MBELIB_DIR}': clone https://github.com/jontio/libaeroambe and point MBELIB_DIR at its mbelib-master folder")
endif()
file(GLOB MBE_SRC "${MBELIB_DIR}/*.c")
add_library(onair-codec-aeroambe SHARED aeroambe_plugin.c ${MBE_SRC})
target_include_directories(onair-codec-aeroambe PRIVATE "${MBELIB_DIR}")
if(NOT MSVC)
  target_compile_options(onair-codec-aeroambe PRIVATE -w -O2)
  target_link_libraries(onair-codec-aeroambe PRIVATE m)
endif()
set_target_properties(onair-codec-aeroambe PROPERTIES PREFIX "" C_VISIBILITY_PRESET default)
