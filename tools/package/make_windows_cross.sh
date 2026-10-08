#!/bin/bash
# Cross-compile OnAir for Windows x64 on macOS/Linux with mingw-w64 and make a zip and an installer.
#   tools/package/cross_deps_windows.sh   (once: builds FFmpeg, GLFW, libusb and the radio libraries for Windows into ~/onair-win-prefix)
#   tools/package/make_windows_cross.sh
# The folder is checked with check_win_dlls.sh before it is zipped: a missing or too old DLL stops the build.
# ONAIR_WIN_BUILD (default build-windows) is the CMake build folder, ONAIR_WIN_OUT (default <build>/OnAir-<version>-windows-x64) the folder that is
# zipped, ONAIR_WIN_NO_INSTALLER=1 skips the installer.
set -e
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
export ONAIR_WIN_PREFIX=${ONAIR_WIN_PREFIX:-$HOME/onair-win-prefix}
BUILD=${ONAIR_WIN_BUILD:-build-windows}
# Check the dependencies before the (long) compile: the README below promises these radios, so a prefix that lacks their libraries must stop
# the build (ONAIR_WIN_PARTIAL=1 makes a package without them, for a test).
P=$ONAIR_WIN_PREFIX
if [ -z "$ONAIR_WIN_PARTIAL" ]; then
  missing=""
  for f in libSoapySDR librtlsdr libairspy libairspyhf libbladeRF libLimeSuite libiio FTD3XX; do [ -f "$P/bin/$f.dll" ] || missing="$missing bin/$f.dll"; done
  for f in libuhd libusb-1.0 libstdc++-6 libgcc_s_seh-1 libwinpthread-1; do [ -f "$P/redist/$f.dll" ] || missing="$missing redist/$f.dll"; done
  [ -z "$missing" ] || { echo "error: not in $P:$missing"; echo "run tools/package/cross_deps_windows.sh first (or set ONAIR_WIN_PARTIAL=1 to package without them)"; exit 1; }
fi
cmake --fresh -S . -B $BUILD "-DCMAKE_TOOLCHAIN_FILE=$PWD/packaging/windows/mingw-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON -DDECT2_WITH_SOAPY=ON -DONAIR_WIN_SOAPY=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
cmake --build $BUILD -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target dect2 dect2cli dect2scan
OUT=${ONAIR_WIN_OUT:-$BUILD/OnAir-$VERSION-windows-x64}
ZIP=${OUT%/}.zip
SETUP=${OUT%/}-setup.exe
rm -rf "$OUT" && mkdir -p "$OUT"
S=x86_64-w64-mingw32-strip
$S -o "$OUT/OnAir.exe" $BUILD/dect2.exe
$S -o "$OUT/onair-cli.exe" $BUILD/dect2cli.exe
$S -o "$OUT/onair-scan.exe" $BUILD/dect2scan.exe
# generic radios: SoapySDR (a DLL). Its RTL-SDR and Airspy modules are not shipped: the native drivers of the same name always win and hide them.
cp "$P/bin/libSoapySDR.dll" "$OUT/"
# the radios OnAir drives by itself: their libraries next to the program (nothing to install). FTD3XX.dll is FTDI's USB 3 driver library that the
# LimeSDR Mini needs; it is signed by FTDI, so it is copied as it is (not stripped).
for d in librtlsdr libairspy libairspyhf libbladeRF libLimeSuite libiio FTD3XX; do if [ -f "$P/bin/$d.dll" ]; then cp "$P/bin/$d.dll" "$OUT/"; else echo "warning: $d.dll is missing in $P/bin (run cross_deps_windows.sh)"; fi; done
# UHD (USRP) with the Boost, Python and libusb DLLs it needs, and the GCC 16 C++ runtime (libstdc++, libgcc_s, libwinpthread) that all the DLLs share
for f in "$P"/redist/*.dll; do [ -f "$f" ] && cp "$f" "$OUT/"; done
# the C++ runtime of the compiler that built OnAir goes over the MSYS2 copy: the programs import functions (__cxa_thread_atexit) that the
# MSYS2 libstdc++ does not export, and the check below makes sure the MSYS2-built DLLs (UHD, Boost) still find all they need in it
LIBDIR=$(dirname "$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)")
for f in "$LIBDIR/libstdc++-6.dll" "$LIBDIR/libgcc_s_seh-1.dll" "$LIBDIR/../bin/libwinpthread-1.dll"; do [ -f "$f" ] && cp "$f" "$OUT/" || echo "warning: $f not found"; done
mkdir -p "$OUT/lib/SoapySDR/modules0.8"
for m in "$P"/lib/SoapySDR/modules0.8/*.dll; do
  case "$m" in *rtlsdrSupport.dll|*airspySupport.dll) continue;; esac   # (an older prefix may still hold them)
  [ -f "$m" ] && cp "$m" "$OUT/lib/SoapySDR/modules0.8/"
done
[ -d "$P/licenses" ] && { mkdir -p "$OUT/licenses"; cp -R "$P/licenses/." "$OUT/licenses/"; }
for f in "$OUT"/*.dll "$OUT"/lib/SoapySDR/modules0.8/*.dll; do
  [ -f "$f" ] || continue
  [ "$(basename "$f")" = FTD3XX.dll ] || $S --strip-unneeded "$f" 2>/dev/null   # debugging information makes libstdc++ alone 29 MB
done
python3 tools/package/scrub_paths.py "$OUT"   # no build-machine paths in the shipped files
tools/package/check_win_dlls.sh "$OUT" || { echo "error: the DLLs of $OUT do not fit together (see above)"; exit 1; }
cp LICENSE "$OUT/LICENSE.txt"
cat > "$OUT/README.txt" <<'TXT'
OnAir - DVB-T2 / DVB-T / ATSC / DAB receiver (Windows 10 or 11, 64-bit, needs a CPU with AVX2)

Start OnAir.exe. Nothing to install.
HackRF, RTL-SDR and Airspy radios: install the WinUSB driver once with Zadig (https://zadig.akeo.ie): choose the radio in the list, driver "WinUSB", Install.
Airspy HF+ radios: the same, WinUSB with Zadig.
SDRplay RSP radios: install the SDRplay API from sdrplay.com first (it brings the driver; no Zadig).
BladeRF radios: the library is included; the radio needs a WinUSB (or libusbK) driver: the installer from nuand.com brings one, or use Zadig.
USRP radios (Ettus): UHD is included; install its firmware and FPGA images once (uhd_images_downloader from the UHD package, files.ettus.com). USB models (B200/B210) also need a WinUSB driver (Zadig); network models (N2xx, X3xx) need nothing more.
LimeSDR Mini / Mini 2: the library is included; the radio needs FTDI's FT60x USB driver (Windows Update or ftdichip.com). LimeSDR-USB: install LimeSuite (limemicro.com or PothosSDR) first.
PlutoSDR: install the PlutoSDR Windows drivers from Analog Devices (wiki.analog.com), or connect to it over the network.
Video is decoded on the graphics chip and the error correction runs on the GPU when the computer has a suitable one (switchable in the player panel).
No radio? The tour (first start, or the "Tour" button) plays a demo signal.
onair-cli.exe and onair-scan.exe are command-line tools (run them from a terminal).
TXT
ABS_ZIP=$(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")   # before the cd below: $ZIP is relative to the repo
rm -f "$ABS_ZIP"; (cd "$(dirname "$OUT")" && zip -qr "$ABS_ZIP" "$(basename "$OUT")")
ls -la "$ZIP"
[ -n "$ONAIR_WIN_NO_INSTALLER" ] && exit 0
# The installer: makensis from Homebrew crashes on some macOS versions (std::bad_alloc, "Abort trap: 6"; its message comes from the shell, hence
# the braces), so a small Docker image with NSIS is used when it fails.
ABS_OUT=$(cd "$OUT" && pwd); ABS_SETUP=$(cd "$(dirname "$SETUP")" && pwd)/$(basename "$SETUP")
nsis_ok() { command -v makensis >/dev/null 2>&1 && makensis -V1 -CMDHELP >/dev/null 2>&1 && printf 'Name "t"\nOutFile "/tmp/onair-nsis-probe.exe"\nSection\nSectionEnd\n' > /tmp/onair-probe.nsi && makensis -V1 /tmp/onair-probe.nsi >/dev/null 2>&1; }
if { nsis_ok; } 2>/dev/null; then
  makensis -V2 -DVERSION="$VERSION" -DSRC="$ABS_OUT" -DICON="$PWD/packaging/windows/OnAir.ico" -DOUTFILE="$ABS_SETUP" packaging/windows/onair.nsi
elif command -v docker >/dev/null 2>&1; then
  docker image inspect onair-nsis >/dev/null 2>&1 || printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends nsis && rm -rf /var/lib/apt/lists/*\n' | docker build -q -t onair-nsis -
  # Colima / Docker Desktop only share some folders (the home folder) with the container; a mount of any other folder (e.g. /private/tmp, a
  # clone in a temporary folder) is EMPTY inside, and makensis then says "Can't open script packaging/windows/onair.nsi". Then everything the
  # installer needs is staged in a folder under $HOME (cloned copy-on-write on APFS).
  D_REPO=$PWD; D_OUT=$ABS_OUT; D_SETUP_DIR=$(dirname "$ABS_SETUP"); STAGE=""
  if ! docker run --rm -v "$D_REPO":/src onair-nsis test -f /src/packaging/windows/onair.nsi >/dev/null 2>&1 || ! docker run --rm -v "$D_OUT":/o onair-nsis test -f /o/OnAir.exe >/dev/null 2>&1 || ! docker run --rm -v "$D_SETUP_DIR":/o onair-nsis test -d /o >/dev/null 2>&1; then
    STAGE=$(mktemp -d "$HOME/.onair-nsis-stage.XXXXXX")
    mkdir -p "$STAGE/repo" "$STAGE/setup"
    cp -R packaging "$STAGE/repo/packaging"
    cp -cR "$ABS_OUT" "$STAGE/out" 2>/dev/null || cp -R "$ABS_OUT" "$STAGE/out"
    D_REPO=$STAGE/repo; D_OUT=$STAGE/out; D_SETUP_DIR=$STAGE/setup
    echo "note: Docker cannot see $PWD or the output folder; the installer is built from a copy in $STAGE"
  fi
  docker run --rm -v "$D_REPO":/src -v "$D_OUT":/onair-out -v "$D_SETUP_DIR":/onair-setup -w /src onair-nsis makensis -V2 -DVERSION="$VERSION" -DSRC=/onair-out -DICON=/src/packaging/windows/OnAir.ico -DOUTFILE="/onair-setup/$(basename "$ABS_SETUP")"  packaging/windows/onair.nsi || NSIS_RC=$?
  if [ -n "$STAGE" ]; then [ -f "$D_SETUP_DIR/$(basename "$ABS_SETUP")" ] && cp "$D_SETUP_DIR/$(basename "$ABS_SETUP")" "$ABS_SETUP"; rm -rf "$STAGE"; fi
  [ -z "$NSIS_RC" ] || { echo "error: the installer was not built (makensis exit $NSIS_RC)"; exit "$NSIS_RC"; }
else
  echo "no working makensis or Docker: only the zip was made"
fi
ls -la "$ZIP" "$SETUP" 2>&1 || true
