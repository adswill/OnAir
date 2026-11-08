#!/bin/bash
# OnAir for Ubuntu 26.04 or newer from the clang SDK: <OUT>/onair_<v>_<arch>.deb.   deb.sh amd64|arm64
# Installed with the CMake rules (udev rules, blacklists, systemd unit ...), packed with dpkg-deb. The libraries are the SDK's, in /usr/lib/onair.
. "$(dirname "$0")/common.sh"
ARCH=${1:?usage: deb.sh amd64|arm64}
case "$ARCH" in
  amd64) TRIPLE=x86_64-linux-gnu  LOCK=sysroot.lock;       EXTRA=();;
  arm64) TRIPLE=aarch64-linux-gnu LOCK=sysroot-arm64.lock; EXTRA=(-DRPI_CPU=generic);;   # the updater offers this one package to every arm64 machine, not only a Pi 5
  *) echo "amd64 or arm64" >&2; exit 2;;
esac
B=build-pkg-$ARCH
SY=$SDK/sysroots/$TRIPLE
SLIB=$SY/usr/local/lib
# the folder DECT2_RADIO_LIB_DIR installs into /usr/lib/onair: the run-time names of the SDK's shared libraries
RL=$REPO/$B-radiolibs; rm -rf "$RL" "$RL.licenses"; mkdir -p "$RL/SoapySDR/modules0.8"
for pat in 'libSoapySDR.so.*' 'libhamlib.so.*' 'librtlsdr.so.*' 'libairspy.so.*' 'libairspyhf.so.*' 'libbladeRF.so*' 'libiio.so.*' 'libLimeSuite.so.*'; do
  n=$(find "$SLIB" -maxdepth 1 -name "$pat" | wc -l); [ "$n" -gt 0 ] || { echo "error: no $pat in $SLIB" >&2; exit 1; }
  find "$SLIB" -maxdepth 1 -name "$pat" -exec cp -P {} "$RL/" \;
done
rm -f "$RL/libbladeRF.so"   # (the link name: only run-time names are shipped)
cp "$SLIB/SoapySDR/modules0.8/libremoteSupport.so" "$RL/SoapySDR/modules0.8/"
copy_licenses "$SY/usr/local/share/licenses" "$RL.licenses" $LIBS
configure_build "$TRIPLE" "$B" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DDECT2_RADIO_LIB_DIR="$RL" "${EXTRA[@]}"
S=$REPO/$B/stage; rm -rf "$S"; mkdir -p "$S"
DESTDIR=$S cmake --install "$REPO/$B" >/dev/null
# no build-machine files, smaller programs
while IFS= read -r f; do
  case "$(head -c4 "$f" | od -An -tx1 | tr -d ' ')" in 7f454c46) llvm-strip --strip-unneeded "$f";; esac
done < <(find "$S/usr/bin" "$S/usr/lib/onair" -type f)
python3 -I "$REPO/tools/package/scrub_paths.py" "$S/usr/bin" "$S/usr/lib/onair"
"$REPO/tools/package/check_lgpl_bundle.sh" "$S/usr/bin/onair" "$S/usr/lib/onair"; "$REPO/tools/package/check_lgpl_bundle.sh" "$S/usr/bin/onair-server" "$S/usr/lib/onair"
for p in onair onair-server onair-cli; do
  llvm-readobj --dynamic-table "$S/usr/bin/$p" | grep -q 'RUNPATH.*/usr/lib/onair' || { echo "error: $p has no RUNPATH to /usr/lib/onair" >&2; exit 1; }
done
# GLFW is linked in and loads the window system at run time, so NEEDED does not show all of it
DEPENDS=$(python3 -I "$REPO/tools/package/sdk/deb_depends.py" "$S" "$(dirname "$SDK")/linux/$LOCK" "$(dirname "$SDK")/cache/ubuntu" libgl1 libxcursor1 libxi6 libxrandr2)
mkdir -p "$S/DEBIAN"
for f in postinst postrm; do install -m 755 "$REPO/packaging/linux/deb/$f" "$S/DEBIAN/$f"; done
install -m 644 "$REPO/packaging/linux/deb/conffiles" "$S/DEBIAN/conffiles"
cat > "$S/DEBIAN/control" <<CTL
Package: onair
Version: $VERSION
Section: hamradio
Priority: optional
Architecture: $ARCH
Maintainer: OnAir <adswill@users.noreply.github.com>
Installed-Size: $(du -sk --exclude=DEBIAN "$S" | cut -f1)
Homepage: https://github.com/adswill/OnAir
Depends: $DEPENDS
Recommends: zenity, libwayland-client0, libwayland-cursor0, libwayland-egl1, libxkbcommon0, libxinerama1
Suggests: libuhd4.9.0
Description: DVB-T2, DVB-T and ATSC receiver for HackRF and other SDR radios
 Open-source digital TV receiver: DVB-T2, DVB-T and ATSC 1.0 (8-VSB) with live spectrum,
 constellations, quality and multipath analysis, video and audio playback, and a direction
 finder for the antenna. OnAir name and logo (c) 2026 Kristijonas Adomaitis, all rights
 reserved; the logo is not covered by the GPL licence of the source code.
CTL
find "$S" -type f -exec chmod go-w {} +; find "$S" -type d -exec chmod 755 {} +
dpkg-deb --root-owner-group --build "$S" "$OUT/onair_${VERSION}_$ARCH.deb" >/dev/null
ls -la "$OUT/onair_${VERSION}_$ARCH.deb"
