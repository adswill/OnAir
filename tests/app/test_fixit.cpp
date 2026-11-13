// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "How to fix" table (app/panels/fixit.cpp): every pattern resolves to its own entry, ids and patterns are unique, every entry explains
// itself and has steps on every OS, and the buttons are complete.
#include "panels/fixit.h"
#include <cctype>
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    int n = 0;
    const fixit::Entry* t = fixit::table(n);
    CHECK(n > 10, "table has %d entries", n);
    std::set<std::string> ids, pats;
    for (int i = 0; i < n; i++) {
        const fixit::Entry& e = t[i];
        CHECK(ids.insert(e.id).second, "duplicate id %s", e.id);
        std::string p = e.pattern;
        for (char& c : p) c = (char)std::tolower((unsigned char)c);
        CHECK(pats.insert(p).second, "duplicate pattern %s", e.pattern);
        CHECK(*e.pattern, "%s has no pattern", e.id);
        const fixit::Entry* r = fixit::lookup(std::string("xx ") + e.pattern + " yy");
        CHECK(r == &e, "pattern of %s resolves to %s", e.id, r ? r->id : "nothing");
        CHECK(fixit::byId(e.id) == &e, "byId %s", e.id);
        CHECK(e.title && e.why && *e.title && *e.why, "%s lacks title or explanation", e.id);
        for (int os : {fixit::OsMac, fixit::OsWin, fixit::OsLinux}) CHECK(!fixit::stepsFor(e, os).empty(), "%s has no steps for os %d", e.id, os);
        for (int b = 0; b < 3 && e.buttons[b].label; b++) {
            CHECK(e.buttons[b].arg != nullptr, "%s button %d has no arg", e.id, b);
            if (e.buttons[b].act == fixit::Act::OpenUrl || e.buttons[b].act == fixit::Act::CopyText) CHECK(*e.buttons[b].arg, "%s button %d has an empty arg", e.id, b);
            CHECK(e.buttons[b].os != 0, "%s button %d is offered nowhere", e.id, b);
        }
    }
    // real messages the app shows
    CHECK(fixit::lookup("Could not start the radio: SDRplay: the SDRplay API service is not running") == fixit::byId("sdrplay_service"), "sdrplay service");
    CHECK(fixit::lookup("HackRF is plugged in but this user may not open it: install OnAir's udev rules") == fixit::byId("usb_permission"), "udev");
    CHECK(fixit::lookup("Could not start the radio: open: HackRF not found") == fixit::byId("hackrf_not_found"), "hackrf not found");
    CHECK(fixit::lookup("Could not start the radio: rtl_tcp: 1.2.3.4:1234 is not an rtl_tcp server") == fixit::byId("rtltcp"), "rtl_tcp");
    CHECK(fixit::lookup("Could not start the radio: something new") == fixit::byId("start_failed"), "generic start failure");
    CHECK(fixit::lookup("Could not start the radio: libusb: LIBUSB_ERROR_BUSY") == fixit::byId("radio_busy"), "busy (libusb)");
    CHECK(fixit::lookup("open failed: Device or resource busy") == fixit::byId("radio_busy2"), "busy (os)");
    CHECK(fixit::lookup("Could not start the radio: LIBUSB_ERROR_ACCESS") == fixit::byId("radio_access"), "access denied");
    CHECK(fixit::lookup("Could not start the radio: LIBUSB_ERROR_NOT_SUPPORTED") == fixit::byId("radio_driver"), "wrong driver");
    CHECK(fixit::lookup("everything is fine") == nullptr, "no match");
    if (!failures) std::printf("fixit: %d entries OK\n", n);
    return failures ? 1 : 0;
}
