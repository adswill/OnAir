# Precompiled headers for the C++ targets (docs/BUILD-SPEED.md).
#
# Only STANDARD LIBRARY headers go into the PCH (cmake/onair_pch.h): no project header, no platform header, so no macro
# can leak between headers and the code the compiler sees is the same as without it. -DDECT2_PCH=OFF turns it off.
# It is off by itself on MSVC/Windows (NOMINMAX / windows.h ordering), with the Xcode generator and with CMake < 3.16.
# ccache: the build is hashed with the PCH contents; keep `sloppiness = pch_defines,time_macros` in ccache.conf.
option(DECT2_PCH "Precompile the common standard library headers (faster builds)" ON)
if(DECT2_PCH AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.19 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
   AND NOT WIN32 AND NOT ANDROID AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
  function(_onair_apply_pch)
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
      if(_t MATCHES "^(dect2_lua|dect2_sqlite|dect2_whisper|dect2_rnnoise|dect2_ffmpeg)$")
        continue()   # C / vendored code
      endif()
      get_target_property(_type ${_t} TYPE)
      if(NOT _type MATCHES "^(STATIC_LIBRARY|EXECUTABLE)$")
        continue()
      endif()
      get_target_property(_src ${_t} SOURCES)
      set(_cxx 0)
      foreach(_s IN LISTS _src)
        if(_s MATCHES "\\.(cpp|cc|cxx)$")
          math(EXPR _cxx "${_cxx}+1")
        endif()
      endforeach()
      # a target with a single C++ file gains nothing from its own PCH (building it costs what it saves)
      if(_cxx GREATER 2)
        target_precompile_headers(${_t} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${ONAIR_ROOT}/cmake/onair_pch.h>")
      endif()
    endforeach()
  endfunction()
  cmake_language(DEFER CALL _onair_apply_pch)
endif()
