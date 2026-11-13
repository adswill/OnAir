# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Runs `onair --hidden --mute --drag-selftest` from a renamed copy of the program (its own data, config and user-defaults domain, deleted after):
# mouse drags are injected on the spectrum (drag to tune, the scale pan, right and middle drag, Shift fine steps, the old Pan setting) and
# the tuned frequency and the view are checked (app/gfx/plots.cpp dragTuneSelfTest). Nothing is drawn on the real screen, no sound plays.
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}/data" "${DIR}/cfg")
if(WIN32)
  set(copy "${DIR}/onair_dragtest.exe")
else()
  set(copy "${DIR}/onair_dragtest")
endif()
file(COPY_FILE "${BIN}" "${copy}")
if(APPLE)   # settings live in the user defaults under the program name: start from nothing
  execute_process(COMMAND defaults delete onair_dragtest ERROR_QUIET OUTPUT_QUIET)
endif()
set(ENV{DECT2_DATA_DIR} "${DIR}/data")
set(ENV{XDG_CONFIG_HOME} "${DIR}/cfg")
set(ENV{APPDATA} "${DIR}/cfg")
set(ENV{HOME} "${DIR}")
execute_process(COMMAND "${copy}" --hidden --mute --drag-selftest
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 100)
message("${out}")
if(APPLE)
  execute_process(COMMAND defaults delete onair_dragtest ERROR_QUIET OUTPUT_QUIET)   # and leave nothing behind
endif()
file(REMOVE_RECURSE "${DIR}")
if(out MATCHES "SKIP:")
  message(WARNING "drag to tune GUI test skipped: the spectrum was not on screen")
endif()
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "drag to tune GUI test failed (exit ${rc})\n${err}")
endif()
