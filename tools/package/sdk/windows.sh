#!/bin/bash
# OnAir for Windows x64 from the clang SDK: <OUT>/OnAir-<v>-windows-x64.zip and -setup.exe (the name the updater looks for).
# MAKENSIS: the makensis program, with NSISDIR set when it is not installed (default: makensis on PATH).
. "$(dirname "$0")/common.sh"
B=build-pkg-win
configure_build x86_64-w64-mingw32 $B
SY=$WIN_SYSROOT
S=$REPO/$B/OnAir-$VERSION-windows-x64
rm -rf "$S"; mkdir -p "$S"
strip_to() { x86_64-w64-mingw32-strip -o "$S/$2" "$REPO/$B/$1"; }
strip_to dect2.exe OnAir.exe; strip_to dect2cli.exe onair-cli.exe; strip_to dect2scan.exe onair-scan.exe; strip_to onair-server.exe onair-server.exe
mkdir -p "$S/web"; cp -R "$REPO/web/onair-web" "$S/web/"
# what the programs import, recursively, and the radio libraries OnAir's native drivers load by name
for e in "$S"/*.exe; do
  "$SDK/collect_dlls.sh" "$e" "$SY" librtlsdr.dll libairspy.dll libairspyhf.dll libbladeRF.dll libiio.dll libLimeSuite.dll 'libhamlib-*.dll' FTD3XX.dll >/dev/null
done
cp "$REPO/$B/libcodec2.dll" "$S/"
mkdir -p "$S/lib/SoapySDR/modules0.8"; cp "$SY/lib/SoapySDR/modules0.8/libremoteSupport.dll" "$S/lib/SoapySDR/modules0.8/"
"$SDK/collect_dlls.sh" "$S/lib/SoapySDR/modules0.8/libremoteSupport.dll" "$SY" >/dev/null || true
rm -f "$S/lib/SoapySDR/modules0.8"/lib[!r]*.dll   # (the module finds SoapySDR and the runtime next to the program)
mkdir -p "$S/codecs" "$S/licenses/sddc_fx3" "$S/licenses/codec2" "$S/licenses/hamlib"
cp "$REPO/third_party/sddc_fx3/SDDC_FX3.img" "$S/codecs/"; cp "$REPO/third_party/sddc_fx3/README-OnAir.md" "$S/licenses/sddc_fx3/NOTICE.md"
cp "$REPO/third_party/codec2/COPYING" "$S/licenses/codec2/"; cp "$REPO/third_party/codec2/README-OnAir.md" "$S/licenses/codec2/NOTICE.md"
cp "$REPO/packaging/licenses/hamlib/README-OnAir.md" "$S/licenses/hamlib/NOTICE.md"
# licences: the libraries built from source, and the MSYS2 runtime that is shipped
copy_licenses "$SY/share/licenses" "$S/licenses" $LIBS
for n in gcc libgcc libstdc++ libiconv libwinpthread winpthreads zlib; do [ ! -d "$SY/share/licenses/$n" ] || copy_licenses "$SY/share/licenses" "$S/licenses" "$n"; done
for f in "$S"/*.dll "$S"/*.exe "$S"/lib/SoapySDR/modules0.8/*.dll; do
  case "$(basename "$f")" in FTD3XX.dll|*.exe) ;; *) x86_64-w64-mingw32-strip --strip-unneeded "$f";; esac   # (FTD3XX.dll: FTDI's signature)
done
python3 -I "$REPO/tools/package/scrub_paths.py" "$S"/*.exe "$S"/*.dll "$S"/lib/SoapySDR/modules0.8/*.dll
OBJDUMP=x86_64-w64-mingw32-objdump "$REPO/tools/package/check_win_dlls.sh" "$S" 2>/dev/null
cp "$REPO/LICENSE" "$S/LICENSE.txt"
"$REPO/tools/package/check_lgpl_bundle.sh" "$S/OnAir.exe"; "$REPO/tools/package/check_lgpl_bundle.sh" "$S/onair-server.exe"
"$REPO/tools/package/add_license_files.sh" "$S"
sed -n '/^OnAir - DVB-T2/,/^TXT$/p' "$REPO/tools/package/make_windows_cross.sh" | sed '$d' > "$S/README.txt"
rm -f "$OUT/$(basename "$S").zip"; (cd "$(dirname "$S")" && zip -qr "$OUT/$(basename "$S").zip" "$(basename "$S")")
"${MAKENSIS:-makensis}" -V2 -DVERSION="$VERSION" -DSRC="$S" -DICON="$REPO/packaging/windows/OnAir.ico" -DOUTFILE="$OUT/$(basename "$S")-setup.exe" "$REPO/packaging/windows/onair.nsi"
ls -la "$OUT/$(basename "$S")".zip "$OUT/$(basename "$S")-setup.exe"
