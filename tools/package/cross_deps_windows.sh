#!/bin/bash
# Cross-compile OnAir's dependencies for Windows x64 with mingw-w64 (macOS or Linux host). Result: ~/onair-win-prefix (override with ONAIR_WIN_PREFIX)
# Needs: x86_64-w64-mingw32-gcc, x86_64-w64-mingw32-objdump, cmake, nasm, make, pkg-config, patch, tar (with zstd) and the source archives in
# build-windows-deps/src (nothing is downloaded). The prebuilt MSYS2 UCRT64 packages in build-windows-deps/src/msys2 are only unpacked, never installed.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$ROOT/build-windows-deps
SRC=$W/src
P=${ONAIR_WIN_PREFIX:-$HOME/onair-win-prefix}   # no spaces in this path: the dependencies' configure scripts cannot cope with them
WORK=${ONAIR_WIN_WORK:-$HOME/onair-win-work}
HOST=x86_64-w64-mingw32
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mkdir -p "$P" "$WORK"
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH=
export ONAIR_WIN_PREFIX=$P
TC=$ROOT/packaging/windows/mingw-toolchain.cmake
cd "$WORK"
unpack() { [ -d "$2" ] || { case "$1" in *.zip) unzip -q "$SRC/$1";; *) tar xf "$SRC/$1";; esac; }; }

step() { echo; echo "=== $1"; }


[ -f "$P/lib/libusb-1.0.a" ] || { step libusb; unpack libusb-1.0.27.tar.bz2 libusb-1.0.27; (cd libusb-1.0.27 && ./configure --host=$HOST --prefix=$P --enable-static --disable-shared >/dev/null && make -j$JOBS >/dev/null && make install >/dev/null); }

# OpenSSL (Apache-2.0 from 3.0): HTTPS for onair-server (web_tls_cert) and the checked HTTPS call of the web.on-air.dev directory heartbeat. Built STATIC
# (libssl.a, libcrypto.a), so the programs need no extra DLL. Source: any openssl-3.*.tar.gz (the 3.0 LTS or newer) from https://www.openssl.org/source/
# put into build-windows-deps/src; check its SHA-256 against the one published next to it. The certificate authorities are read from the Windows
# root store at run time (core/src/net/tls_conn.cpp), so no bundle file is shipped.
[ -f "$P/lib/libssl.a" ] || { step openssl; OSSL=$(ls "$SRC"/openssl-3.*.tar.gz 2>/dev/null | sort -V | tail -1); [ -n "$OSSL" ] || { echo "error: no openssl-3.*.tar.gz in $SRC (https://www.openssl.org/source/)"; exit 1; }
  OD=$(basename "$OSSL" .tar.gz); unpack "$(basename "$OSSL")" "$OD"
  (cd "$OD" && ./Configure mingw64 --cross-compile-prefix=$HOST- no-shared no-tests no-docs no-apps no-module --prefix=$P --libdir=lib --openssldir=C:/OnAir-no-openssl-dir >/dev/null && make -j$JOBS build_libs >/dev/null && make install_dev >/dev/null)
  mkdir -p "$P/licenses"; cp "$OD/LICENSE.txt" "$P/licenses/openssl-LICENSE.txt"; [ -f "$OD/NOTICE.md" ] && cp "$OD/NOTICE.md" "$P/licenses/openssl-NOTICE.md" || true; }

[ -f "$P/lib/libglfw3.a" ] || { step glfw; unpack glfw-3.4.zip glfw-3.4; cmake -S glfw-3.4 -B glfw-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null && cmake --build glfw-build -j$JOBS >/dev/null && cmake --install glfw-build >/dev/null; }

[ -f "$P/lib/libhackrf.a" ] || { step libhackrf; unpack hackrf-2024.02.1.tar.xz hackrf-2024.02.1
  cmake -S hackrf-2024.02.1/host/libhackrf -B hackrf-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DLIBUSB_INCLUDE_DIR=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a -DCMAKE_C_FLAGS="-DLIBHACKRF_STATIC" >/dev/null
  cmake --build hackrf-build -j$JOBS --target hackrf-static >/dev/null 2>&1 || cmake --build hackrf-build -j$JOBS >/dev/null
  cmake --install hackrf-build >/dev/null 2>&1 || true
  # keep only the static library under its usual name
  [ -f "$P/lib/libhackrf.a" ] || cp hackrf-build/src/libhackrf_static.a "$P/lib/libhackrf.a" 2>/dev/null || cp $(find hackrf-build -name 'libhackrf*.a' | head -1) "$P/lib/libhackrf.a"
  [ -f "$P/include/libhackrf/hackrf.h" ] || { mkdir -p "$P/include/libhackrf"; cp hackrf-2024.02.1/host/libhackrf/src/hackrf.h "$P/include/libhackrf/"; }
  mkdir -p "$P/lib/pkgconfig"
  cat > "$P/lib/pkgconfig/libhackrf.pc" <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libhackrf
Description: HackRF library (static)
Version: 2024.02.1
Libs: -L\${libdir} -lhackrf -lusb-1.0 -lsetupapi -lole32
Cflags: -I\${includedir}/libhackrf -DLIBHACKRF_STATIC
PC
}

# libavdevice with only the DirectShow input: the cameras and capture cards (core/src/audio/videoin_ffmpeg.cpp). Without it CMake builds the camera stub and
# the program lists no camera at all. A prefix built before this has no libavdevice.a and is rebuilt.
[ -f "$P/lib/libavcodec.a" ] && [ -f "$P/lib/libavdevice.a" ] || { step ffmpeg; rm -rf ffmpeg-build; unpack ffmpeg-8.0.tar.xz ffmpeg-8.0; mkdir -p ffmpeg-build; (cd ffmpeg-build && ../ffmpeg-8.0/configure --prefix=$P --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix=$HOST- \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug --disable-indevs --enable-indev=dshow --disable-outdevs --disable-network --pkg-config=pkg-config --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib" >/dev/null && make -j$JOBS >/dev/null && grep -qE '^#define CONFIG_GPL 0' config.h && grep -qE '^#define CONFIG_VERSION3 0' config.h && grep -qE '^#define CONFIG_NONFREE 0' config.h && grep -qE '^#define CONFIG_DSHOW_INDEV 1' config_components.h && make install >/dev/null) || { echo "FFmpeg for Windows failed or is not the LGPL build (GPL/version3/nonfree are not allowed) or has no DirectShow camera input"; exit 1; }; }

# --- generic radios: SoapySDR (a DLL). Its RTL-SDR and Airspy modules are not built: the native drivers of the same name always win and hide them.

[ -f "$P/lib/librtlsdr.a" ] || { step librtlsdr; [ -f "$SRC/rtl-sdr-blog-1.4.0.tar.gz" ] || curl -fsSL -o "$SRC/rtl-sdr-blog-1.4.0.tar.gz" https://github.com/rtlsdrblog/rtl-sdr-blog/archive/aed0ea1.tar.gz   # 1.4.0 = commit aed0ea1: V4 Lite support, HF tracking filter bypass
  [ -d rtl-sdr-blog-1.4.0 ] || { mkdir rtl-sdr-blog-1.4.0 && tar xf "$SRC/rtl-sdr-blog-1.4.0.tar.gz" -C rtl-sdr-blog-1.4.0 --strip-components=1; }
  sed -i.bak 's/^set(VERSION_INFO_PATCH_VERSION git)/set(VERSION_INFO_PATCH_VERSION 0)/' rtl-sdr-blog-1.4.0/CMakeLists.txt   # (the tarball has no git history to take the version from)
  cmake -S rtl-sdr-blog-1.4.0 -B rtl-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVERSION=0.6.6 -DCMAKE_C_FLAGS="-std=gnu11" -DDETACH_KERNEL_DRIVER=OFF -DINSTALL_UDEV_RULES=OFF >/dev/null
  cmake --build rtl-build -j$JOBS --target rtlsdr_static >/dev/null
  cp rtl-build/src/librtlsdr_static.a "$P/lib/librtlsdr.a"; cp rtl-sdr-blog-1.4.0/include/rtl-sdr.h rtl-sdr-blog-1.4.0/include/rtl-sdr_export.h "$P/include/"
  cat > "$P/lib/pkgconfig/librtlsdr.pc" <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: librtlsdr
Description: RTL-SDR library (static)
Version: 0.6.6
Libs: -L\${libdir} -lrtlsdr -lusb-1.0
Cflags: -I\${includedir} -DRTLSDR_STATIC
Requires: libusb-1.0
PC
}

# libairspy 1.0.10 frees its USB transfers after ending libusb in airspy_close(), which crashes on Windows (patches/airspy-1.0.10-close-order.patch).
# The stamp makes a prefix built before the patch rebuild the library.
[ -f "$P/airspy-close-order.stamp" ] || { rm -rf "$P/lib/libairspy.a" "$P/bin/libairspy.dll" airspyone_host-1.0.10 airspy-build; }
[ -f "$P/lib/libairspy.a" ] || { step libairspy; unpack airspyone_host-1.0.10.tar.gz airspyone_host-1.0.10
  (cd airspyone_host-1.0.10 && patch -p1 -s < "$ROOT/tools/package/patches/airspy-1.0.10-close-order.patch")
  cmake -S airspyone_host-1.0.10/libairspy -B airspy-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS="-std=gnu11" -DLIBUSB_INCLUDE_DIR=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a >/dev/null
  cmake --build airspy-build -j$JOBS --target airspy-static >/dev/null
  mkdir -p "$P/include/libairspy"; cp airspyone_host-1.0.10/libairspy/src/airspy.h airspyone_host-1.0.10/libairspy/src/airspy_commands.h "$P/include/libairspy/"; cp airspy-build/src/libairspy.a "$P/lib/libairspy.a"
  cat > "$P/lib/pkgconfig/libairspy.pc" <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libairspy
Description: Airspy library (static)
Version: 1.0.10
Libs: -L\${libdir} -lairspy -lusb-1.0
Cflags: -I\${includedir}/libairspy
Requires: libusb-1.0
PC
}

[ -f "$P/bin/libSoapySDR.dll" ] || { step SoapySDR; unpack soapysdr-0.8.1.tar.gz SoapySDR-soapy-sdr-0.8.1
  cmake -S SoapySDR-soapy-sdr-0.8.1 -B soapy-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_TESTS=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON=OFF -DENABLE_PYTHON3=OFF -DENABLE_DOCS=OFF >/dev/null
  cmake --build soapy-build -j$JOBS >/dev/null && cmake --install soapy-build >/dev/null; }

# SoapyRemote client module (libremoteSupport.dll, issue #84 SoapySDR over LAN), BSL-1.0. Source: soapy-remote-0.5.3 (newest tag, 2026-10),
#   https://github.com/pothosware/SoapyRemote/archive/refs/tags/soapy-remote-0.5.3.tar.gz  (74563 bytes, put it in build-windows-deps/src/soapyremote-0.5.3.tar.gz)
#   SHA-256 de5bdf209dc93bf7c341077e1e21353037fc387e7c6a9ee41eace3390669dcc1
# The upstream sources only build with MSVC (_MSC_VER guards): the patch makes them build with MinGW. No mDNS (Avahi) on Windows; Winsock and IP Helper are system DLLs.
[ -f "$P/lib/SoapySDR/modules0.8/libremoteSupport.dll" ] || { step SoapyRemote; unpack soapyremote-0.5.3.tar.gz SoapyRemote-soapy-remote-0.5.3
  (cd SoapyRemote-soapy-remote-0.5.3 && patch -p1 -N -s < "$ROOT/tools/package/patches/soapyremote-0.5.3-mingw.patch" || true)
  cmake -S SoapyRemote-soapy-remote-0.5.3 -B soapyremote-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DENABLE_AVAHI=OFF -DENABLE_APPS=OFF "-DSoapySDR_DIR=$P/share/cmake/SoapySDR" "-DCMAKE_CXX_STANDARD_LIBRARIES=-liphlpapi -lws2_32" >/dev/null
  cmake --build soapyremote-build -j$JOBS >/dev/null && cmake --install soapyremote-build >/dev/null
  mkdir -p "$P/licenses"; cp SoapyRemote-soapy-remote-0.5.3/LICENSE_1_0.txt "$P/licenses/SoapyRemote-LICENSE_1_0.txt"; }

# --- librtlsdr and libairspy as DLLs of their own (libusb inside): the native drivers of OnAir load rtlsdr.dll / airspy.dll by name from the program's folder
[ -f "$P/bin/librtlsdr.dll" ] || { step "librtlsdr.dll"
  cmake rtl-build "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null && cmake --build rtl-build -j$JOBS --target rtlsdr >/dev/null && cp rtl-build/src/librtlsdr.dll "$P/bin/"; }
[ -f "$P/bin/libairspy.dll" ] || { step "libairspy.dll"
  cmake --build airspy-build -j$JOBS --target airspy >/dev/null; cp "$(find "$WORK" -name libairspy.dll -path '*src*' | head -1)" "$P/bin/libairspy.dll" && touch "$P/airspy-close-order.stamp"; }

# --- BladeRF and PlutoSDR (libbladeRF, libiio with libxml2): DLLs of their own, shipped next to OnAir.exe (the native drivers load them by name).
# Needs bladeRF-2023.02.tar.gz, no-OS-0bba46e.tar.gz (a part of the bladeRF source that git keeps as a submodule), libiio-0.25.tar.gz and
# libxml2-2.12.9.tar.xz in build-windows-deps/src.
[ -f "$P/lib/libxml2.a" ] || { step libxml2; unpack libxml2-2.12.9.tar.xz libxml2-2.12.9
  cmake -S libxml2-2.12.9 -B xml-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF >/dev/null
  cmake --build xml-build -j$JOBS >/dev/null && cmake --install xml-build >/dev/null; }
# libiio: USB and network backends (ip:192.168.2.1 or ip:pluto.local work when USB has no driver); the network backend needs libxml2 (above) and
# ws2_32/iphlpapi, which Windows has. The stamp file makes a prefix built with the old USB-only options rebuild it.
[ -f "$P/bin/libiio.dll" ] && [ -f "$P/lib/.onair-libiio-net" ] || { step libiio; unpack libiio-0.25.tar.gz libiio-0.25
  cmake -S libiio-0.25 -B iio-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWITH_USB_BACKEND=ON -DWITH_NETWORK_BACKEND=ON -DWITH_XML_BACKEND=ON -DHAVE_DNS_SD=ON -DWITH_LOCAL_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DWITH_ZSTD=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DENABLE_PACKAGING=OFF -DINSTALL_UDEV_RULE=OFF -DCPP_BINDINGS=OFF -DPYTHON_BINDINGS=OFF -DWITH_IIOD=OFF -DBUILD_SHARED_LIBS=ON -DLIBUSB_INCLUDE_DIR=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a -DLIBXML2_LIBRARIES=$P/lib/libxml2.a -DLIBXML2_INCLUDE_DIR=$P/include/libxml2 "-DCMAKE_C_FLAGS=-DLIBXML_STATIC" "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null
  cmake --build iio-build -j$JOBS >/dev/null && cp iio-build/libiio.dll "$P/bin/" && touch "$P/lib/.onair-libiio-net"; }
[ -f "$P/bin/libbladeRF.dll" ] || { step libbladeRF; unpack bladeRF-2023.02.tar.gz bladeRF-2023.02
  mkdir -p bladeRF-2023.02/thirdparty/analogdevicesinc/no-OS && tar xzf "$SRC/no-OS-0bba46e.tar.gz" -C bladeRF-2023.02/thirdparty/analogdevicesinc/no-OS --strip-components=1
  find bladeRF-2023.02/host \( -name CMakeLists.txt -o -name "*.cmake" \) -exec sed -i.bak 's/-Werror//g' {} \;   # a newer compiler than the one the project was written for warns about more
  cmake -S bladeRF-2023.02/host -B blade-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_BACKEND_LIBUSB=ON -DENABLE_BACKEND_CYPRESS=OFF -DBUILD_DOCUMENTATION=OFF -DENABLE_FX3_BUILD=OFF -DENABLE_HOST_BUILD=ON -DBUILD_BLADERF_CLI=OFF -DENABLE_UDEV_RULES=OFF -DINSTALL_UDEV_RULES=OFF -DBUILD_NATIVE=OFF -DLIBUSB_PATH=$P -DLIBUSB_INCLUDE_DIRS=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a "-DCMAKE_C_FLAGS=-std=gnu11" "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null
  cmake --build blade-build -j$JOBS --target libbladerf_shared >/dev/null && cp blade-build/output/libbladeRF.dll "$P/bin/"; }

# --- MSYS2 UCRT64 packages (prebuilt): UHD for USRP radios and the newer GCC 16 C++ runtime. Only unpacked here. Everything the UHD DLL imports that
# Windows does not have (Boost, Python, libusb) ships next to it, found by following the import tables. The runtime DLLs are shipped instead of the
# older ones of the cross compiler: check_win_dlls.sh proves that every function the programs import from them is there.
MS=$WORK/msys2; MB=$MS/ucrt64/bin
OBJDUMP=${OBJDUMP:-$HOST-objdump}
[ -f "$MB/libuhd.dll" ] && [ -f "$MB/libstdc++-6.dll" ] || { step "MSYS2 packages"; mkdir -p "$MS"
  for f in "$SRC"/msys2/*.pkg.tar.zst; do tar xf "$f" -C "$MS" 2>/dev/null || zstd -dc "$f" | tar xf - -C "$MS"; done; }
[ -f "$MB/libuhd.dll" ] || { echo "error: libuhd.dll is not in the MSYS2 packages ($SRC/msys2)"; exit 1; }
[ -f "$P/redist/libuhd.dll" ] && [ -f "$P/redist/libusb-1.0.dll" ] || { step "UHD and runtime DLLs"
  rm -rf "$P/redist"; mkdir -p "$P/redist"
  todo="libuhd.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll"; seen=" "
  while [ -n "$todo" ]; do
    set -- $todo; f=$1; shift; todo="$*"
    case "$seen" in *" $f "*) continue;; esac
    seen="$seen$f "
    [ -f "$MB/$f" ] || { echo "error: $f is needed but is not in the MSYS2 packages"; exit 1; }
    for d in $($OBJDUMP -p "$MB/$f" | awk '/DLL Name:/ {print tolower($3)}'); do [ -f "$MB/$d" ] && todo="$todo $d"; done   # (Windows' own DLLs are not in that folder)
  done
  for f in $seen; do cp "$MB/$f" "$P/redist/"; echo "  $f"; done
  # their licenses next to them (GCC runtime, Boost, libusb, Python, winpthreads)
  mkdir -p "$P/licenses"; cp -R "$MS"/ucrt64/share/licenses/. "$P/licenses/" 2>/dev/null || true; }

# --- LimeSuite 23.11.0 (LimeSDR): the library only, as libLimeSuite.dll. The LimeSDR Mini/Mini 2 talk through FTDI's FTD3XX.dll (from the tarball),
# shipped next to it. The LimeSDR-USB (Cypress FX3 / CyAPI: MSVC-only Cypress SDK, not redistributable here) is built without, so this copy
# cannot see a LimeSDR-USB on the Cypress (CyUSB3) driver; at run time OnAir then asks a vendor LimeSuite.dll (PothosSDR, LimeSuite
# installer) as well (source_limesdr.cpp, listLime). A release that wants FX3 itself must build LimeSuite with MSVC, -DENABLE_FX3=ON and
# the FX3 SDK (CyAPI.lib), and ship that LimeSuite.dll instead.
# The patch only makes the sources compile with MinGW.
[ -f "$P/bin/libLimeSuite.dll" ] && [ -f "$P/bin/FTD3XX.dll" ] || { step LimeSuite; unpack LimeSuite-23.11.0.tar.gz LimeSuite-23.11.0
  [ -f LimeSuite-23.11.0/.onair-patched ] || { for pf in "$ROOT"/tools/package/patches/limesuite-*.patch; do patch -s -d LimeSuite-23.11.0 -p1 < "$pf"; done; touch LimeSuite-23.11.0/.onair-patched; }
  cmake -S LimeSuite-23.11.0 -B lime-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DENABLE_SIMD_FLAGS=SSE3 -DLIME_SUITE_EXTVER=onair \
    -DENABLE_FX3=OFF -DFTD3XX_STATIC=OFF -DENABLE_PCIE_XILLYBUS=OFF -DENABLE_LIMERFE=OFF -DENABLE_GUI=OFF -DENABLE_SOAPY_LMS7=OFF -DENABLE_UTILITIES=OFF -DENABLE_EXAMPLES=OFF \
    -DENABLE_LIME_UTIL=OFF -DENABLE_QUICKTEST=OFF -DENABLE_OCTAVE=OFF -DENABLE_DESKTOP=OFF -DENABLE_HEADERS=OFF "-DCMAKE_C_FLAGS=-std=gnu11" >/dev/null   # (gnu11: the C code has old-style declarations)
  cmake --build lime-build -j$JOBS --target LimeSuite >/dev/null
  cp lime-build/src/libLimeSuite.dll "$P/bin/"; cp LimeSuite-23.11.0/src/ConnectionFTDI/FTD3XXLibrary/x64/FTD3XX.dll "$P/bin/"
  mkdir -p "$P/licenses"; cp LimeSuite-23.11.0/COPYING "$P/licenses/LimeSuite-COPYING.txt"
  for t in LimeSuite-23.11.0/src/ConnectionFTDI/FTD3XXLibrary/*icen*; do if [ -f "$t" ]; then cp "$t" "$P/licenses/FTD3XX-$(basename "$t")"; fi; done; }

# --- libairspyhf 1.6.8 (Airspy HF+): a DLL, linked to the libusb-1.0.dll that ships with UHD (one libusb, the newer MSYS2 one, for both)
# --- Hamlib 4.6.2 (CAT rig control): libhamlib-4.dll in bin/ and headers for the cross build. Needs hamlib-4.6.2.tar.gz in build-windows-deps/src.
_hamlib_ready() { [ -f "$P/lib/pkgconfig/hamlib.pc" ] && ls "$P/bin"/libhamlib*.dll "$P/bin"/hamlib.dll 2>/dev/null | grep -q .; }
if ! _hamlib_ready; then
  if [ -f "$SRC/hamlib-4.6.2.tar.gz" ]; then
    step hamlib; unpack hamlib-4.6.2.tar.gz hamlib-4.6.2
    cmake -S hamlib-4.6.2 -B hamlib-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DBUILD_SHARED_LIBS=ON -DENABLE_WINRADIO=OFF -DENABLE_PYTHON=OFF -DENABLE_PERL=OFF -DENABLE_CSHARP=OFF -DENABLE_JAVA=OFF -DENABLE_TESTING=OFF \
      -DENABLE_SHARED=ON -DENABLE_STATIC=OFF >/dev/null
    cmake --build hamlib-build -j$JOBS >/dev/null && cmake --install hamlib-build >/dev/null
    for d in "$P/bin"/libhamlib*.dll "$P/bin"/hamlib.dll; do [ -f "$d" ] && break; done || { echo "error: Hamlib install produced no DLL in $P/bin"; exit 1; }
    mkdir -p "$P/licenses"; cp hamlib-4.6.2/COPYING "$P/licenses/hamlib-COPYING.txt" 2>/dev/null || cp hamlib-4.6.2/COPYING.LIB "$P/licenses/hamlib-COPYING.txt" 2>/dev/null || true
  else
    echo "note: skip Hamlib (add hamlib-4.6.2.tar.gz to build-windows-deps/src to bundle it)"
  fi
fi

[ -f "$P/bin/libairspyhf.dll" ] || { step libairspyhf; unpack airspyhf-1.6.8.tar.gz airspyhf-1.6.8
  cmake -S airspyhf-1.6.8/libairspyhf -B airspyhf-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_C_FLAGS=-std=gnu11" -DLIBUSB_INCLUDE_DIR=$MS/ucrt64/include/libusb-1.0 -DLIBUSB_LIBRARIES=$MS/ucrt64/lib/libusb-1.0.dll.a >/dev/null
  cmake --build airspyhf-build -j$JOBS --target airspyhf >/dev/null && cp airspyhf-build/src/libairspyhf.dll "$P/bin/"
  mkdir -p "$P/licenses"; cp airspyhf-1.6.8/LICENSE "$P/licenses/airspyhf-LICENSE.txt"; }

step done
echo "$P"; ls "$P/lib"/*.a
