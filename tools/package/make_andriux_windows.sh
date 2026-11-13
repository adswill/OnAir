#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Create the Andriux Edition portable Windows x64 package from an EXISTING
# working MSYS2 UCRT64 build (no rebuild/reconfiguration required).
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION="${1:-0.2.3-andriux.1}"
PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
SRC="build"
OUT="dist/OnAir-${VERSION}-windows-x64"
ZIP="${OUT}.zip"
if [ ! -f "$SRC/dect2.exe" ]; then
  echo "ERROR: $SRC/dect2.exe missing; build the program first." >&2
  exit 1
fi
SPY_DLL="$PREFIX/lib/SoapySDR/modules0.8/libSpyServerSupport.dll"
if [ ! -f "$SPY_DLL" ]; then
  SPY_DLL="$SRC/lib/SoapySDR/modules0.8/libSpyServerSupport.dll"
fi
if [ ! -f "$SPY_DLL" ]; then
  echo "ERROR: SpyServer Soapy module not found; install/build SoapySpyServer first." >&2
  exit 1
fi
mkdir -p dist
rm -rf "$OUT"
mkdir -p "$OUT/lib/SoapySDR/modules0.8"
cp "$SRC/dect2.exe" "$OUT/OnAir.exe"
for pair in "dect2cli.exe:onair-cli.exe" "dect2scan.exe:onair-scan.exe"; do
  from="${pair%%:*}" to="${pair##*:}"
  [ ! -f "$SRC/$from" ] || cp "$SRC/$from" "$OUT/$to"
done
cp "$SPY_DLL" "$OUT/lib/SoapySDR/modules0.8/"

# Collect runtime DLLs recursively from each bundled EXE/DLL. The SpyServer
# module itself is also inspected; it needs dependencies in the root folder.
collect() {
  local f="$1" dll base
  while IFS= read -r dll; do
    [ -f "$dll" ] || continue
    base="$(basename "$dll")"
    case "$base" in
      api-ms-win-*|KERNEL32.dll|USER32.dll) continue ;;
    esac
    if [ ! -f "$OUT/$base" ]; then
      cp "$dll" "$OUT/$base"
      collect "$OUT/$base"
    fi
  done < <(ldd "$f" 2>/dev/null | awk -v prefix="$PREFIX/" 'index($3,prefix)==1 {print $3}')
}
for f in "$OUT"/*.exe; do [ -f "$f" ] && collect "$f"; done
collect "$OUT/lib/SoapySDR/modules0.8/libSpyServerSupport.dll"

# Copy any additional Soapy modules and inspect their dependencies.
for folder in "$PREFIX"/lib/SoapySDR/modules*/; do
  [ -d "$folder" ] || continue
  for dll in "$folder"*.dll; do
    [ -f "$dll" ] || continue
    case "$(basename "$dll")" in
      *rtlsdrSupport.dll|*airspySupport.dll|libSpyServerSupport.dll) continue ;;
    esac
    cp "$dll" "$OUT/lib/SoapySDR/modules0.8/"
    collect "$OUT/lib/SoapySDR/modules0.8/$(basename "$dll")"
  done
done
# OnAir loads native radio libraries dynamically, so ldd of OnAir.exe alone
# cannot discover them.
for name in librtlsdr libairspy libairspyhf libbladeRF libLimeSuite libiio libuhd; do
  if [ -f "$PREFIX/bin/$name.dll" ]; then
    cp "$PREFIX/bin/$name.dll" "$OUT/"
    collect "$OUT/$name.dll"
  fi
done
cp LICENSE "$OUT/LICENSE.txt"
tools/package/add_license_files.sh "$OUT"   # THIRD_PARTY.md, LICENSES/, LICENSE-NOTICES-BINARIES.md (licence of every bundled DLL, source offer)
cat > "$OUT/README-LT.txt" <<'TXT'
OnAir v0.2.3 Andriux Edition - Windows x64 (bandomoji versija)

1. Isskleiskite visa ZIP archyva i atskira aplanka.
2. Paleiskite OnAir.exe (nereikia MSYS2).
3. Network radios -> SpyServer: iveskite Raspberry Pi IP ir porta.
4. View -> Passes: Celestrak, Keplerpass ir savi TLE serveriai.

SpyServer palaikymui sukompiliuotas SoapySpyServer modulis idetas i
lib/SoapySDR/modules0.8. Tikrinta tik turimos Windows konfiguracijos lygiu;
kituose kompiuteriuose ar su kitais SDR imtuvais gali reiketi tvarkykliu.

Ispejimas: neskelbkite asmeniniu slaptazodziu ar API raktu ZIP archyve.
TXT
if [ -x tools/package/check_win_dlls.sh ]; then
  bash tools/package/check_win_dlls.sh "$OUT"
fi
if command -v zip >/dev/null 2>&1; then
  rm -f "$ZIP"
  (cd dist && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")")
else
  echo "ERROR: zip unavailable. Install: pacman -S --needed zip" >&2
  exit 1
fi
echo
echo "PACKAGE READY: $ZIP"
echo "Next: gh release upload v$VERSION \"$ZIP\" --repo andriux26/OnAir --clobber"
