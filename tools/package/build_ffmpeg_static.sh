#!/bin/bash
# Builds a small static FFmpeg with only what OnAir uses, so that the Linux packages need no FFmpeg from the system (system FFmpeg
# versions change with every Linux release and a package built against one does not install on the next).
#   tools/package/build_ffmpeg_static.sh <prefix> [ffmpeg-8.0.tar.xz]
# Then build OnAir with -DDECT2_FFMPEG_PREFIX=<prefix>. Needs a C compiler, make and nasm (on x86-64); downloads the source when no tarball is given.
set -e
PREFIX=${1:?usage: build_ffmpeg_static.sh <prefix> [tarball]}
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
ASM=
[ "$(uname -m)" = "x86_64" ] && ! command -v nasm >/dev/null && { echo "nasm is needed for the fast x86-64 code"; exit 1; }
"$WORK"/ffmpeg-$VER/configure --prefix="$PREFIX" --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
  --disable-avdevice --disable-avfilter \
  --disable-everything \
  --enable-protocol=file \
  --enable-demuxer=mpegts \
  --enable-muxer=mpegts,hls \
  --enable-parser=h264,hevc,mpegvideo,mpegaudio,aac,aac_latm,ac3,dvbsub \
  --enable-decoder=h264,hevc,mpeg2video,mp2,mp2float,mp3,mp3float,ac3,eac3,aac,aac_latm,dvbsub \
  --enable-encoder=mpeg2video,mp2,aac \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata,null,setts,dts2pts \
  > "$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log"; exit 1; }
make -j"${FFMPEG_JOBS:-$(nproc)}" >/dev/null
make install >/dev/null
echo "FFmpeg $VER installed in $PREFIX"
