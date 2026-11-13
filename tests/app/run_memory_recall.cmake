# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Runs `onair --hidden --mute --memory-selftest` from a renamed copy of the program, so the owner's settings (kept under the program's name) and
# data folder are never touched: the copy gets its own data folder (DECT2_DATA_DIR) and config folders. The self test is app/memory/memory_selftest.cpp.
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}/data" "${DIR}/cfg")
if(WIN32)
  set(copy "${DIR}/onair_memtest.exe")
else()
  set(copy "${DIR}/onair_memtest")
endif()
file(COPY_FILE "${BIN}" "${copy}")
if(APPLE)   # settings live in the user defaults under the program name: start from nothing
  execute_process(COMMAND defaults delete onair_memtest ERROR_QUIET OUTPUT_QUIET)
endif()
set(ENV{DECT2_DATA_DIR} "${DIR}/data")
set(ENV{XDG_CONFIG_HOME} "${DIR}/cfg")
set(ENV{APPDATA} "${DIR}/cfg")
execute_process(COMMAND "${copy}" --hidden --mute --memory-selftest
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
message("${out}")
if(APPLE)
  execute_process(COMMAND defaults delete onair_memtest ERROR_QUIET OUTPUT_QUIET)   # and leave nothing behind
endif()
file(REMOVE_RECURSE "${DIR}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "memory self test failed (exit ${rc})\n${err}")
endif()
