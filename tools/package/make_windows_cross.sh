#!/bin/bash
# Cross-compile OnAir for Windows x64 on macOS/Linux with mingw-w64 and make a zip. One self-contained program (no DLLs to ship).
#   tools/package/cross_deps_windows.sh   (once: builds FFmpeg, GLFW, libusb, libhackrf for Windows into ~/onair-win-prefix)
#   tools/package/make_windows_cross.sh
set -e
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
export ONAIR_WIN_PREFIX=${ONAIR_WIN_PREFIX:-$HOME/onair-win-prefix}
BUILD=build-windows
cmake --fresh -S . -B $BUILD "-DCMAKE_TOOLCHAIN_FILE=$PWD/packaging/windows/mingw-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON -DDECT2_WITH_SOAPY=ON -DONAIR_WIN_SOAPY=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
cmake --build $BUILD -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target dect2 dect2cli dect2scan
OUT=$BUILD/OnAir-$VERSION-windows-x64
rm -rf "$OUT" && mkdir -p "$OUT"
S=x86_64-w64-mingw32-strip
$S -o "$OUT/OnAir.exe" $BUILD/dect2.exe
$S -o "$OUT/onair-cli.exe" $BUILD/dect2cli.exe
$S -o "$OUT/onair-scan.exe" $BUILD/dect2scan.exe
# generic radios: SoapySDR with the RTL-SDR and Airspy drivers, and the C++ runtime they share with the program
P=$ONAIR_WIN_PREFIX
cp "$P/bin/libSoapySDR.dll" "$OUT/"
# the radios OnAir drives by itself: their libraries next to the program (nothing to install)
for d in librtlsdr libairspy libbladeRF libLimeSuite libiio; do [ -f "$P/bin/$d.dll" ] && cp "$P/bin/$d.dll" "$OUT/"; done
mkdir -p "$OUT/lib/SoapySDR/modules0.8"
cp "$P"/lib/SoapySDR/modules0.8/*.dll "$OUT/lib/SoapySDR/modules0.8/"
LIBDIR=$(dirname "$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)")
for f in "$LIBDIR/libstdc++-6.dll" "$LIBDIR/libgcc_s_seh-1.dll" "$LIBDIR/../bin/libwinpthread-1.dll"; do [ -f "$f" ] && cp "$f" "$OUT/" || echo "warning: $f not found"; done
for f in "$OUT"/*.dll "$OUT"/lib/SoapySDR/modules0.8/*.dll; do [ -f "$f" ] && $S --strip-unneeded "$f" 2>/dev/null; done   # debugging information makes libstdc++ alone 29 MB
python3 tools/package/scrub_paths.py "$OUT"   # no build-machine paths in the shipped files
cp LICENSE "$OUT/LICENSE.txt"
cat > "$OUT/README.txt" <<'TXT'
OnAir - DVB-T2 / DVB-T / ATSC / DAB receiver (Windows x64, needs a CPU with AVX2)

Start OnAir.exe. Nothing to install.
HackRF, RTL-SDR and Airspy radios: install the WinUSB driver once with Zadig (https://zadig.akeo.ie): choose the radio in the list, driver "WinUSB", Install.
Video is decoded on the graphics chip and the error correction runs on the GPU when the computer has a suitable one (switchable in the player panel).
No radio? The tour (first start, or the "Tour" button) plays a demo signal.
onair-cli.exe and onair-scan.exe are command-line tools (run them from a terminal).
TXT
(cd $BUILD && rm -f "OnAir-$VERSION-windows-x64.zip" && zip -qr "OnAir-$VERSION-windows-x64.zip" "OnAir-$VERSION-windows-x64")
ls -la "$BUILD/OnAir-$VERSION-windows-x64.zip"
# The installer: makensis from Homebrew crashes on some macOS versions, so a small Docker image with NSIS is used when it fails
NSI_ARGS="-DVERSION=$VERSION -DOUTFILE=$BUILD/OnAir-$VERSION-windows-x64-setup.exe"
if command -v makensis >/dev/null 2>&1 && makensis -V1 -CMDHELP >/dev/null 2>&1 && printf 'Name "t"\nOutFile "/tmp/onair-nsis-probe.exe"\nSection\nSectionEnd\n' > /tmp/onair-probe.nsi && makensis -V1 /tmp/onair-probe.nsi >/dev/null 2>&1; then
  makensis -V2 -DVERSION="$VERSION" -DSRC="$PWD/$OUT" -DICON="$PWD/packaging/windows/OnAir.ico" -DOUTFILE="$PWD/$BUILD/OnAir-$VERSION-windows-x64-setup.exe" packaging/windows/onair.nsi
elif command -v docker >/dev/null 2>&1; then
  docker image inspect onair-nsis >/dev/null 2>&1 || printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends nsis && rm -rf /var/lib/apt/lists/*\n' | docker build -q -t onair-nsis -
  docker run --rm -v "$PWD":/src -w /src onair-nsis makensis -V2 -DVERSION="$VERSION" -DSRC="/src/$OUT" -DICON=/src/packaging/windows/OnAir.ico -DOUTFILE="/src/$BUILD/OnAir-$VERSION-windows-x64-setup.exe" packaging/windows/onair.nsi
else
  echo "no working makensis or Docker: only the zip was made"
fi
ls -la "$BUILD"/OnAir-$VERSION-windows-x64*
