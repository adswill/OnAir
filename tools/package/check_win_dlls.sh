#!/bin/bash
# Checks a Windows folder (the one that is zipped and installed) without running anything: for every .exe and .dll in it,
#   1. each DLL it imports is in the folder (next to the file or in the top folder) or is one Windows has itself, and
#   2. each function it imports from a DLL of the folder is exported by that DLL (this is what proves that the newer libstdc++, libgcc_s,
#      libwinpthread and libusb DLLs can stand in for the older ones the programs were linked against).
# One line per problem on stdout, exit 1 if there is any. Works on a Mac/Linux host (mingw-w64 objdump) and inside MSYS2 (objdump).
#   tools/package/check_win_dlls.sh <folder>
# What it cannot show: that a DLL starts (DllMain), that data (not functions) it imports exists, or anything about delay-loaded or
# run-time (LoadLibrary) dependencies of libraries that are not in the folder. Only a run on Windows shows those.
set -e
export LC_ALL=C   # one sort order for sort and comm
DIR=${1:?usage: check_win_dlls.sh <folder>}
[ -d "$DIR" ] || { echo "not a folder: $DIR"; exit 2; }
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || OBJDUMP=objdump
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "no objdump found (install mingw-w64 binutils)"; exit 2; }

# DLLs every Windows has, plus api-ms-win-* (the universal C runtime forwarders)
SYSTEM=" $(echo kernel32 user32 gdi32 advapi32 shell32 ole32 oleaut32 ws2_32 wsock32 iphlpapi setupapi winusb cfgmgr32 crypt32 bcrypt ntdll msvcrt ucrtbase \
    opengl32 d3d11 dxgi d3dcompiler_47 dbghelp comdlg32 imm32 winmm version shlwapi secur32 userenv mswsock dwmapi uxtheme hid avrt mfplat ksuser \
    propsys powrprof) "
is_system() {
    case "$1" in api-ms-win-*) return 0;; esac
    case "$SYSTEM" in *" ${1%.dll} "*) return 0;; esac
    return 1
}

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
FILES=$T/files; find "$DIR" -type f \( -iname '*.exe' -o -iname '*.dll' \) | sort > "$FILES"
[ -s "$FILES" ] || { echo "no .exe or .dll in $DIR"; exit 2; }

# imports of one file: "dll function" per line (dll lowercase); exports of one file: one name per line
imports() {
    "$OBJDUMP" -p "$1" | awk '
        /^[A-Za-z]/ { cur = "" }
        /DLL Name:/ { cur = tolower($NF); next }
        cur != "" && /^[ \t]*[0-9a-fA-F]+[ \t]+/ {
            if ($2 == "<none>") name = $4; else name = $3
            if (name ~ /^[A-Za-z_?@][^ ]*$/) print cur, name
        }'
}
exports() {
    "$OBJDUMP" -p "$1" | awk '
        /\+base\[/ && NF >= 2 && $(NF-1) ~ /^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$/ { print $NF }' | sort -u
}

# exports of every DLL in the folder, by lowercase name
mkdir -p "$T/exp"
while read -r f; do
    b=$(basename "$f" | tr 'A-Z' 'a-z')
    case "$b" in *.dll) exports "$f" > "$T/exp/$b";; esac
done < "$FILES"

bad=0
problem() { echo "PROBLEM $*"; bad=$((bad + 1)); }
# the copy of a DLL that the file would load: next to it, else in the top folder (Windows looks in the program's own folder)
locate() {
    local dir=$1 want=$2 f
    for f in "$dir"/* "$DIR"/*; do
        [ -f "$f" ] && [ "$(basename "$f" | tr 'A-Z' 'a-z')" = "$want" ] && { echo "$f"; return 0; }
    done
    return 1
}

n=0
while read -r f; do
    n=$((n + 1)); rel=${f#$DIR/}
    imports "$f" | sort -u > "$T/imp"
    for d in $(awk '{print $1}' "$T/imp" | sort -u); do
        if [ -f "$T/exp/$d" ] && locate "$(dirname "$f")" "$d" >/dev/null; then
            # in the folder: every function taken from it must be exported by it
            missing=$(awk -v d="$d" '$1 == d {print $2}' "$T/imp" | sort -u | comm -23 - "$T/exp/$d")
            if [ -n "$missing" ]; then
                cnt=$(echo "$missing" | wc -l | tr -d ' ')
                problem "$rel imports $cnt function(s) that $d of this folder does not export: $(echo "$missing" | head -4 | tr '\n' ' ')"
            fi
        elif is_system "$d"; then
            :
        else
            problem "$rel imports $d, which is neither in the folder nor a Windows system DLL"
        fi
    done
done < "$FILES"

if [ $bad -gt 0 ]; then echo "$bad problem(s) in $n files of $DIR"; exit 1; fi
echo "OK: $n .exe/.dll files in $DIR, all imported DLLs are present or part of Windows, all imported functions from the shipped DLLs exist"
