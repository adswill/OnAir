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

[ -f "$P/lib/libavcodec.a" ] || { step ffmpeg; unpack ffmpeg-8.0.tar.xz ffmpeg-8.0; mkdir -p ffmpeg-build; (cd ffmpeg-build && ../ffmpeg-8.0/configure --prefix=$P --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix=$HOST- \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-network --pkg-config=pkg-config --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib" >/dev/null && make -j$JOBS >/dev/null && make install >/dev/null); }

# --- generic radios: SoapySDR (a DLL). Its RTL-SDR and Airspy modules are not built: the native drivers of the same name always win and hide them.

[ -f "$P/lib/librtlsdr.a" ] || { step librtlsdr; unpack rtl-sdr-blog-1.3.6.tar.gz rtl-sdr-blog-1.3.6
  sed -i.bak 's/^set(VERSION_INFO_PATCH_VERSION git)/set(VERSION_INFO_PATCH_VERSION 6)/' rtl-sdr-blog-1.3.6/CMakeLists.txt   # (the tarball has no git history to take the version from)
  cmake -S rtl-sdr-blog-1.3.6 -B rtl-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVERSION=0.6.6 -DCMAKE_C_FLAGS="-std=gnu11" -DDETACH_KERNEL_DRIVER=OFF -DINSTALL_UDEV_RULES=OFF >/dev/null
  cmake --build rtl-build -j$JOBS --target rtlsdr_static >/dev/null
  cp rtl-build/src/librtlsdr_static.a "$P/lib/librtlsdr.a"; cp rtl-sdr-blog-1.3.6/include/rtl-sdr.h rtl-sdr-blog-1.3.6/include/rtl-sdr_export.h "$P/include/"
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
# shipped next to it. The LimeSDR-USB (Cypress FX3 / CyAPI, not available) is built without: those users keep a LimeSuite installed by the vendor.
# The patch only makes the sources compile with MinGW.
[ -f "$P/bin/libLimeSuite.dll" ] && [ -f "$P/bin/FTD3XX.dll" ] || { step LimeSuite; unpack LimeSuite-23.11.0.tar.gz LimeSuite-23.11.0
  [ -f LimeSuite-23.11.0/.onair-patched ] || { for pf in "$ROOT"/tools/package/patches/limesuite-*.patch; do patch -s -d LimeSuite-23.11.0 -p1 < "$pf"; done; touch LimeSuite-23.11.0/.onair-patched; }
  cmake -S LimeSuite-23.11.0 -B lime-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DENABLE_SIMD_FLAGS=SSE3 -DLIME_SUITE_EXTVER=onair \
    -DENABLE_FX3=OFF -DFTD3XX_STATIC=OFF -DENABLE_PCIE_XILLYBUS=OFF -DENABLE_LIMERFE=OFF -DENABLE_GUI=OFF -DENABLE_SOAPY_LMS7=OFF -DENABLE_UTILITIES=OFF -DENABLE_EXAMPLES=OFF \
    -DENABLE_LIME_UTIL=OFF -DENABLE_QUICKTEST=OFF -DENABLE_OCTAVE=OFF -DENABLE_DESKTOP=OFF -DENABLE_HEADERS=OFF "-DCMAKE_C_FLAGS=-std=gnu11" >/dev/null   # (gnu11: the C code has old-style declarations)
  cmake --build lime-build -j$JOBS --target LimeSuite >/dev/null
  cp lime-build/src/libLimeSuite.dll "$P/bin/"; cp LimeSuite-23.11.0/src/ConnectionFTDI/FTD3XXLibrary/x64/FTD3XX.dll "$P/bin/"
  mkdir -p "$P/licenses"; cp LimeSuite-23.11.0/COPYING "$P/licenses/LimeSuite-COPYING.txt"; }

# --- libairspyhf 1.6.8 (Airspy HF+): a DLL, linked to the libusb-1.0.dll that ships with UHD (one libusb, the newer MSYS2 one, for both)
[ -f "$P/bin/libairspyhf.dll" ] || { step libairspyhf; unpack airspyhf-1.6.8.tar.gz airspyhf-1.6.8
  cmake -S airspyhf-1.6.8/libairspyhf -B airspyhf-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_C_FLAGS=-std=gnu11" -DLIBUSB_INCLUDE_DIR=$MS/ucrt64/include/libusb-1.0 -DLIBUSB_LIBRARIES=$MS/ucrt64/lib/libusb-1.0.dll.a >/dev/null
  cmake --build airspyhf-build -j$JOBS --target airspyhf >/dev/null && cp airspyhf-build/src/libairspyhf.dll "$P/bin/"
  mkdir -p "$P/licenses"; cp airspyhf-1.6.8/LICENSE "$P/licenses/airspyhf-LICENSE.txt"; }

step done
echo "$P"; ls "$P/lib"/*.a
