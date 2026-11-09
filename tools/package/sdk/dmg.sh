#!/bin/bash
# OnAir for macOS from the clang SDK, on Linux: <OUT>/OnAir-<v>-macos-<arch>.dmg (the name the updater looks for).   dmg.sh arm64|x86_64
# The app is signed ad hoc with rcodesign, as make_dmg.sh does with Apple's codesign, so it gets the same first-start prompt; the disk image is
# a compressed Apple image holding an HFS+ volume (mkfs.hfsplus, hfsplus, dmg). All of them come with the SDK (common/build_pkgtools.sh).
. "$(dirname "$0")/common.sh"
ARCH=${1:?usage: dmg.sh arm64|x86_64}
case "$ARCH" in arm64|x86_64) ;; *) echo "arm64 or x86_64" >&2; exit 2;; esac
TRIPLE=$ARCH-apple-darwin B=$(pkg_dir build-pkg-mac-$ARCH)
configure_build "$TRIPLE" "$B" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=@executable_path/../Frameworks
for t in rcodesign mkfs.hfsplus fsck.hfsplus hfsplus dmg; do command -v "$t" >/dev/null || { echo "error: $t is not in the SDK (make_sdk.sh makes it)" >&2; exit 1; }; done
SY=$SDK/sysroots/$TRIPLE
MIN=$(sed -n 's/^deployment: *macOS \([0-9.]*\).*/\1/p' "$SY/SYSROOT-MANIFEST.txt")
APP=$B/OnAir.app; C=$APP/Contents
rm -rf "$APP"; mkdir -p "$C/MacOS" "$C/Resources/web" "$C/Frameworks/SoapySDR/modules"
cp "$B/dect2" "$C/MacOS/OnAir"; cp "$B/onair-server" "$C/MacOS/onair-server"
cp -R "$REPO/web/onair-web" "$C/Resources/web/"; cp "$REPO/packaging/icons/OnAir.icns" "$C/Resources/"
sed -e "s/\$VERSION/$VERSION/g" -e "s/\$MACOSX_DEPLOYMENT_TARGET/$MIN/g" > "$C/Info.plist" < <(sed -n '/^<?xml version/,/^<\/dict><\/plist>$/p' "$REPO/tools/package/make_dmg.sh")
for e in "$C/MacOS/OnAir" "$C/MacOS/onair-server"; do
  "$SDK/collect_dylibs.sh" "$e" "$C/Frameworks" "$SY" librtlsdr.0.dylib libairspy.0.dylib libairspyhf.0.dylib libbladeRF.2.dylib libiio.0.dylib 'libLimeSuite.*.dylib' libhamlib.4.dylib >/dev/null
done
M=$SY/usr/local/lib/SoapySDR/modules0.8/libremoteSupport.so
cp "$M" "$C/Frameworks/SoapySDR/modules/"; "$SDK/collect_dylibs.sh" "$M" "$C/Frameworks" "$SY" >/dev/null
cp -P "$B"/libcodec2*.dylib "$C/Frameworks/"
cp "$REPO/third_party/sddc_fx3/SDDC_FX3.img" "$C/Resources/"   # (OnAir looks in Resources/; only code belongs in MacOS/)
L=$C/Resources/licenses; mkdir -p "$L/codec2" "$L/hamlib" "$L/sddc_fx3" "$L/ffmpeg"
copy_licenses "$SY/usr/local/share/licenses" "$L" $LIBS
cp "$REPO/third_party/codec2/COPYING" "$L/codec2/"; cp "$REPO/third_party/codec2/README-OnAir.md" "$L/codec2/NOTICE.md"
cp "$REPO/packaging/licenses/hamlib/README-OnAir.md" "$L/hamlib/NOTICE.md"; cp "$REPO/third_party/sddc_fx3/README-OnAir.md" "$L/sddc_fx3/NOTICE.md"
cat > "$L/ffmpeg/NOTICE.md" <<'NOTE'
OnAir uses the FFmpeg libraries (https://ffmpeg.org, version 8.0), LGPL-2.1-or-later, linked statically into the program. They were built
without --enable-gpl, --enable-version3 and --enable-nonfree: CONFIGURATION.txt holds the exact configure line.
Source: https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz (unmodified). The patent licences of the formats are the user's matter, as with FFmpeg everywhere.
NOTE
"$REPO/tools/package/add_license_files.sh" "$C/Resources"
for p in OnAir onair-server; do "$REPO/tools/package/check_lgpl_bundle.sh" "$C/MacOS/$p" "$C/Frameworks"; done
python3 -I "$SDK/macho.py" deps "$C/MacOS/OnAir" | grep -vE '@rpath/|/usr/lib/|/System/Library/' | grep -E '^\s*(/|@)' && { echo "error: OnAir loads from outside the app and the system" >&2; exit 1; } || true
find "$APP" -type f -exec chmod go-w {} +; find "$APP" -type d -exec chmod 755 {} +

# no build-machine paths, then the signature: whatever changes a file comes before it
python3 -I "$REPO/tools/package/scrub_paths.py" "$C/MacOS" "$C/Frameworks"
SIGNLOG=$B/rcodesign.log
rcodesign sign "$APP" > "$SIGNLOG" 2>&1 || { tail -20 "$SIGNLOG" >&2; exit 1; }
if grep -E 'we do not know|^(warning|error)' "$SIGNLOG"; then echo "error: rcodesign did not like this app" >&2; exit 1; fi
python3 -I "$SDK/macho.py" bundlecheck "$APP"

# the volume: a whole number of MB (dmg build drops a part block at the end, which would lose the backup header); hfsplus leaves wrong folder
# counts behind, which fsck repairs on the plain file
ST=$B/dmg; HFS=$B/volume.hfs; DMG=$OUT/OnAir-$VERSION-macos-$ARCH.dmg
rm -rf "$ST" "$HFS"; mkdir -p "$ST"; cp -a "$APP" "$ST/"
truncate -s "$(( ($(du -sm "$ST" | cut -f1) * 11 / 10 + 16) * 1024 * 1024 ))" "$HFS"
mkfs.hfsplus -v "OnAir $VERSION" "$HFS" >/dev/null
hfsplus "$HFS" addall "$ST" --symlinks clone_link >/dev/null 2>&1
hfsplus "$HFS" symlink /Applications /Applications >/dev/null 2>&1
fsck.hfsplus -fy "$HFS" >/dev/null 2>&1 || true
fsck.hfsplus -fn "$HFS" 2>&1 | grep -q 'appears to be OK' || { echo "error: the HFS+ volume is not clean" >&2; fsck.hfsplus -fn "$HFS" >&2; exit 1; }
rm -f "$DMG"; dmg build "$HFS" "$DMG" >/dev/null 2>&1
# what is in the image must be what was signed: back out of the compressed image, file by file
BACK=$B/back.hfs; rm -f "$BACK"; dmg extract "$DMG" "$BACK" >/dev/null 2>&1
cmp "$HFS" "$BACK" || { echo "error: the disk image does not give the volume back" >&2; exit 1; }
[ "$(tail -c 512 "$DMG" | head -c 4)" = koly ] || { echo "error: no disk image trailer in $DMG" >&2; exit 1; }
n=0
while IFS= read -r f; do
  rel=${f#"$APP"/}
  hfsplus "$BACK" cat "/OnAir.app/$rel" 2>/dev/null | cmp -s - "$f" || { echo "error: $rel in the image differs from the signed app" >&2; exit 1; }
  n=$((n + 1))
done < <(find "$APP" -type f)
echo "the image holds all $n files of the signed app unchanged"
rm -f "$HFS" "$BACK"; rm -rf "$ST"
ls -la "$DMG"
