// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// USB radios that the platform opened for us and handed over as a file descriptor (Android: UsbManager.openDevice().getFileDescriptor(), no root,
// no bus scan). The app registers each opened radio here; the drivers list and open them from this registry instead of scanning the USB bus.
// The descriptor stays owned by the app (UsbDeviceConnection); detach before closing it.
//
// How each driver opens a registered radio (android/docs/USB.md):
//  - RTL-SDR: the Rust protocol layer over usbfs ioctls (rtl_fd_shim.h, DECT2_RUST_USB), no libusb.
//  - HackRF:  OnAir's own driver (hackrf_usb.cpp) on libusb built with LIBUSB_OPTION_NO_DEVICE_DISCOVERY, the descriptor wrapped with
//             libusb_wrap_sys_device (usbFdOpen below).
//  - Airspy R2 / Mini, HF+: libairspy / libairspyhf when the app ships them, through their airspy_open_fd / airspyhf_open_fd.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct UsbFdRadio {
    std::string name;          // the platform's device name (Android: "/dev/bus/usb/001/004"), the key for detach
    std::string kind;          // which driver: "rtlsdr", "hackrf", "airspy", "airspyhf"
    int fd = -1;               // usbfs descriptor, read/write
    int vid = 0, pid = 0;
    std::string manufacturer, product, serial;   // the strings the platform read (may be empty)
};

// Adds the radio (replaces one with the same name). false: bad descriptor or empty name.
bool usbFdAttach(const UsbFdRadio& r);
// Removes it (hot-plug out, or the app is done with the connection). Streams on it fail and the driver reopens or stops.
void usbFdDetach(const std::string& name);
// The attached radios of one kind ("" = all), in the order they were attached.
std::vector<UsbFdRadio> usbFdList(const std::string& kind = "");
// Which driver an USB id belongs to ("" if none of the supported radios). Same table as the app's device filter
// (android/app/src/main/res/xml/usb_device_filter.xml, android/.../usb/UsbCatalog.kt).
std::string usbFdKindForId(int vid, int pid);

// True where the radios come from the platform as descriptors (Android): the drivers then list and open the registry and never scan the
// USB bus (a scan fails without root there). Elsewhere false, unless the environment has DECT2_USB_FD=1 (tests).
bool usbFdPlatform();

// The radio of `kind` to open: the one whose serial number is `serialWish` or ends with it (as the radios' own tools match), the first one
// when the wish is empty. false: err says why in plain words (none plugged in, none with that serial).
bool usbFdPick(const std::string& kind, const std::string& serialWish, UsbFdRadio& out, std::string& err);

// A libusb error code (or a usbfs -errno mapped to one, usb_fd_io.h) as the user should read it on a phone: cable, power, permission,
// another app.
std::string usbFdErrorText(int libusbCode);

// The open-from-descriptor step the drivers share: pick the radio, hand its descriptor to `wrap` (libusb_wrap_sys_device in the HackRF
// driver, airspy_open_fd in the Airspy one, a stub in the tests). wrap returns 0 or a libusb error code. Returns that code (-4, "no
// device", when nothing was picked); on failure err is the plain-language reason.
template <class Wrap>
int usbFdOpen(const std::string& kind, const std::string& serialWish, Wrap&& wrap, UsbFdRadio* picked, std::string& err) {
    UsbFdRadio r;
    if (!usbFdPick(kind, serialWish, r, err)) return -4;
    const int rc = wrap(r.fd);
    if (rc != 0) { err = usbFdErrorText(rc); return rc; }
    if (picked) *picked = r;
    return 0;
}

// The serial number of an Airspy from its USB serial string ("AIRSPY SN:0123456789ABCDEF", or the hex digits alone); 0 when it has none.
uint64_t usbFdAirspySerial(const std::string& usbSerial);

} // namespace dect2
