#!/bin/bash
# Build OnAir.app (with its libraries inside) and a .dmg disk image for macOS.
#   tools/package/make_dmg.sh [version]
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
ARCH=$(uname -m)
BUILD=build-release
# Homebrew dependencies from the macOS 15 release runners set the supported floor.
MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.0}
export MACOSX_DEPLOYMENT_TARGET
# FFmpeg: the LGPL shared build of tools/package/build_ffmpeg.sh (the same as on Linux), never Homebrew's, which is configured with --enable-gpl.
# bundle_macos.py copies its dylibs into Contents/Frameworks; the user can choose another FFmpeg in Settings > Advanced > FFmpeg libraries.
# The workflow caches $FFMPEG_PREFIX; built here when it is missing.
FFMPEG_PREFIX=${FFMPEG_PREFIX:-$PWD/$BUILD/ffmpeg}
[ -f "$FFMPEG_PREFIX/lib/libavcodec.dylib" ] || tools/package/build_ffmpeg.sh "$FFMPEG_PREFIX"
cmake -S . -B $BUILD -DDECT2_FFMPEG_PREFIX="$FFMPEG_PREFIX" -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_TESTS=OFF -DDECT2_BUILD_APP=ON -DDECT2_HAVE_HAMLIB=ON -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DONAIR_VERSION="$VERSION" -DDECT2_WITH_TLS=ON -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"   # TLS: HTTPS for onair-server and the web.on-air.dev heartbeat; libssl/libcrypto are bundled by bundle_macos.py
cmake --build $BUILD -j"$(sysctl -n hw.ncpu)" --target dect2 onair-server
APP=$BUILD/OnAir.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp $BUILD/dect2 "$APP/Contents/MacOS/OnAir"
cp $BUILD/onair-server "$APP/Contents/MacOS/onair-server"   # the headless server for OnAir Web (docs/headless-server.md); bundle_macos.py gives it the same libraries
mkdir -p "$APP/Contents/Resources/web"; cp -R web/onair-web "$APP/Contents/Resources/web/"   # the OnAir Web browser page (onair-server looks in ../Resources/web/onair-web)
cp packaging/icons/OnAir.icns "$APP/Contents/Resources/OnAir.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>OnAir</string>
  <key>CFBundleDisplayName</key><string>OnAir</string>
  <key>CFBundleIdentifier</key><string>io.github.adswill.onair</string>
  <key>CFBundleExecutable</key><string>OnAir</string>
  <key>CFBundleIconFile</key><string>OnAir</string>
  <key>NSHumanReadableCopyright</key><string>OnAir name and logo © 2026 Kristijonas Adomaitis. All rights reserved; the logo is not covered by the GPL licence of the source code.</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>$MACOSX_DEPLOYMENT_TARGET</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
</dict></plist>
PLIST
# codec2 (FreeDV and M17 speech, LGPL-2.1, built from third_party/codec2 as its own shared library): into Contents/Frameworks, where OnAir looks first
mkdir -p "$APP/Contents/Frameworks" "$APP/Contents/Resources/licenses/codec2"
cp -a $BUILD/libcodec2*.dylib "$APP/Contents/Frameworks/"
REAL=$(ls "$APP"/Contents/Frameworks/libcodec2.*.*.*.dylib | head -1)
install_name_tool -id "@executable_path/../Frameworks/$(basename "$REAL")" "$REAL"
cp third_party/codec2/COPYING "$APP/Contents/Resources/licenses/codec2/COPYING"
cp third_party/codec2/README-OnAir.md "$APP/Contents/Resources/licenses/codec2/NOTICE.md"
# Hamlib (CAT rig control, LGPL-2.0+): bundled by bundle_macos.py; licence from Homebrew or the system
mkdir -p "$APP/Contents/Resources/licenses/hamlib"
cp packaging/licenses/hamlib/README-OnAir.md "$APP/Contents/Resources/licenses/hamlib/NOTICE.md"
for prefix in /opt/homebrew/opt/hamlib /usr/local/opt/hamlib; do
  [ -f "$prefix/COPYING" ] && cp "$prefix/COPYING" "$APP/Contents/Resources/licenses/hamlib/COPYING" && break
done
if [ -f third_party/sddc_fx3/SDDC_FX3.img ]; then
  mkdir -p "$APP/Contents/MacOS/codecs" "$APP/Contents/Resources/licenses/sddc_fx3"
  cp third_party/sddc_fx3/SDDC_FX3.img "$APP/Contents/MacOS/codecs/"
  cp third_party/sddc_fx3/README-OnAir.md "$APP/Contents/Resources/licenses/sddc_fx3/NOTICE.md"
fi
RTL_FW=$BUILD/rtlsdr-blog-framework
bash tools/package/build_rtlsdr_blog_macos.sh "$RTL_FW"
export ONAIR_RTLSDR_FRAMEWORK="$RTL_FW"
python3 tools/package/bundle_macos.py "$APP" || { echo "Failed to bundle radio libraries"; exit 1; }
python3 tools/package/check_macos_target.py "$APP"
# FFmpeg licence notice and source offer (LGPL-2.1+): how the library inside the program was built, and the script that rebuilds it
mkdir -p "$APP/Contents/Resources/licenses/ffmpeg"
cp tools/package/build_ffmpeg.sh "$APP/Contents/Resources/licenses/ffmpeg/"
cp "$FFMPEG_PREFIX/share/ffmpeg-configuration.txt" "$APP/Contents/Resources/licenses/ffmpeg/configuration.txt"
cp LICENSES/LGPL-2.1.txt "$APP/Contents/Resources/licenses/ffmpeg/COPYING.LGPLv2.1"
cat > "$APP/Contents/Resources/licenses/ffmpeg/NOTICE.md" <<'NOTE'
OnAir uses the FFmpeg libraries (https://ffmpeg.org, version 8.0), LGPL-2.1-or-later, shipped as shared libraries in Contents/Frameworks.
They were built without --enable-gpl, --enable-version3 and --enable-nonfree: configuration.txt is the exact configure line.
Source: https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz (unmodified). To rebuild: run build_ffmpeg.sh <folder> (this folder),
then build OnAir from its public source with -DDECT2_FFMPEG_PREFIX=<folder> (see LICENSE-NOTICES-BINARIES.md), or choose your own FFmpeg
build of the same major version in Settings > Advanced > FFmpeg libraries. The patent licences of
the video and audio formats (H.264, H.265, AAC, AC-3 ...) are the user's matter, as with FFmpeg everywhere.
NOTE
tools/package/check_lgpl_bundle.sh "$APP/Contents/MacOS/OnAir" "$APP/Contents/Frameworks" || { echo "Licence check failed"; exit 1; }
tools/package/check_lgpl_bundle.sh "$APP/Contents/MacOS/onair-server" "$APP/Contents/Frameworks" || { echo "Licence check failed (onair-server)"; exit 1; }
[ -x "$APP/Contents/MacOS/onair-server" ] || { echo "error: onair-server is missing from the app"; exit 1; }
# licence files: the project's own, and the licence file of the Homebrew formula behind every bundled library
mkdir -p "$APP/Contents/Resources"
tools/package/add_license_files.sh "$APP/Contents/Resources"
HBLIC="$APP/Contents/Resources/licenses/homebrew"
for f in "$APP"/Contents/Frameworks/*.dylib; do
  b=$(basename "$f")
  for root in /opt/homebrew/opt /usr/local/opt; do
    for d in "$root"/*/; do
      if [ -e "${d}lib/$b" ]; then
        n=$(basename "$d"); mkdir -p "$HBLIC/$n"
        for t in "$d"COPYING* "$d"LICENSE* "$d"LICENCE* "$d"COPYRIGHT*; do if [ -f "$t" ]; then cp "$t" "$HBLIC/$n/"; fi; done
      fi
    done
  done
done
# the SoapyRemote module (BSL-1.0, bundled by bundle_macos.py) is not a *.dylib in Frameworks/: its formula's licence is copied explicitly
for root in /opt/homebrew/opt /usr/local/opt; do
  if [ -d "$root/soapyremote" ]; then mkdir -p "$HBLIC/soapyremote"; for t in "$root"/soapyremote/COPYING* "$root"/soapyremote/LICENSE*; do if [ -f "$t" ]; then cp "$t" "$HBLIC/soapyremote/"; fi; done; fi
done
# the app is signed ad hoc (no developer certificate): macOS shows a one-time "unidentified developer" prompt on first launch
python3 tools/package/scrub_paths.py "$APP"   # no build-machine paths in the shipped files (before signing)
# data files next to the program (the RX888 firmware in MacOS/codecs) must carry a signature too, or signing the app stops with "code object is not signed at all"
for f in "$APP"/Contents/Frameworks/*.dylib "$APP"/Contents/Frameworks/SoapySDR/modules/* "$APP"/Contents/MacOS/codecs/*; do
  if [ -L "$f" ] || [ ! -f "$f" ]; then continue; fi
  codesign --force -s - "$f" || echo "warning: could not sign $f"
done
# the second program (onair-server) and any other helper in MacOS/ must be signed before the app, or the app signature stops with "code object is not signed at all"
for f in "$APP"/Contents/MacOS/*; do
  if [ -L "$f" ] || [ ! -f "$f" ] || [ "$(basename "$f")" = "OnAir" ]; then continue; fi
  codesign --force -s - "$f" || echo "warning: could not sign $f"
done
# no --deep: every library and module inside is signed above, and a folder name with an extension (SoapySDR "modules0.8") is read as a malformed bundle, hence "modules"
codesign --force -s - "$APP" || { echo "Signing the app failed"; codesign -dvvv "$APP" 2>&1 | head -20; exit 1; }
STAGE=$BUILD/dmg
rm -rf $STAGE && mkdir -p $STAGE
cp -R "$APP" $STAGE/
ln -s /Applications $STAGE/Applications
DMG="$BUILD/OnAir-$VERSION-macos-$ARCH.dmg"
rm -f "$DMG"
hdiutil create -volname "OnAir $VERSION" -srcfolder $STAGE -ov -format UDZO "$DMG" >/dev/null
echo "$DMG  ($(du -h "$DMG" | cut -f1))"
