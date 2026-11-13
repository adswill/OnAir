# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# A starting point for your own OnAir plugin. Copy this folder, rename "my-plugin" everywhere (also in manifest.json, whose "id" must equal it),
# put the path of OnAir's sdk/ folder in ONAIR_SDK_DIR and build:
#   cmake -S . -B build -DONAIR_SDK_DIR=/path/to/OnAir/sdk && cmake --build build
# The result is build/plugins/my-plugin/ (library + manifest.json): copy that folder into OnAir's plugins folder (View > Plugins > Open folder).
cmake_minimum_required(VERSION 3.16)
project(my_plugin CXX)
set(CMAKE_CXX_STANDARD 17)
set(ONAIR_SDK_DIR "" CACHE PATH "OnAir's sdk/ folder")
include(${ONAIR_SDK_DIR}/cmake/OnAirPlugin.cmake)
onair_add_plugin(my-plugin SOURCES plugin.cpp INCLUDE ${ONAIR_SDK_DIR}/include)
