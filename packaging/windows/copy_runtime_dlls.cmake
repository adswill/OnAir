# Run after a Windows (MinGW) build: copies every DLL a program needs from the compiler's tree (FFmpeg, GLFW, libhackrf, libusb, the C++
# runtime ...) next to it, plus any extra DLLs given (a radio library such as LimeSuite.dll), so the program starts from the build folder.
#   cmake -DEXE=<program> -DSEARCH_DIR=<compiler bin dir> -DOBJDUMP=<objdump> [-DEXTRA=<dll;dll>] -P copy_runtime_dlls.cmake
# Files that are already up to date are left alone.
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)   # normalize the paths before the regexes see them
endif()
get_filename_component(dest "${EXE}" DIRECTORY)
file(TO_CMAKE_PATH "${SEARCH_DIR}" search)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${EXE}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved   # the Windows system DLLs: not searched for, not copied
  DIRECTORIES "${search}"
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-")
set(copy "")
foreach(dll IN LISTS resolved)
  file(TO_CMAKE_PATH "${dll}" dll)
  string(FIND "${dll}" "${search}/" at)
  if(at EQUAL 0)
    list(APPEND copy "${dll}")
  endif()
endforeach()
foreach(dll IN LISTS EXTRA)
  if(NOT EXISTS "${dll}")
    message(WARNING "DECT2_EXTRA_RUNTIME_DLLS: ${dll} does not exist")
  else()
    list(APPEND copy "${dll}")
  endif()
endforeach()
if(copy)
  file(COPY ${copy} DESTINATION "${dest}")
endif()
