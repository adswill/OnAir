# Optional add-on: the Aero AMBE (4800 bit/s) speech codec plug-in for Inmarsat Aero C-channel voice, built as onair-codec-aeroambe
# (OnAir loads it from the codecs folder at run time, docs/CODECS.md "Inmarsat Aero voice"). Not part of the OnAir build, never installed or
# packaged: the vocoder is patented. The decoder itself is NOT in this repository: it is the mbelib of libaeroambe (github.com/jontio/libaeroambe,
# folder mbelib-master, the one with mbe_processAmbe4800x3600Frame; the plain mbelib does not have it).
#   git clone https://github.com/jontio/libaeroambe
#   cmake -S libaeroambe/mbelib-master -B libaeroambe/mbelib-master/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --build libaeroambe/mbelib-master/build
#   cmake -S . -B build -DAEROAMBE_MBELIB=libaeroambe/mbelib-master && cmake --build build
# then copy onair-codec-aeroambe.so / .dylib / .dll (and libmbe next to it, or install it) into the OnAir codecs folder and accept the notice.
cmake_minimum_required(VERSION 3.16)
project(onair_codec_aeroambe C)
set(AEROAMBE_MBELIB "${CMAKE_CURRENT_SOURCE_DIR}/libaeroambe/mbelib-master" CACHE PATH "libaeroambe's mbelib-master folder, built in its build/ subfolder")
if(NOT EXISTS "${AEROAMBE_MBELIB}/mbelib.h")
  message(FATAL_ERROR "mbelib.h not found in ${AEROAMBE_MBELIB}: clone github.com/jontio/libaeroambe and point AEROAMBE_MBELIB at its mbelib-master (see the comment at the top of this file)")
endif()
find_library(AEROAMBE_MBE_LIB NAMES mbe libmbe PATHS "${AEROAMBE_MBELIB}/build" "${AEROAMBE_MBELIB}/build/Release" NO_DEFAULT_PATH)
if(NOT AEROAMBE_MBE_LIB)
  find_library(AEROAMBE_MBE_LIB NAMES mbe libmbe)
endif()
if(NOT AEROAMBE_MBE_LIB)
  message(FATAL_ERROR "libmbe not found: build ${AEROAMBE_MBELIB} first (cmake -S ... -B .../build && cmake --build .../build)")
endif()
add_library(onair-codec-aeroambe SHARED onair-codec-aeroambe.c)
target_include_directories(onair-codec-aeroambe PRIVATE "${AEROAMBE_MBELIB}")
target_link_libraries(onair-codec-aeroambe PRIVATE "${AEROAMBE_MBE_LIB}")
if(NOT MSVC)
  target_compile_options(onair-codec-aeroambe PRIVATE -O2 -fvisibility=hidden)
  target_link_libraries(onair-codec-aeroambe PRIVATE m)
endif()
set_target_properties(onair-codec-aeroambe PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)   # onair-codec-aeroambe.so / .dylib / .dll
# decode_bits: a call's .bits file (the coded frames OnAir saves next to each recording) -> 8 kHz WAV, with the vocoder settings as options,
# to compare them by ear on real calls: decode_bits in.bits out.wav [uvquality 1..64] [mute: errors beyond which a frame is silenced, -1 = mbelib's rule]
add_executable(decode_bits decode_bits.c)
target_include_directories(decode_bits PRIVATE "${AEROAMBE_MBELIB}")
target_link_libraries(decode_bits PRIVATE "${AEROAMBE_MBE_LIB}")
if(NOT MSVC)
  target_link_libraries(decode_bits PRIVATE m)
endif()
