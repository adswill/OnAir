#!/bin/bash
# Licence guard for the packages: fails if the FFmpeg shipped is a GPL build, or a GPL-only library is bundled next to the program.
#   tools/package/check_lgpl_bundle.sh <program> [library folder ...]
# Looks at the FFmpeg configuration string that libavcodec embeds (in the program, and in the libavcodec shipped in the folders: FFmpeg is a
# shared library next to the program), at the libraries the program links (otool -L on macOS, readelf on Linux) and at every library file in
# the given folders (a Windows package: the program's own folder is searched as well).
set -u
PROG=${1:?usage: check_lgpl_bundle.sh <program> [library folder ...]}; shift
BAD='x264|x265|xvid|postproc|vidstab|rubberband|frei0r|cdio|davs2|xavs2|uavs3d|fdk-aac|fdk_aac|libsmbclient|libcaca|zimg'
fail=0
DIRS=("$@")
case "$PROG" in *.exe) DIRS+=("$(dirname "$PROG")") ;; esac
for f in "$PROG" $(for d in "${DIRS[@]}"; do ls "$d"/libavcodec* "$d"/avcodec-*.dll 2>/dev/null; done); do
  if strings "$f" | grep -E -- '--enable-(gpl|nonfree|version3)' | grep -v -e 'without --enable' -e 'Source offer' | head -3 | grep .; then
    echo "FAIL: $f is (or contains) an FFmpeg built with GPL / nonfree options"; fail=1
  fi
done
links=
case "$(file -b "$PROG")" in
  *Mach-O*) links=$(otool -L "$PROG" | tail -n +2) ;;
  *ELF*) links=$(readelf -d "$PROG" 2>/dev/null | grep NEEDED) ;;
esac   # (a Windows .exe: only the configuration string is checked)
for d in "${DIRS[@]}"; do links="$links"$'\n'"$(ls "$d" 2>/dev/null)"; done
if echo "$links" | grep -E -i "$BAD" | grep .; then echo "FAIL: a GPL-only library is linked or bundled"; fail=1; fi
# the FFmpeg libraries the program links must be the shipped ones, never Homebrew / system copies
if echo "$links" | grep -E '(/opt/homebrew|/usr/local|/usr/lib)/.*lib(av(codec|format|util)|sw(scale|resample))' | grep .; then echo "FAIL: the program links a Homebrew / system FFmpeg instead of the shipped LGPL build"; fail=1; fi
[ $fail = 0 ] && echo "licence check: FFmpeg is the LGPL build, no GPL-only libraries"
exit $fail
