#!/bin/bash
# Build OnAir.app (with its libraries inside) and a .dmg disk image for macOS.
#   tools/package/make_dmg.sh [version]
set -e
cd "$(dirname "$0")/../.."
VERSION=${1:-$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
ARCH=$(uname -m)
BUILD=build-release
cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_TESTS=OFF -DDECT2_BUILD_APP=ON >/dev/null
cmake --build $BUILD -j"$(sysctl -n hw.ncpu)" --target dect2 2>&1 | grep -E "error" -A3 || true
APP=$BUILD/OnAir.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp $BUILD/dect2 "$APP/Contents/MacOS/OnAir"
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
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
</dict></plist>
PLIST
python3 tools/package/bundle_macos.py "$APP"
# the app is signed ad hoc (no developer certificate): macOS shows a one-time "unidentified developer" prompt on first launch
for f in "$APP"/Contents/Frameworks/*.dylib; do codesign --force -s - "$f" >/dev/null 2>&1; done
codesign --force --deep -s - "$APP" >/dev/null 2>&1
STAGE=$BUILD/dmg
rm -rf $STAGE && mkdir -p $STAGE
cp -R "$APP" $STAGE/
ln -s /Applications $STAGE/Applications
DMG="$BUILD/OnAir-$VERSION-macos-$ARCH.dmg"
rm -f "$DMG"
hdiutil create -volname "OnAir $VERSION" -srcfolder $STAGE -ov -format UDZO "$DMG" >/dev/null
echo "$DMG  ($(du -h "$DMG" | cut -f1))"
