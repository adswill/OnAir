#!/usr/bin/env bash
# OnAir voice codec build helper for macOS and Linux.
# Run it yourself:  bash tools/codec-builder/build-codecs.sh
# It asks which plug-ins you want, shows a patent/licence notice, and fetches the upstream source ONLY after you type "yes".
# OnAir (the app and the project) hosts and ships no codec binary and no codec source. See docs/CODECS-BUILD-HELPER.md.
set -u

REPO_URL="https://github.com/arancormonk/mbelib-neo"
# Pinned upstream commit (mbelib-neo 2.6.2, GPL-2.0-or-later). The script refuses to build any other commit.
PIN_COMMIT="a3c7bbdae9eb21cbfab57006c1a5daca7bdcba7f"

# Non-interactive use (you have read the notice below and accept it):
#   bash build-codecs.sh --ids all --yes [--src <mbelib-neo source dir you already downloaded>] [--out <codecs folder>]
ARG_IDS=""; ARG_YES=""; ARG_SRC=""; ARG_OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ids) ARG_IDS="$2"; shift 2 ;;
    --yes) ARG_YES="yes"; shift ;;
    --src) ARG_SRC="$2"; shift 2 ;;
    --out) ARG_OUT="$2"; shift 2 ;;
    -h|--help) sed -n 2,5p "$0"; echo "Options: --ids all|\"tetra ambe2 ...\"  --yes  --src DIR  --out DIR"; exit 0 ;;
    *) echo "unknown option: $1"; exit 1 ;;
  esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
WRAPPER="$HERE/onair_codec_wrapper.c"

OS="$(uname -s)"
case "$OS" in
  Darwin) EXT="dylib"; OUTDIR="$HOME/Library/Application Support/OnAir/codecs"; SHARED="-dynamiclib" ;;
  Linux)  EXT="so";    OUTDIR="${XDG_DATA_HOME:-$HOME/.local/share}/onair/codecs"; SHARED="-shared" ;;
  *) echo "This script is for macOS and Linux. On Windows use build-codecs.ps1."; exit 1 ;;
esac

die() { echo "ERROR: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || MISSING="$MISSING $1"; }

[ -n "$ARG_OUT" ] && OUTDIR="$ARG_OUT"
echo "OnAir voice codec build helper"
echo "=============================="
echo
echo "Plug-ins you can build (each one decodes the voice of one or more modes):"
echo "  tetra  TETRA voice (unencrypted calls only)"
echo "  ambe2  AMBE+2 for DMR, NXDN, dPMR"
echo "  ambe   AMBE for D-STAR"
echo "  imbe   IMBE for P25"
echo "  ysf    Yaesu System Fusion (only the 7-byte V/D mode 2 frames)"
echo
if [ -n "$ARG_IDS" ]; then CHOICE="$ARG_IDS"; else
printf 'Which ones? Type ids separated by spaces, or "all": '
read -r CHOICE
fi
[ "$CHOICE" = "all" ] && CHOICE="tetra ambe2 ambe imbe ysf"
LIST=""
for id in $CHOICE; do
  case "$id" in
    tetra|ambe2|ambe|imbe|ysf) LIST="$LIST $id" ;;
    *) die "unknown plug-in id: $id" ;;
  esac
done
[ -n "$LIST" ] || die "nothing chosen"

cat <<EOF

IMPORTANT, please read.
 - These speech codecs (TETRA ACELP, IMBE, AMBE, AMBE+2) may be covered by patents in some countries.
 - Building and using them is your own choice and your own responsibility. Check the rules where you live,
   including the rules on listening to radio traffic. OnAir never decodes encrypted calls.
 - OnAir does not include, download or link these codecs, and the OnAir project hosts no codec files.
 - If you continue, THIS script (running on your computer, at your request) will download the source of mbelib-neo from
   $REPO_URL
   at commit $PIN_COMMIT. That source is GPL-2.0-or-later: you get it from its authors under their licence.
 - It will then compile it on your computer and copy the plug-in files into:
   $OUTDIR
 - Chosen plug-ins:$LIST
EOF
echo
if [ -n "$ARG_YES" ]; then ANSWER="yes"; echo "(--yes given: notice accepted)"; else
printf 'Type yes to continue (anything else stops here, nothing is downloaded): '
read -r ANSWER
fi
[ "$ANSWER" = "yes" ] || { echo "Stopped. Nothing was downloaded."; exit 0; }

MISSING=""
need cmake; need cc; [ -n "$ARG_SRC" ] || need git
if [ -n "$MISSING" ]; then
  echo "Missing tools:$MISSING"
  if [ "$OS" = "Darwin" ]; then echo "Install them with Xcode command line tools (xcode-select --install) and Homebrew (brew install cmake git)."
  else echo "Install them with your package manager, for example: sudo apt install git cmake build-essential"; fi
  echo "Nothing was installed or downloaded. Run this script again afterwards."
  exit 1
fi
[ -f "$WRAPPER" ] || die "onair_codec_wrapper.c not found next to this script ($HERE). Use the whole tools/codec-builder folder."

WORK="$(mktemp -d "${TMPDIR:-/tmp}/onair-codecs.XXXXXX")" || die "cannot make a work folder"
echo "Work folder: $WORK (you can delete it afterwards)"

if [ -n "$ARG_SRC" ]; then
  [ -f "$ARG_SRC/CMakeLists.txt" ] || die "--src $ARG_SRC is not an mbelib-neo source folder"
  echo "Using the mbelib-neo source you gave: $ARG_SRC (nothing is downloaded)"
  cp -R "$ARG_SRC" "$WORK/src" || die "cannot copy the source"
else
echo "Fetching mbelib-neo source ..."
git clone --quiet "$REPO_URL" "$WORK/src" || die "download failed"
git -C "$WORK/src" checkout --quiet "$PIN_COMMIT" || die "pinned commit not found"
[ "$(git -C "$WORK/src" rev-parse HEAD)" = "$PIN_COMMIT" ] || die "commit check failed"
fi

echo "Building mbelib-neo ..."
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF -DMBELIB_WARNINGS_AS_ERRORS=OFF >"$WORK/cmake.log" 2>&1 \
  || { tail -n 30 "$WORK/cmake.log"; die "cmake configure failed (log: $WORK/cmake.log)"; }
cmake --build "$WORK/build" >"$WORK/build.log" 2>&1 || { tail -n 30 "$WORK/build.log"; die "build failed (log: $WORK/build.log)"; }

LIB="$(find "$WORK/build" -name 'libmbe*static*.a' | head -n 1)"
[ -n "$LIB" ] || LIB="$(find "$WORK/build" -name 'libmbe*.a' | head -n 1)"
[ -n "$LIB" ] || die "static library not found in $WORK/build"
VER="$(find "$WORK/build" -name version.h -path '*mbelib-neo*' | head -n 1)"
[ -n "$VER" ] || die "generated version.h not found in $WORK/build"
GENINC="$(dirname "$(dirname "$VER")")"

mkdir -p "$OUTDIR" || die "cannot make $OUTDIR"
DONE=""
for id in $LIST; do
  OUT="$WORK/onair-codec-$id.$EXT"
  UPPER="$(echo "$id" | tr '[:lower:]' '[:upper:]')"
  echo "Building plug-in $id ..."
  # shellcheck disable=SC2086
  if cc $SHARED -fPIC -O2 -DCODEC_"$UPPER" -I"$WORK/src/include" -I"$GENINC" "$WRAPPER" "$LIB" -lm -o "$OUT" 2>"$WORK/cc-$id.log"; then
    cp "$OUT" "$OUTDIR/" && DONE="$DONE $id"
  else
    tail -n 20 "$WORK/cc-$id.log"; echo "  plug-in $id failed (see above)"
  fi
done

echo
if [ -n "$DONE" ]; then
  echo "Installed into $OUTDIR:$DONE"
  echo "Next: open OnAir, Help > Voice codecs, press Rescan, and accept the notice for each codec."
else
  echo "No plug-in was built."
fi
echo "You can delete $WORK now."
