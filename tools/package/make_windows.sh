#!/bin/bash
# Build OnAir for Windows (x86-64) in an MSYS2 MINGW64 shell and produce a portable .zip (and a setup .exe if NSIS is installed).
#   tools/package/make_windows.sh [version]
# Needs (pacman -S): mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,glfw,hackrf,soapysdr,libiconv,nsis} zip
set -e
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
BUILD=build-windows
OUT=$BUILD/OnAir-$VERSION-windows-x64
cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON
cmake --build $BUILD -j"$(nproc)" --target dect2 dect2cli dect2scan

rm -rf "$OUT" && mkdir -p "$OUT"
cp $BUILD/dect2.exe "$OUT/OnAir.exe"
cp $BUILD/dect2cli.exe $BUILD/dect2scan.exe "$OUT/"
# every DLL the programs need from the MSYS2 tree (FFmpeg, GLFW, libhackrf, libusb, the C++ runtime ...), found recursively
collect() {
  ldd "$1" | awk '/=> \/mingw64\//{print $3}' | while read -r dll; do
    [ -f "$OUT/$(basename "$dll")" ] || { cp "$dll" "$OUT/"; collect "$dll"; }
  done
}
collect "$OUT/OnAir.exe"; collect "$OUT/dect2cli.exe"; collect "$OUT/dect2scan.exe"
# SoapySDR plug-ins (generic radios) next to the program, when present
if [ -d /mingw64/lib/SoapySDR/modules* ]; then mkdir -p "$OUT/SoapySDR"; cp -r /mingw64/lib/SoapySDR/modules*/ "$OUT/SoapySDR/" 2>/dev/null || true; fi
cp LICENSE "$OUT/LICENSE.txt"
cat > "$OUT/README.txt" <<'TXT'
OnAir - DVB-T2 / DVB-T / ATSC / DAB receiver

Run OnAir.exe.
HackRF One / Pro: install the WinUSB driver once with Zadig (https://zadig.akeo.ie): pick the HackRF, driver "WinUSB", Install.
Other radios (RTL-SDR, Airspy, SDRplay, ...): they need their own SoapySDR driver; see the README on the project page.
TXT
(cd $BUILD && rm -f "OnAir-$VERSION-windows-x64.zip" && zip -qr "OnAir-$VERSION-windows-x64.zip" "OnAir-$VERSION-windows-x64")
echo "$BUILD/OnAir-$VERSION-windows-x64.zip"

if command -v makensis >/dev/null 2>&1; then
  makensis -DVERSION="$VERSION" -DSRC="$(cygpath -w "$PWD/$OUT")" -DOUTFILE="$(cygpath -w "$PWD/$BUILD/OnAir-$VERSION-windows-x64-setup.exe")" packaging/windows/onair.nsi
  echo "$BUILD/OnAir-$VERSION-windows-x64-setup.exe"
fi
