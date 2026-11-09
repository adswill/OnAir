# Fails the build when a program built with DECT2_FFMPEG_PREFIX still imports an FFmpeg DLL (avcodec-62.dll ...): it then runs on
# the host's FFmpeg (MSYS2: --enable-gpl, with libavdevice), not on the local LGPL build the release ships, and a difference such as
# a missing camera input (no libavdevice) does not show in the developer's build.
#   cmake -DEXE=<program.exe> -DOBJDUMP=<objdump> -P check_no_ffmpeg_dll.cmake
execute_process(COMMAND "${OBJDUMP}" -p "${EXE}" OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "objdump could not read ${EXE}")
endif()
string(REGEX MATCHALL "DLL Name: (avcodec|avformat|avutil|avdevice|avfilter|swscale|swresample)-[0-9]+\\.dll" hits "${dump}")
if(hits)
  message(FATAL_ERROR "${EXE} imports the host's FFmpeg (${hits}) although DECT2_FFMPEG_PREFIX is set: check that the prefix has the static libraries (libavcodec.a, libavdevice.a)")
endif()
