// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #663 / #337: "Treat as RTL-SDR Blog V4 / V4L" for a V4 that reports other USB strings, against the fake librtlsdr (native_fakes.cpp).
// A generic dongle samples the Q branch below 24 MHz; with the option it goes through the library's upconverter (no direct sampling).
#include "dect2/source/source.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
using namespace dect2;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static DeviceInfo rtl() {
    std::string e;
    for (const auto& d : listNativeDevices(e)) if (d.board == "rtlsdr") return d;
    return DeviceInfo();
}
static bool hasSetting(const DeviceInfo& d, const char* key) { for (const auto& s : d.settings) if (s.key == key) return true; return false; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_rtl_v4_manual <fake library>\n"); return 2; }
    char tmpl[] = "/tmp/onair-v4m-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
#ifdef __APPLE__
    const std::string lib = std::string(dir) + "/librtlsdr.dylib";
#else
    const std::string lib = std::string(dir) + "/librtlsdr.so";
#endif
    { std::ifstream i(argv[1], std::ios::binary); std::ofstream o(lib, std::ios::binary); o << i.rdbuf(); }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    (void)rtl();
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    auto st = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    CHECK(set && st, "fake library not loaded");
    if (!set || !st) return 1;

    const DeviceInfo g = rtl();
    CHECK(hasSetting(g, "v4"), "a generic dongle offers 'Treat as RTL-SDR Blog V4 / V4L'");
    IqRing ring(1 << 22);
    std::string e;
    {   // without the option: Q-branch direct sampling at 7.1 MHz (a stock dongle)
        auto s = makeSource(g);
        TuneSettings t; t.centerHz = 7.1e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        const bool ok = s && s->start(t, ring, e);
        CHECK(ok && st("rtl.ds") == 2, "generic at 7.1 MHz: Q branch (%.0f) %s", st("rtl.ds"), e.c_str());
        if (ok) s->stop();
    }
    set("rtl.ds", 0);   // (the fake keeps the last value; a source that never asks for direct sampling leaves it as it is)
    {   // with it: no direct sampling, the library gets the real frequency (it upconverts itself)
        auto s = makeSource(g);
        TuneSettings t; t.centerHz = 7.1e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 0.2; t.radio["v4"] = "1";
        e.clear();
        const bool ok = s && s->start(t, ring, e);
        CHECK(ok && st("rtl.ds") == 0 && st("rtl.freq") == 7.1e6, "manual V4 at 7.1 MHz: ds %.0f freq %.0f (%s)", st("rtl.ds"), st("rtl.freq"), e.c_str());
        if (ok) { t.centerHz = 14.2e6; CHECK(s->retune(t, e) && st("rtl.ds") == 0 && st("rtl.freq") == 14.2e6, "retune to 14.2 MHz keeps the upconverter path"); s->stop(); }
    }
    // A V4L whose USB strings can only be read before the open (librtlsdr: -3 on a dongle it has open) stays a V4: the identity
    // is read before rtlsdr_open, and check_dongle_model is a comparison, not a getter (#663).
    setenv("FAKE_RTL_STRINGS_BUSY", "1", 1);
    set("rtl.v4l", 1); set("rtl.ds", 0);
    CHECK(!hasSetting(rtl(), "v4"), "a listed V4L has no manual option");
    {
        auto s = makeSource(rtl());
        TuneSettings t; t.centerHz = 9.6e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        e.clear();
        const bool ok = s && s->start(t, ring, e);
        CHECK(ok && st("rtl.ds") == 0 && st("rtl.freq") == 9.6e6, "a V4L with unreadable strings once open: ds %.0f freq %.0f (%s)", st("rtl.ds"), st("rtl.freq"), e.c_str());
        if (ok) s->stop();
    }
    unsetenv("FAKE_RTL_STRINGS_BUSY"); set("rtl.v4l", 0); set("rtl.ds", 0);

    set("rtl.v4", 1);
    CHECK(!hasSetting(rtl(), "v4"), "a recognised V4 has no manual option");
    set("rtl.v4", 0); set("rtl.v3", 1);
    CHECK(!hasSetting(rtl(), "v4"), "a V3 has no manual option");
    set("rtl.v3", 0); set("rtl.dxp", 1);
    CHECK(!hasSetting(rtl(), "v4"), "a DXpatrol has no manual option");
    printf(failures ? "rtl_v4_manual: %d FAILED\n" : "rtl_v4_manual: ok\n", failures);
    return failures ? 1 : 0;
}
