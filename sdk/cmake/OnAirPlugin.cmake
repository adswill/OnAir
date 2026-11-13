# SPDX-License-Identifier: CC0-1.0
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# onair_add_plugin(<id> SOURCES a.cpp ... [INCLUDE <dir to onair_plugin.h>])
# Builds <id> as a shared library named <id>.<dylib|so|dll> next to a copy of manifest.json in <build>/plugins/<id>/,
# ready to be copied into OnAir's plugins folder (or zipped as <id>.onairplugin).
function(onair_add_plugin id)
  cmake_parse_arguments(P "" "INCLUDE;MANIFEST" "SOURCES" ${ARGN})
  if(NOT P_INCLUDE)
    set(P_INCLUDE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include)
  endif()
  if(NOT P_MANIFEST)
    set(P_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json)
  endif()
  string(REPLACE "-" "_" tgt "plugin_${id}")
  add_library(${tgt} SHARED ${P_SOURCES})
  target_include_directories(${tgt} PRIVATE ${P_INCLUDE})
  set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${id} PREFIX ""
    CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/${id}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/${id})
  if(WIN32)
    set_target_properties(${tgt} PROPERTIES SUFFIX ".dll")
  elseif(APPLE)
    set_target_properties(${tgt} PROPERTIES SUFFIX ".dylib")
  else()
    set_target_properties(${tgt} PROPERTIES SUFFIX ".so")
  endif()
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${P_MANIFEST} ${CMAKE_BINARY_DIR}/plugins/${id}/manifest.json)
endfunction()
