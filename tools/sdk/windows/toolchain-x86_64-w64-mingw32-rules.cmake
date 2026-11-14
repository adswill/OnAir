# SPDX-License-Identifier: GPL-3.0-or-later
#
# Read by CMake through CMAKE_USER_MAKE_RULES_OVERRIDE (set in toolchain-x86_64-w64-mingw32.cmake), after its
# Windows-GNU platform defaults, so the value below is not overwritten by them.
#
# MSYS2's GCC links winpthreads by default (its driver adds -lpthread to every link: GCC is built with POSIX threads).
# clang's MinGW driver does not. libstdc++'s inline code (std::mutex, std::condition_variable, std::this_thread) then
# calls pthread_mutex_lock, nanosleep64, sched_yield ... straight from the object files, so a target that does not link
# Threads::Threads itself fails with "undefined reference to `pthread_mutex_lock'". Linking -lpthread as one of the
# standard libraries (after the target's own libraries) makes clang behave like MSYS2's GCC.
foreach(_lang C CXX)
  if(NOT " ${CMAKE_${_lang}_STANDARD_LIBRARIES_INIT} " MATCHES " -lpthread ")
    string(APPEND CMAKE_${_lang}_STANDARD_LIBRARIES_INIT " -lpthread")
  endif()
endforeach()
unset(_lang)
