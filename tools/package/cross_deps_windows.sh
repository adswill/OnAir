#!/bin/bash
# Cross-compile OnAir's dependencies for Windows x64 with mingw-w64 (macOS or Linux host). Result: ~/onair-win-prefix (override with ONAIR_WIN_PREFIX)
# Needs: x86_64-w64-mingw32-gcc, cmake, nasm, make, pkg-config, curl and the source archives in build-windows-deps/src
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

# --- generic radios: SoapySDR (a DLL) with the RTL-SDR and Airspy drivers (libusb and the vendor libraries are linked into the driver DLLs)
LIBUSB_LIBS="-L$P/lib -lusb-1.0"

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

[ -f "$P/lib/libairspy.a" ] || { step libairspy; unpack airspyone_host-1.0.10.tar.gz airspyone_host-1.0.10
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

for mod in "soapyrtlsdr-0.3.0.tar.gz SoapyRTLSDR-soapy-rtlsdr-0.3.0 librtlsdrSupport.dll" "soapyairspy-0.2.0.tar.gz SoapyAirspy-soapy-airspy-0.2.0 libairspySupport.dll"; do
  set -- $mod
  [ -f "$P/lib/SoapySDR/modules0.8/$3" ] || { step "$2"; unpack $1 $2
    cmake -S $2 -B build-$2 "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSoapySDR_DIR=$P/cmake "-DCMAKE_CXX_STANDARD_LIBRARIES=$LIBUSB_LIBS" >/dev/null
    cmake --build build-$2 -j$JOBS >/dev/null && cmake --install build-$2 >/dev/null; }
done

# --- the same two libraries as DLLs of their own (libusb inside): the native drivers of OnAir load rtlsdr.dll / airspy.dll by name from the program's folder
[ -f "$P/bin/librtlsdr.dll" ] || { step "librtlsdr.dll"
  cmake rtl-build "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null && cmake --build rtl-build -j$JOBS --target rtlsdr >/dev/null && cp rtl-build/src/librtlsdr.dll "$P/bin/"; }
[ -f "$P/bin/libairspy.dll" ] || { step "libairspy.dll"
  cmake --build airspy-build -j$JOBS --target airspy >/dev/null; cp "$(find "$WORK" -name libairspy.dll -path '*src*' | head -1)" "$P/bin/libairspy.dll"; }

# --- BladeRF and PlutoSDR (libbladeRF, libiio with libxml2): DLLs of their own, shipped next to OnAir.exe (the native drivers load them by name).
# Needs bladeRF-2023.02.tar.gz, no-OS-0bba46e.tar.gz (a part of the bladeRF source that git keeps as a submodule), libiio-0.25.tar.gz and
# libxml2-2.12.9.tar.xz in build-windows-deps/src. LimeSuite and UHD are not built: their Windows drivers need Cypress/FTDI libraries or Boost.
[ -f "$P/lib/libxml2.a" ] || { step libxml2; unpack libxml2-2.12.9.tar.xz libxml2-2.12.9
  cmake -S libxml2-2.12.9 -B xml-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF >/dev/null
  cmake --build xml-build -j$JOBS >/dev/null && cmake --install xml-build >/dev/null; }
[ -f "$P/bin/libiio.dll" ] || { step libiio; unpack libiio-0.25.tar.gz libiio-0.25
  cmake -S libiio-0.25 -B iio-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWITH_USB_BACKEND=ON -DWITH_NETWORK_BACKEND=OFF -DWITH_LOCAL_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DWITH_ZSTD=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DENABLE_PACKAGING=OFF -DINSTALL_UDEV_RULE=OFF -DCPP_BINDINGS=OFF -DPYTHON_BINDINGS=OFF -DWITH_IIOD=OFF -DBUILD_SHARED_LIBS=ON -DLIBUSB_INCLUDE_DIR=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a -DLIBXML2_LIBRARIES=$P/lib/libxml2.a -DLIBXML2_INCLUDE_DIR=$P/include/libxml2 "-DCMAKE_C_FLAGS=-DLIBXML_STATIC" "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null
  cmake --build iio-build -j$JOBS >/dev/null && cp iio-build/libiio.dll "$P/bin/"; }
[ -f "$P/bin/libbladeRF.dll" ] || { step libbladeRF; unpack bladeRF-2023.02.tar.gz bladeRF-2023.02
  mkdir -p bladeRF-2023.02/thirdparty/analogdevicesinc/no-OS && tar xzf "$SRC/no-OS-0bba46e.tar.gz" -C bladeRF-2023.02/thirdparty/analogdevicesinc/no-OS --strip-components=1
  find bladeRF-2023.02/host \( -name CMakeLists.txt -o -name "*.cmake" \) -exec sed -i.bak 's/-Werror//g' {} \;   # a newer compiler than the one the project was written for warns about more
  cmake -S bladeRF-2023.02/host -B blade-build "-DCMAKE_TOOLCHAIN_FILE=$TC" -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_BACKEND_LIBUSB=ON -DENABLE_BACKEND_CYPRESS=OFF -DBUILD_DOCUMENTATION=OFF -DENABLE_FX3_BUILD=OFF -DENABLE_HOST_BUILD=ON -DBUILD_BLADERF_CLI=OFF -DENABLE_UDEV_RULES=OFF -DINSTALL_UDEV_RULES=OFF -DBUILD_NATIVE=OFF -DLIBUSB_PATH=$P -DLIBUSB_INCLUDE_DIRS=$P/include/libusb-1.0 -DLIBUSB_LIBRARIES=$P/lib/libusb-1.0.a "-DCMAKE_C_FLAGS=-std=gnu11" "-DCMAKE_SHARED_LINKER_FLAGS=-L$P/lib" >/dev/null
  cmake --build blade-build -j$JOBS --target libbladerf_shared >/dev/null && cp blade-build/output/libbladeRF.dll "$P/bin/"; }

step done
echo "$P"; ls "$P/lib"/*.a
