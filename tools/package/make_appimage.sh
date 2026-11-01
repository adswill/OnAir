#!/bin/bash
# An AppImage (one file, runs on most Linux systems without installing; OnAir updates it in place, no root) made from the portable package.
#   make_appimage.sh <portable .tar.gz> <output dir>
# Needs appimagetool (APPIMAGETOOL=path, default: appimagetool on PATH; in CI it is fetched at a pinned version and run with --appimage-extract-and-run).
set -e
T=$1; OUT=$2
TOOL=${APPIMAGETOOL:-appimagetool}
W=$(mktemp -d)
tar xzf "$T" -C "$W"
P=$(ls -d "$W"/onair-*-portable)
NAME=$(basename "$P" -portable)          # onair-3.6.0-linux-x86_64
VERSION=$(echo "$NAME" | sed -E 's/^onair-([0-9.]+)-linux-.*/\1/')
ARCH=$(echo "$NAME" | sed -E 's/.*-linux-//')
A="$W/OnAir.AppDir"
mv "$P" "$A"
cat > "$A/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/bin/onair" "$@"
RUN
chmod 755 "$A/AppRun"
cp "$A/share/applications/onair.desktop" "$A/onair.desktop"
cp "$A/share/icons/onair.png" "$A/onair.png"
grep -q '^Icon=' "$A/onair.desktop" || echo 'Icon=onair' >> "$A/onair.desktop"
F="$OUT/OnAir-$VERSION-linux-$ARCH.AppImage"
ARCH=$ARCH $TOOL "$A" "$F" >&2
chmod 755 "$F"
echo "$F"
