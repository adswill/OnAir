#!/bin/sh
# Makes a synthetic ATSC 3.0 IQ recording for trying the receiver: a test picture with sound, fragmented like ROUTE sends it, then atsc3gen.
#   tools/dev/make_atsc3_sample.sh [seconds] [output.cs8]     (needs ffmpeg with libx265; build/atsc3gen from the normal build)
set -e
SECS=${1:-12}
OUT=${2:-$HOME/atsc3_sample.cs8}
BUILD=${BUILD:-build}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=25" -t "$SECS" -c:v libx265 -preset fast -crf 27 \
  -x265-params "keyint=25:min-keyint=25:open-gop=0:log-level=error" -pix_fmt yuv420p -tag:v hvc1 \
  -movflags frag_keyframe+empty_moov+default_base_moof+separate_moof "$TMP/video.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=440:sample_rate=48000" -f lavfi -i "sine=frequency=660:sample_rate=48000" -t "$SECS" \
  -filter_complex "[0][1]amerge=inputs=2,volume=0.4" -c:a aac -b:a 96k -movflags frag_keyframe+empty_moov+default_base_moof -frag_duration 1000000 "$TMP/audio.mp4"
"$BUILD/atsc3gen" --video "$TMP/video.mp4" --audio "$TMP/audio.mp4" --out "$OUT" --rate 10e6 --cfo 2500 --snr 25 --echo-us 2 --echo-db -12
echo
echo "Open it in OnAir: source 'IQ recording file', format CS8, sample rate 10 Msps, ATSC 3.0 selected."
