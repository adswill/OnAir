#!/bin/bash
# A Linux package that carries its own libraries: unpack anywhere and run bin/onair. Run inside the build container.
#   make_portable_linux.sh <build dir> <output dir>
# The libraries of the system that every program has anyway (libc, graphics, windowing, sound) are NOT included: those must be the
# system's own. The C++ runtime is not included either: the build container is old, so the system's is always newer.
set -e
B=$1; OUT=$2
SRC=${SRC:-/src}   # the source tree (the build container mounts it at /src)
VERSION=$(grep -m1 'project(' $SRC/CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
ARCH=$(uname -m)
D=$(mktemp -d)/onair-$VERSION-linux-$ARCH-portable
mkdir -p "$D/bin" "$D/lib"
cp $B/dect2 "$D/bin/onair"; cp $B/dect2cli "$D/bin/onair-cli"; cp $B/dect2scan "$D/bin/onair-scan"
cp $B/onair-server "$D/bin/onair-server"   # the headless server (OnAir Web)
SKIP='linux-vdso|ld-linux|/libc\.so|/libm\.so|/libdl\.so|/libpthread\.so|/librt\.so|/libutil\.so|/libresolv\.so|/libstdc\+\+|/libgcc_s|/libGL|/libEGL|/libOpenGL|/libX|/libxcb|/libwayland|/libdrm|/libasound|/libpulse|/libudev|/libdbus|/libsystemd|/libxkbcommon|/libffi|/libgbm|/libva'
collect() {
  ldd "$1" | awk '/=> \//{print $3}' | while read -r lib; do
    echo "$lib" | grep -Eq "$SKIP" && continue
    [ -f "$D/lib/$(basename "$lib")" ] || { cp -L "$lib" "$D/lib/"; collect "$lib"; }
  done
}
[ -x "$D/bin/onair-server" ] || { echo "error: onair-server is missing from the portable package"; exit 1; }
# FFmpeg: the shared LGPL build the programs were linked with (DECT2_FFMPEG_PREFIX); in the build folder the programs' RUNPATH ($ORIGIN/../lib/<arch>/onair) finds nothing, so ldd
# is told where it is. A user can choose another FFmpeg in Settings > Advanced > FFmpeg libraries.
FF=$(sed -n 's/^DECT2_FFMPEG_PREFIX:PATH=//p' "$B/CMakeCache.txt" 2>/dev/null)
if [ -n "$FF" ]; then cp -a "$FF"/lib/lib*.so.* "$D/lib/"; export LD_LIBRARY_PATH="$FF/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; fi
for p in "$D"/bin/*; do collect "$p"; done
# the radio libraries (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP), found by name next to the program
# codec2 (FreeDV and M17 speech, LGPL-2.1, third_party/codec2): its own shared library, next to the radio libraries (OnAir looks in ../lib of its program)
cp -a "$B"/libcodec2.so* "$D/lib/" 2>/dev/null || echo "warning: no libcodec2 in $B (built with -DDECT2_BUNDLE_CODEC2=OFF?)"
mkdir -p "$D/licenses/codec2"; cp $SRC/third_party/codec2/COPYING "$D/licenses/codec2/COPYING"; cp $SRC/third_party/codec2/README-OnAir.md "$D/licenses/codec2/NOTICE.md"
mkdir -p "$D/licenses/hamlib"; cp $SRC/packaging/licenses/hamlib/README-OnAir.md "$D/licenses/hamlib/NOTICE.md"
[ -f /usr/share/doc/libhamlib2/copyright ] && cp /usr/share/doc/libhamlib2/copyright "$D/licenses/hamlib/COPYING"
if [ -f "$SRC/third_party/sddc_fx3/SDDC_FX3.img" ]; then
  mkdir -p "$D/codecs" "$D/licenses/sddc_fx3"
  cp "$SRC/third_party/sddc_fx3/SDDC_FX3.img" "$D/codecs/"
  cp "$SRC/third_party/sddc_fx3/README-OnAir.md" "$D/licenses/sddc_fx3/NOTICE.md"
fi
[ -d "$B/radiolibs" ] && cp -a "$B"/radiolibs/. "$D/lib/"
for p in "$D"/bin/*; do patchelf --set-rpath '$ORIGIN/../lib' "$p"; done
for l in "$D"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l" 2>/dev/null || true; done
mkdir -p "$D/share/applications" "$D/share/icons"
cp $SRC/packaging/linux/onair.desktop "$D/share/applications/"; cp $SRC/packaging/icons/onair_1024.png "$D/share/icons/onair.png"
cp $SRC/LICENSE "$D/LICENSE"
$SRC/tools/package/add_license_files.sh "$D" "$SRC"   # THIRD_PARTY.md, LICENSES/, LICENSE-NOTICES-BINARIES.md (licence of every bundled library, source offer)
# the Debian copyright file of each bundled system library (the ones built here, RTL-SDR Blog and codec2, are covered by the files above)
mkdir -p "$D/licenses/system-libs"
for l in "$D"/lib/*.so.*; do
  b=$(basename "$(readlink -f "$l")")
  pkg=$(dpkg -S "*/$b" 2>/dev/null | head -1 | cut -d: -f1)
  [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ] && cp "/usr/share/doc/$pkg/copyright" "$D/licenses/system-libs/$pkg.copyright"
done
# udev rules and the kernel-driver blacklist for the radios, with the script that installs them (the .deb installs them itself)
mkdir -p "$D/udev"
cp $SRC/packaging/linux/60-onair-sdr.rules $SRC/packaging/linux/onair-rtlsdr-blacklist.conf $SRC/packaging/linux/onair-sdrplay-blacklist.conf "$D/udev/"
mkdir -p "$D/web"; cp -R $SRC/web/onair-web "$D/web/"   # the OnAir Web browser page (onair-server looks in ../web/onair-web)
cp $SRC/packaging/linux/onair-server.service "$D/"   # optional systemd unit, see its header (the paths there are /usr/local/bin)
cp $SRC/packaging/linux/install-udev-rules.sh "$D/"; chmod 755 "$D/install-udev-rules.sh"
cat > "$D/README.txt" <<'TXT'
OnAir portable: run bin/onair. Nothing to install.
OnAir Web / headless: bin/onair-server --web --web-lan (docs/headless-server.md; onair-server.service is an optional systemd unit).
If a radio is not found, run sudo ./install-udev-rules.sh once (it lets a normal user open the radios and keeps the kernel's TV driver off RTL-SDR dongles), then replug the radio.
Libraries OnAir needs from the system: the graphics driver (OpenGL), X11 or Wayland, and for sound ALSA, PulseAudio or PipeWire.
TXT
(cd "$(dirname "$D")" && tar czf "$OUT/$(basename "$D").tar.gz" "$(basename "$D")")
echo "$OUT/$(basename "$D").tar.gz"; ls "$D/lib"
