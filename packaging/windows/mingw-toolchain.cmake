# CMake toolchain file: cross-compile OnAir for Windows x64 from macOS/Linux with mingw-w64.
# The dependencies (FFmpeg, GLFW, libusb, libhackrf, libiconv) come from tools/package/cross_deps_windows.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)
if(NOT ONAIR_WIN_PREFIX)
  set(ONAIR_WIN_PREFIX "$ENV{ONAIR_WIN_PREFIX}")
endif()
set(CMAKE_FIND_ROOT_PATH ${ONAIR_WIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${ONAIR_WIN_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(PKG_CONFIG_ARGN --static)
# One self-contained .exe (static libraries, including the C++ runtime and winpthread), unless generic radios are built in: SoapySDR and its
# driver modules are DLLs that exchange C++ objects and exceptions with the program, so they all have to share one C++ runtime (shipped as DLLs).
if(NOT ONAIR_WIN_SOAPY)
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
endif()
