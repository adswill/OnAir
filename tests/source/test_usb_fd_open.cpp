// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "open from descriptor" step the Android drivers share (dect2/source/usb_fd.h: usbFdPick / usbFdOpen), with a STUB in place of
// libusb_wrap_sys_device / airspy_open_fd, plus the plain-language errors, the Airspy serial parsing, the platform switch and the pushed
// sound-card input (audio_push.h, fed by Android's AudioRecord). No radio and no libusb are involved.
#include "dect2/source/audio_push.h"
#include "dect2/source/usb_fd.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dect2;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    // the platform switch: off on a desktop unless the tests ask for it
#if !defined(__ANDROID__)
    CHECK(!usbFdPlatform() || (getenv("DECT2_USB_FD") && *getenv("DECT2_USB_FD") == '1'));
#endif

    // nothing attached: a plain reason, no call of the wrapper
    std::string err;
    int calls = 0;
    auto stub = [&](int rc) { return [&calls, rc](int) { calls++; return rc; }; };
    CHECK(usbFdOpen("hackrf", "", stub(0), nullptr, err) == -4 && calls == 0 && err.find("permission") != std::string::npos);

    UsbFdRadio h1; h1.name = "/dev/bus/usb/002/003"; h1.kind = "hackrf"; h1.fd = 41; h1.vid = 0x1d50; h1.pid = 0x6089; h1.serial = "0000000000000000a06063c8234e925f";
    UsbFdRadio h2 = h1; h2.name = "/dev/bus/usb/002/004"; h2.fd = 42; h2.serial = "000000000000000057b068dc2e5a1a1b";
    CHECK(usbFdAttach(h1) && usbFdAttach(h2));

    // the first one when no serial is asked for; the descriptor handed to the wrapper is the registered one
    int seenFd = -1;
    UsbFdRadio got;
    err.clear();
    CHECK(usbFdOpen("hackrf", "", [&](int fd) { seenFd = fd; return 0; }, &got, err) == 0 && seenFd == 41 && got.name == h1.name && err.empty());
    // the end of a serial number picks the radio, as hackrf_info / the desktop driver match it
    CHECK(usbFdOpen("hackrf", "2e5a1a1b", [&](int fd) { seenFd = fd; return 0; }, &got, err) == 0 && seenFd == 42);
    // a serial that is not there: no wrapper call, the reason names the count
    calls = 0;
    CHECK(usbFdOpen("hackrf", "deadbeef", stub(0), nullptr, err) == -4 && calls == 0 && err.find("deadbeef") != std::string::npos);
    // the wrapper fails: its code comes back and the reason is in plain words
    CHECK(usbFdOpen("hackrf", "", stub(-3), nullptr, err) == -3 && err.find("Allow") != std::string::npos);
    CHECK(usbFdOpen("hackrf", "", stub(-6), nullptr, err) == -6 && err.find("Another app") != std::string::npos);
    CHECK(usbFdOpen("hackrf", "", stub(-4), nullptr, err) == -4 && err.find("power") != std::string::npos);
    // another kind is not mixed in
    CHECK(usbFdOpen("airspy", "", stub(0), nullptr, err) == -4);
    usbFdDetach(h1.name);
    CHECK(usbFdOpen("hackrf", "", [&](int fd) { seenFd = fd; return 0; }, nullptr, err) == 0 && seenFd == 42);   // hot-unplug of the first
    usbFdDetach(h2.name);
    CHECK(usbFdList("hackrf").empty());

    // every error code has a text; unknown ones too
    for (int c = -12; c <= -1; c++) CHECK(!usbFdErrorText(c).empty());
    CHECK(usbFdErrorText(0).empty() && usbFdErrorText(-99).find("-99") != std::string::npos);

    // Airspy serial numbers from the USB string
    CHECK(usbFdAirspySerial("AIRSPY SN:44A06763C22E8B4F") == 0x44A06763C22E8B4FULL);
    CHECK(usbFdAirspySerial("44a06763c22e8b4f") == 0x44A06763C22E8B4FULL);
    CHECK(usbFdAirspySerial("") == 0 && usbFdAirspySerial("SN: ") == 0);

    // the USB ids of the device filter
    CHECK(usbFdKindForId(0x0bda, 0x2832) == "rtlsdr" && usbFdKindForId(0x0ccd, 0x00a9) == "rtlsdr" && usbFdKindForId(0x1d50, 0x604b) == "hackrf" &&
          usbFdKindForId(0x1d50, 0xcc15) == "hackrf" && usbFdKindForId(0x1df7, 0x3000).empty());   // (an SDRplay RSP: not supported)

    // the pushed sound-card input: rate, order, overflow drops the oldest
    audioPushConfigure(48000, 0.1);   // 4800 frames
    CHECK(audioPushRate() == 48000 && audioPushAvailable() == 0);
    std::vector<float> in(2 * 6000), out(2 * 6000);
    for (size_t i = 0; i < 6000; i++) { in[2 * i] = (float)i; in[2 * i + 1] = -(float)i; }
    CHECK(audioPushWrite(in.data(), 1000) == 1000 && audioPushAvailable() == 1000);
    CHECK(audioPushRead(out.data(), 400) == 400 && out[0] == 0.f && out[2 * 399] == 399.f && out[2 * 399 + 1] == -399.f);
    CHECK(audioPushWrite(in.data() + 2 * 1000, 5000) == 4800);    // more than the buffer holds: 4800 kept, 800 of the oldest dropped
    CHECK(audioPushAvailable() == 4800 && audioPushDropped() == 800);
    CHECK(audioPushRead(out.data(), 6000) == 4800 && out[0] == 1200.f && out[2 * 4799] == 5999.f);
    CHECK(audioPushRead(out.data(), 10) == 0);
    audioPushReset();
    CHECK(audioPushAvailable() == 0 && audioPushRate() == 48000);

    printf(failures ? "usb_fd_open: %d FAILED\n" : "usb_fd_open: ok\n", failures);
    return failures ? 1 : 0;
}
