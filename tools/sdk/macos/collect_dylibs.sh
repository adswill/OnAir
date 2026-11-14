#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# collect_dylibs.sh - copy the dylibs a macOS program needs from the sysroot into a folder, recursively: every
# @rpath/<name> it loads (conda-forge's libraries, libc++) from <sysroot>/usr/local/lib. macOS's own libraries
# (/usr/lib, /System/Library) are left. Give the program an rpath to that folder (e.g. -Wl,-rpath,@executable_path/../Frameworks).
#
#   collect_dylibs.sh <program or dylib> <dest folder> [<sysroot>] [<extra dylib name> ...]
#     <sysroot>   default: $MAC_SYSROOT
#     extra       dylibs to copy as well, with what they need (loaded at run time with dlopen): a glob such as 'libSoapy*'
set -euo pipefail
[ $# -ge 2 ] || { sed -n '4,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
EXE=$1 DEST=$2; shift 2
SYSROOT=${1:-${MAC_SYSROOT:-}}; [ $# -gt 0 ] && shift
[ -n "$SYSROOT" ] && [ -d "$SYSROOT/usr/local/lib" ] || { echo "give the sysroot as the third argument (or set MAC_SYSROOT)" >&2; exit 1; }
exec python3 -I "$(cd "$(dirname "$0")" && pwd)/macho.py" collect "$EXE" "$DEST" "$SYSROOT" "$@"
