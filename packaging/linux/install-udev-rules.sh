#!/bin/sh
# Lets a normal user open the USB radios (HackRF, RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP, SDRplay) and keeps the kernel's
# TV driver off RTL-SDR dongles. Run it once with sudo; then replug the radio (or reboot).   sudo ./install-udev-rules.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RULES=$HERE/udev/60-onair-sdr.rules
BLACKLIST=$HERE/udev/onair-rtlsdr-blacklist.conf
if [ "$(id -u)" != 0 ]; then
    if command -v sudo >/dev/null 2>&1; then exec sudo "$0" "$@"; fi
    echo "Run this as root (sudo $0)."; exit 1
fi
for f in "$RULES" "$BLACKLIST"; do [ -f "$f" ] || { echo "Missing $f"; exit 1; }; done
install -d /etc/udev/rules.d /etc/modprobe.d
install -m 644 "$RULES" /etc/udev/rules.d/60-onair-sdr.rules
echo "Installed /etc/udev/rules.d/60-onair-sdr.rules (radios can be opened without root)"
install -m 644 "$BLACKLIST" /etc/modprobe.d/onair-rtlsdr-blacklist.conf
echo "Installed /etc/modprobe.d/onair-rtlsdr-blacklist.conf (the kernel's TV driver stays off RTL-SDR dongles)"
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules && udevadm trigger --subsystem-match=usb && echo "Reloaded the udev rules for the radios that are plugged in"
fi
# a dongle the kernel already grabbed is let go now, unless it is in use (modprobe -r refuses a module that is in use)
if [ -r /proc/modules ] && command -v modprobe >/dev/null 2>&1 && grep -q '^dvb_usb_rtl28xxu ' /proc/modules; then
    for m in dvb_usb_rtl28xxu rtl2832_sdr rtl2832 rtl2830; do modprobe -r "$m" >/dev/null 2>&1 || true; done
    if grep -q '^dvb_usb_rtl28xxu ' /proc/modules; then echo "The kernel's TV driver is still loaded (in use): unplug the RTL-SDR and plug it in again after a reboot"
    else echo "Unloaded the kernel's TV driver for RTL-SDR dongles"; fi
fi
# the rules give access to the user at the desk (uaccess) and to the group plugdev
if getent group plugdev >/dev/null 2>&1 && [ -n "$SUDO_USER" ] && ! id -nG "$SUDO_USER" | grep -qw plugdev; then
    echo "To open radios from a remote or text login too: sudo usermod -aG plugdev $SUDO_USER (then log in again)"
fi
echo "Done. Replug the radio if OnAir does not see it yet."
