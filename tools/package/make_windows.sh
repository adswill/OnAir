#!/bin/bash
# Build OnAir for Windows (x86-64) in an MSYS2 UCRT64 shell and produce a portable .zip (and a setup .exe if NSIS is installed).
#   tools/package/make_windows.sh [version]
# Needs (pacman -S): mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,glfw,hackrf,soapysdr,libiconv} zip, and nsis for the installer.
# Radio libraries that pacman has (any of rtl-sdr, airspyone_host, airspyhf, bladerf, libiio, limesuite, uhd) are shipped too, with what they need.
# (The MINGW64 environment has no hackrf and soapysdr packages: use UCRT64.)
# From macOS or Linux, tools/package/make_windows_cross.sh builds the same package with a cross-compiler.
set -e
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
BUILD=build-windows
OUT=$BUILD/OnAir-$VERSION-windows-x64
PREFIX=${MSYSTEM_PREFIX:-/ucrt64}
cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON
cmake --build $BUILD -j"$(nproc)" --target dect2 dect2cli dect2scan

rm -rf "$OUT" && mkdir -p "$OUT"
strip -o "$OUT/OnAir.exe" $BUILD/dect2.exe
strip -o "$OUT/onair-cli.exe" $BUILD/dect2cli.exe
strip -o "$OUT/onair-scan.exe" $BUILD/dect2scan.exe
# every DLL the programs need from the MSYS2 tree (FFmpeg, GLFW, libhackrf, libusb, the C++ runtime ...), found recursively
collect() {
  ldd "$1" | awk -v p="$PREFIX/" 'index($3, p) == 1 {print $3}' | while read -r dll; do
    [ -f "$OUT/$(basename "$dll")" ] || { cp "$dll" "$OUT/"; collect "$dll"; }
  done
}
collect "$OUT/OnAir.exe"; collect "$OUT/onair-cli.exe"; collect "$OUT/onair-scan.exe"
# the radios OnAir drives by itself load their libraries at run time (nothing links them), so they are copied here
for d in librtlsdr libairspy libairspyhf libbladeRF libLimeSuite libiio libuhd; do
  [ -f "$PREFIX/bin/$d.dll" ] && { cp "$PREFIX/bin/$d.dll" "$OUT/"; collect "$OUT/$d.dll"; }
done
# SoapySDR drivers (generic radios): the program looks for them in lib/SoapySDR/modules0.8 next to it. Not those of RTL-SDR and Airspy: the native drivers win over them.
for m in "$PREFIX"/lib/SoapySDR/modules*/; do
  [ -d "$m" ] || continue
  mkdir -p "$OUT/lib/SoapySDR/modules0.8"
  for f in "$m"*.dll; do
    case "$f" in *rtlsdrSupport.dll|*airspySupport.dll) continue;; esac
    [ -f "$f" ] && cp "$f" "$OUT/lib/SoapySDR/modules0.8/"
  done
  for d in "$OUT"/lib/SoapySDR/modules0.8/*.dll; do [ -f "$d" ] && collect "$d"; done
done
tools/package/check_win_dlls.sh "$OUT" || { echo "error: the DLLs of $OUT do not fit together (see above)"; exit 1; }
cp LICENSE "$OUT/LICENSE.txt"
cat > "$OUT/README.txt" <<'TXT'
OnAir - DVB-T2 / DVB-T / ATSC / DAB receiver (Windows x64, needs a CPU with AVX2)

Start OnAir.exe. Nothing to install.
HackRF, RTL-SDR and Airspy radios: install the WinUSB driver once with Zadig (https://zadig.akeo.ie): choose the radio in the list, driver "WinUSB", Install.
Airspy HF+ radios: the same, WinUSB with Zadig.
SDRplay RSP radios: install the SDRplay API from sdrplay.com first (it brings the driver; no Zadig).
USRP radios (Ettus): UHD is included; install its firmware and FPGA images once (uhd_images_downloader from the UHD package, files.ettus.com).
LimeSDR Mini / Mini 2: the library is included; the radio needs FTDI's FT60x USB driver (Windows Update or ftdichip.com). LimeSDR-USB: install LimeSuite (limemicro.com or PothosSDR) first.
PlutoSDR: install the PlutoSDR Windows drivers from Analog Devices (wiki.analog.com), or connect to it over the network.
Video is decoded on the graphics chip and the error correction runs on the GPU when the computer has a suitable one (switchable in the player panel).
No radio? The tour (first start, or the "Tour" button) plays a demo signal.
onair-cli.exe and onair-scan.exe are command-line tools (run them from a terminal).
TXT
(cd $BUILD && rm -f "OnAir-$VERSION-windows-x64.zip" && zip -qr "OnAir-$VERSION-windows-x64.zip" "OnAir-$VERSION-windows-x64")
echo "$BUILD/OnAir-$VERSION-windows-x64.zip"

if command -v makensis >/dev/null 2>&1; then
  # Windows paths with forward slashes, and no MSYS path conversion of the arguments (it breaks the -D definitions)
  export MSYS2_ARG_CONV_EXCL="*"
  makensis -V2 -DVERSION="$VERSION" -DSEP="\\" -DSRC="$(cygpath -w "$PWD/$OUT")" -DICON="$(cygpath -m "$PWD/packaging/windows/OnAir.ico")" -DOUTFILE="$(cygpath -m "$PWD/$BUILD/OnAir-$VERSION-windows-x64-setup.exe")" "$(cygpath -m "$PWD/packaging/windows/onair.nsi")"
  echo "$BUILD/OnAir-$VERSION-windows-x64-setup.exe"
else
  echo "makensis not found: only the zip was made"
fi
