#!/bin/bash
# Builds a small shared FFmpeg with only what OnAir uses, shipped next to the program, so that the Linux packages need no FFmpeg from the system
# (system FFmpeg versions change with every Linux release and a package built against one does not install on the next).
# Shared, not static: a user can point OnAir at another FFmpeg build (Settings > Advanced > FFmpeg libraries, docs/ffmpeg-libraries.md).
#   tools/package/build_ffmpeg.sh <prefix> [ffmpeg-8.0.tar.xz]
# Used for the Linux packages and the macOS app; LGPL-2.1-or-later only: no --enable-gpl, --enable-version3 or --enable-nonfree, no external libraries
# (the script stops if the finished configuration says otherwise). Then build OnAir with -DDECT2_FFMPEG_PREFIX=<prefix>. Needs a C compiler, make and nasm (on x86-64); downloads the source when no tarball is given.
set -e
PREFIX=${1:?usage: build_ffmpeg.sh <prefix> [tarball]}
VER=8.0
TARBALL=${2:-}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ -z "$TARBALL" ]; then
  TARBALL=$WORK/ffmpeg-$VER.tar.xz
  curl -fsSL -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
fi
tar -xf "$TARBALL" -C "$WORK"
mkdir "$WORK/build" && cd "$WORK/build"
# Linux: no RUNPATH needed, the program names all five libraries itself and finds them through its own RUNPATH (or LD_LIBRARY_PATH).
# macOS: @rpath install names, found through the app's rpath (Contents/Frameworks) or DYLD_LIBRARY_PATH.
EXTRA=
if [ "$(uname -s)" = "Darwin" ]; then
  # hardware decoding through Apple's VideoToolbox (the system framework, no extra library); the deployment target is the app's
  EXTRA="--install-name-dir=@rpath --enable-videotoolbox --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,mpeg2_videotoolbox --extra-cflags=-mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET:-15.0} --extra-ldflags=-mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET:-15.0}"
fi
# FFMPEG_DISABLE_ASM=1: no assembly (slower; only for a quick local test on a machine without nasm, never for a release)
[ -n "${FFMPEG_DISABLE_ASM:-}" ] && EXTRA="$EXTRA --disable-x86asm"
[ -z "${FFMPEG_DISABLE_ASM:-}" ] && [ "$(uname -m)" = "x86_64" ] && ! command -v nasm >/dev/null && { echo "nasm is needed for the fast x86-64 code"; exit 1; }
"$WORK"/ffmpeg-$VER/configure --prefix="$PREFIX" --enable-shared --disable-static --enable-pic \
  --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
  --disable-avdevice --disable-avfilter \
  --disable-everything \
  --enable-protocol=file \
  --enable-demuxer=mpegts,mov \
  --enable-muxer=mpegts,hls,mp4 \
  --enable-parser=h264,hevc,mpegvideo,mpegaudio,aac,aac_latm,ac3,dvbsub,cavsvideo \
  --enable-decoder=h264,hevc,mpeg2video,mp2,mp2float,mp3,mp3float,ac3,eac3,aac,aac_latm,dvbsub,cavs \
  --enable-encoder=mpeg2video,mp2,aac \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata,null,setts,dts2pts \
  $EXTRA \
  > "$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log"; exit 1; }
# licence guard: the build must be LGPL (a GPL FFmpeg cannot be combined with GPL-3-only parts and drags in GPL libraries)
if grep -E '^#define (CONFIG_GPL|CONFIG_VERSION3|CONFIG_NONFREE) 1' config.h; then
  echo "FFmpeg was configured with GPL/version3/nonfree options: not allowed, OnAir ships the LGPL-2.1+ build only"; exit 1
fi
grep -m1 FFMPEG_CONFIGURATION config.h | sed -E 's/^#define FFMPEG_CONFIGURATION "(.*)"$/\1/' > "$WORK/configuration.txt"
make -j"${FFMPEG_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}" >/dev/null
make install >/dev/null
mkdir -p "$PREFIX/share"; cp "$WORK/configuration.txt" "$PREFIX/share/ffmpeg-configuration.txt"   # shipped with the app as part of the source offer
echo "FFmpeg $VER installed in $PREFIX"
