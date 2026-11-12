# Fails the build when a Windows program imports an FFmpeg DLL (avcodec-62.dll ...) the normal way. FFmpeg must be delay-loaded
# (cmake/FfmpegDelayLoad.cmake): only then does the program load it from lib\ffmpeg of its folder (core/src/platform/ffmpeg_dll_hook_win.cpp).
# A normal import is resolved by Windows before main(), from the program's folder or the PATH, never from lib\ffmpeg, so the installed
# program would not start (or would run on another FFmpeg found on the PATH).
# objdump lists only the normal imports as "DLL Name:" lines; the delay-loaded DLLs are not among them.
#   cmake -DEXE=<program.exe> -DOBJDUMP=<objdump> -P check_no_ffmpeg_dll.cmake
execute_process(COMMAND "${OBJDUMP}" -p "${EXE}" OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "objdump could not read ${EXE}")
endif()
string(REGEX MATCHALL "DLL Name: (avcodec|avformat|avutil|avdevice|avfilter|swscale|swresample)-[0-9]+\\.dll" hits "${dump}")
if(hits)
  message(FATAL_ERROR "${EXE} imports FFmpeg the normal way (${hits}): it must be delay-loaded from lib/ffmpeg (link FFmpeg through PkgConfig::FFMPEG, cmake/FfmpegDelayLoad.cmake)")
endif()
