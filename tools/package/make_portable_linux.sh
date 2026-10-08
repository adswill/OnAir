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
SKIP='linux-vdso|ld-linux|/libc\.so|/libm\.so|/libdl\.so|/libpthread\.so|/librt\.so|/libutil\.so|/libresolv\.so|/libstdc\+\+|/libgcc_s|/libGL|/libEGL|/libOpenGL|/libX|/libxcb|/libwayland|/libdrm|/libasound|/libpulse|/libudev|/libdbus|/libsystemd|/libxkbcommon|/libffi|/libgbm|/libva'
collect() {
  ldd "$1" | awk '/=> \//{print $3}' | while read -r lib; do
    echo "$lib" | grep -Eq "$SKIP" && continue
    [ -f "$D/lib/$(basename "$lib")" ] || { cp -L "$lib" "$D/lib/"; collect "$lib"; }
  done
}
for p in "$D"/bin/*; do collect "$p"; done
# the radio libraries (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP), found by name next to the program
[ -d "$B/radiolibs" ] && cp -a "$B"/radiolibs/. "$D/lib/"
for p in "$D"/bin/*; do patchelf --set-rpath '$ORIGIN/../lib' "$p"; done
for l in "$D"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l" 2>/dev/null || true; done
mkdir -p "$D/share/applications" "$D/share/icons"
cp $SRC/packaging/linux/onair.desktop "$D/share/applications/"; cp $SRC/packaging/icons/onair_1024.png "$D/share/icons/onair.png"
cp $SRC/LICENSE "$D/LICENSE"
# udev rules and the kernel-driver blacklist for the radios, with the script that installs them (the .deb installs them itself)
mkdir -p "$D/udev"
cp $SRC/packaging/linux/60-onair-sdr.rules $SRC/packaging/linux/onair-rtlsdr-blacklist.conf $SRC/packaging/linux/onair-sdrplay-blacklist.conf "$D/udev/"
cp $SRC/packaging/linux/install-udev-rules.sh "$D/"; chmod 755 "$D/install-udev-rules.sh"
cat > "$D/README.txt" <<'TXT'
OnAir portable: run bin/onair. Nothing to install.
If a radio is not found, run sudo ./install-udev-rules.sh once (it lets a normal user open the radios and keeps the kernel's TV driver off RTL-SDR dongles), then replug the radio.
Libraries OnAir needs from the system: the graphics driver (OpenGL), X11 or Wayland, and for sound ALSA, PulseAudio or PipeWire.
TXT
(cd "$(dirname "$D")" && tar czf "$OUT/$(basename "$D").tar.gz" "$(basename "$D")")
echo "$OUT/$(basename "$D").tar.gz"; ls "$D/lib"
