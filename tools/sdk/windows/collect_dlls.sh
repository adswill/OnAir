#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# collect_dlls.sh - copy the DLLs a Windows program needs from the sysroot next to it, recursively, so it starts on a
# Windows PC without MSYS2. Windows' own DLLs (kernel32, ucrtbase's api-ms-win-*, ...) are not in the sysroot and are left.
#
#   collect_dlls.sh <program.exe or .dll> [<sysroot>] [<extra.dll name> ...]
#     <sysroot>   default: $WIN_SYSROOT, else sysroots/x86_64-w64-mingw32 next to this script (the SDK)
#     extra       DLLs to copy as well, with what they need: those the program loads at run time and so not in its import
#                 table, e.g. the radio DLLs OnAir's native drivers load by name (librtlsdr.dll ...); a glob such as
#                 'libhamlib-*.dll' works
# Needs x86_64-w64-mingw32-objdump (binutils-mingw-w64-x86-64). GNU objdump also lists the delay-import table.
set -euo pipefail
[ $# -ge 1 ] || { sed -n '4,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
EXE=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT=${1:-${WIN_SYSROOT:-$HERE/sysroots/x86_64-w64-mingw32}}; [ $# -gt 0 ] && shift
BIN=$SYSROOT/bin; DEST=$(dirname "$EXE")
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
[ -d "$BIN" ] || { echo "no $BIN: give the sysroot as the second argument" >&2; exit 1; }
command -v "$OBJDUMP" >/dev/null || { echo "$OBJDUMP not found (apt install binutils-mingw-w64-x86-64)" >&2; exit 1; }

# the DLL names in the sysroot, by lower-case name (Windows matches DLL names without case)
declare -A IN_BIN
for f in "$BIN"/*.dll; do b=$(basename "$f"); IN_BIN[${b,,}]=$b; done

declare -A DONE
copied=0
walk() {   # $1: a PE file; copy every DLL it imports that the sysroot has, then walk that one too
  local dll key
  while read -r dll; do
    key=${dll,,}
    [ -z "${DONE[$key]:-}" ] || continue
    DONE[$key]=1
    [ -n "${IN_BIN[$key]:-}" ] || continue          # a Windows system DLL
    if [ ! -f "$DEST/${IN_BIN[$key]}" ] || [ "$BIN/${IN_BIN[$key]}" -nt "$DEST/${IN_BIN[$key]}" ]; then
      cp "$BIN/${IN_BIN[$key]}" "$DEST/"; copied=$((copied + 1))
    fi
    walk "$BIN/${IN_BIN[$key]}"
  done < <("$OBJDUMP" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: *//p' | tr -d '\r')
}
walk "$EXE"
for pat in "$@"; do
  for f in "$BIN"/$pat; do
    [ -f "$f" ] || { echo "warning: no $pat in $BIN" >&2; continue; }
    b=$(basename "$f")
    [ -n "${DONE[${b,,}]:-}" ] || { DONE[${b,,}]=1; cp "$f" "$DEST/"; copied=$((copied + 1)); }
    walk "$f"
  done
done
echo "$copied DLLs copied to $DEST ($(ls "$DEST"/*.dll 2>/dev/null | wc -l) DLLs there now)"
