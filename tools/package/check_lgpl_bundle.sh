#!/bin/bash
# Licence guard for the packages: fails if the FFmpeg OnAir ships is a GPL build, if the program uses another FFmpeg than the one shipped
# in the given folders, or if a GPL-only library is bundled next to the program.
#   tools/package/check_lgpl_bundle.sh <program> [library folder ...]
# FFmpeg is linked dynamically and shipped in a library folder (Windows the program's own folder, Linux lib/ffmpeg, macOS Contents/Frameworks):
# give that folder. The FFmpeg configuration string that libavcodec embeds says --enable-gpl / --enable-nonfree when so; it is read from every
# libavcodec in the folders (and from the program itself, for an older static build). The libraries the program links come from otool -L
# on macOS and readelf on Linux; for a Windows .exe the FFmpeg DLLs in the folders are checked.
set -u
PROG=${1:?usage: check_lgpl_bundle.sh <program> [library folder ...]}; shift
BAD='x264|x265|xvid|postproc|vidstab|rubberband|frei0r|cdio|davs2|xavs2|uavs3d|fdk-aac|fdk_aac|libsmbclient|libcaca|zimg'
FF='libav(codec|format|util|device|filter)|libsw(scale|resample)|(avcodec|avformat|avutil|avdevice|avfilter|swscale|swresample)-[0-9]+\.dll'
fail=0
gpl() {   # the FFmpeg configuration string in one file
  strings "$1" | grep -E -- '--enable-(gpl|nonfree|version3)' | grep -v -e 'without --enable' -e 'Source offer' | head -3 | grep .
}
if gpl "$PROG"; then echo "FAIL: $PROG contains an FFmpeg built with GPL / nonfree options"; fail=1; fi
codecs=
for d in "$@"; do
  [ -d "$d" ] || continue
  for f in $(find "$d" -maxdepth 2 -type f \( -name 'libavcodec*' -o -iname 'avcodec-*.dll' \) 2>/dev/null); do
    codecs="$codecs $f"
    if gpl "$f"; then echo "FAIL: $f is an FFmpeg built with GPL / nonfree options"; fail=1; fi
  done
done
kind=$(file -b "$PROG")
links=
case "$kind" in
  *Mach-O*) links=$(otool -L "$PROG" | tail -n +2) ;;
  *ELF*) links=$(readelf -d "$PROG" 2>/dev/null | grep NEEDED) ;;
esac
bundled=
for d in "$@"; do bundled="$bundled"$'\n'"$(find "$d" -maxdepth 2 2>/dev/null)"; done
if printf '%s\n%s\n' "$links" "$bundled" | grep -E -i "$BAD" | grep .; then echo "FAIL: a GPL-only library is linked or bundled"; fail=1; fi
# every FFmpeg library the program links must be one of the shipped ones: on macOS a path inside the app, on Linux a file in the folders
ffl=$(echo "$links" | grep -E "$FF")
if [ -n "$ffl" ]; then
  [ -n "$codecs" ] || { echo "FAIL: $PROG links FFmpeg, but no libavcodec is in the given folders ($*)"; fail=1; }
  while read -r l; do
    case "$kind" in
      *Mach-O*) l=${l%% (*}; l=${l#"${l%%[![:space:]]*}"}
                case "$l" in @executable_path/*|@loader_path/*|@rpath/*) ;; *) echo "FAIL: $PROG links an FFmpeg outside the app: $l"; fail=1;; esac ;;
      *ELF*) n=$(echo "$l" | sed -E 's/.*\[(.*)\].*/\1/')
             echo "$bundled" | grep -q "/$n\$" || { echo "FAIL: $PROG links $n, which is not in the given folders"; fail=1; } ;;
    esac
  done <<< "$ffl"
fi
case "$kind" in
  PE32*) [ -n "$codecs" ] || { echo "FAIL: no avcodec-*.dll in the given folders ($*): FFmpeg is not shipped"; fail=1; } ;;
esac
[ $fail = 0 ] && echo "licence check: FFmpeg is the LGPL build shipped with the program, no GPL-only libraries"
exit $fail
