#!/bin/bash
# Runs a Windows program of build-windows through Wine on this Mac (Rosetta runs the x64 code): CMAKE_CROSSCOMPILING_EMULATOR of the
# Windows build, so that ctest runs the Windows tests (tools/ci_local.sh). Wine: the Gcenx build that Homebrew's wine-stable uses,
# unpacked to ~/onair-wine (or ONAIR_WINE = the "Wine Stable.app"); its prefix is ~/onair-wine/prefix, made on the first run.
# The DLLs: the MinGW runtime of the compiler (libstdc++, libgcc_s, winpthread) and the radio libraries of ~/onair-win-prefix/bin.
APP="${ONAIR_WINE:-$HOME/onair-wine/Wine Stable.app}"
WINE="$APP/Contents/Resources/wine/bin/wine"
[ -x "$WINE" ] || { echo "no Wine at $APP (see tools/wine_test.sh)" >&2; exit 127; }
CXX=x86_64-w64-mingw32-g++
RT=$(dirname "$($CXX -print-file-name=libstdc++-6.dll)")
export WINEPREFIX="${WINEPREFIX:-$HOME/onair-wine/prefix}"
export WINEDEBUG=-all                         # Wine's own messages would end up in the test logs
export WINEDLLOVERRIDES="mscoree,mshtml="     # no .NET or browser engine: their installers would open windows
export WINEPATH="Z:$RT;Z:$RT/../bin;Z:${ONAIR_WIN_PREFIX:-$HOME/onair-win-prefix}/bin"
export DECT2_MUTE=1
exec "$WINE" "$@"
