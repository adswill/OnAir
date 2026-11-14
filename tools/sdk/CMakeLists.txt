# SPDX-License-Identifier: GPL-3.0-or-later
#
# OnAir's clang cross SDKs, made with CMake: one target per SDK. A project of its own, because OnAir itself is configured
# with an SDK. It runs on Ubuntu 26.04 x86-64 (README.md):
#
#   cmake -S tools/sdk -B build-sdk -G Ninja
#   cmake --build build-sdk --target sdk_windows       # build-sdk/sdk_windows.tar.gz
#
# Targets: sdk_windows, sdk_linux_amd64, sdk_linux_arm64, sdk_macos_amd64, sdk_macos_arm64; sdk_all makes all five.
# Every build checks the SDK's inputs (cached_tarball.sh): the scripts, lock files, patches and licence texts in this folder,
# the options and the host packages. When one changed the SDK is made again from scratch; an SDK made before from the same
# inputs comes from the cache in seconds.
cmake_minimum_required(VERSION 3.20)
project(onair_sdk NONE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "The SDKs are made on Ubuntu 26.04 x86-64, not on ${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_PROCESSOR}.")
endif()
set(_onair_sdk_os "")
if(EXISTS /etc/os-release)
  file(STRINGS /etc/os-release _onair_sdk_os REGEX "^(ID|VERSION_ID)=")
endif()
if(NOT "ID=ubuntu" IN_LIST _onair_sdk_os OR NOT "VERSION_ID=\"26.04\"" IN_LIST _onair_sdk_os)
  message(FATAL_ERROR "The SDKs are made on Ubuntu 26.04 x86-64 (this machine: ${_onair_sdk_os}).")
endif()
execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_host.sh" RESULT_VARIABLE _onair_sdk_host)
if(NOT _onair_sdk_host EQUAL 0)
  message(FATAL_ERROR "Install the host tools above, then configure again.")
endif()

set(ONAIR_SDK_CACHE "${CMAKE_BINARY_DIR}/cache" CACHE PATH
    "The downloads, the built libraries and the finished SDKs, about 10 GB for all five (deleted with the build folder)")
option(ONAIR_SDK_KEEP_WORK "Keep the unpacked SDKs and the libraries' build folders, to see why a build failed" OFF)

set(_onair_sdk_keep "")
if(ONAIR_SDK_KEEP_WORK)
  set(_onair_sdk_keep --keep-work)   # (changes nothing in the SDK, so it is not part of the key)
endif()
set(_onair_sdk_all "")
foreach(platform windows linux_amd64 linux_arm64 macos_amd64 macos_arm64)
  # always run: cached_tarball.sh compares the inputs' key, and does nothing when the SDK has it
  add_custom_target(sdk_${platform}
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cached_tarball.sh" ${platform} -o "${CMAKE_BINARY_DIR}" --cache "${ONAIR_SDK_CACHE}" ${_onair_sdk_keep}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/sdk_${platform}.tar.gz" "${CMAKE_BINARY_DIR}/sdk_${platform}.key"
    USES_TERMINAL   # (the console pool: one SDK at a time, with its output as it goes)
    COMMENT "SDK ${platform}")
  list(APPEND _onair_sdk_all sdk_${platform})
endforeach()
add_custom_target(sdk_all DEPENDS ${_onair_sdk_all})
