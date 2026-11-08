#!/bin/bash
# Licence guard for the packages: fails if FFmpeg inside is a GPL build, or a GPL-only library is bundled next to the program.
#   tools/package/check_lgpl_bundle.sh <program> [library folder ...]
# Looks at the FFmpeg configuration string that libavcodec embeds in the program (it says --enable-gpl / --enable-nonfree when so),
# at the libraries the program links (otool -L on macOS, ldd/readelf on Linux) and at every library file in the given folders.
set -u
PROG=${1:?usage: check_lgpl_bundle.sh <program> [library folder ...]}; shift
BAD='x264|x265|xvid|postproc|vidstab|rubberband|frei0r|cdio|davs2|xavs2|uavs3d|fdk-aac|fdk_aac|libsmbclient|libcaca|zimg'
fail=0
if strings "$PROG" | grep -E -- '--enable-(gpl|nonfree|version3)' | grep -v -e 'without --enable' -e 'Source offer' | head -3 | grep .; then
  echo "FAIL: $PROG contains an FFmpeg built with GPL / nonfree options"; fail=1
fi
links=
case "$(file -b "$PROG")" in
  *Mach-O*) OT=$(command -v otool || command -v llvm-otool) || { echo "FAIL: no otool or llvm-otool, $PROG cannot be checked"; exit 1; }
            links=$("$OT" -L "$PROG" | tail -n +2) ;;
  *ELF*) links=$(readelf -d "$PROG" 2>/dev/null | grep NEEDED) ;;
esac   # (a Windows .exe: only the configuration string is checked)
for d in "$@"; do links="$links"$'\n'"$(ls "$d" 2>/dev/null)"; done
if echo "$links" | grep -E -i "$BAD" | grep .; then echo "FAIL: a GPL-only library is linked or bundled"; fail=1; fi
# the FFmpeg libraries must be inside the program (static), never separate Homebrew / system copies
if echo "$links" | grep -E 'libav(codec|format|util)|libsw(scale|resample)' | grep .; then echo "FAIL: separate FFmpeg libraries are used instead of the LGPL static build"; fail=1; fi
[ $fail = 0 ] && echo "licence check: FFmpeg is the LGPL static build, no GPL-only libraries"
exit $fail
