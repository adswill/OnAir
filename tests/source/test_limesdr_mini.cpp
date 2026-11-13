// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The LimeSDR Mini against the fake LimeSuite library (native_fakes.cpp, FAKE_LIME_MODEL picks the board): discovery by name, the rate list,
// the receive input chosen by frequency, the gain mapping, the calibration calls and the error texts. No radio is involved.
#include "dect2/source/source.h"
#include "../../core/src/source/lime_board.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
using namespace dect2;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static double (*st)(const char*) = nullptr;
static bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

static std::vector<DeviceInfo> limes() {
    std::string e;
    std::vector<DeviceInfo> v;
    for (const auto& d : listNativeDevices(e)) if (d.board == "lime") v.push_back(d);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_limesdr_mini <fake library>\n"); return 2; }
    char tmpl[] = "/tmp/onair-lime-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
#ifdef __APPLE__
    const std::string lib = std::string(dir) + "/libLimeSuite.dylib";
#else
    const std::string lib = std::string(dir) + "/libLimeSuite.so";
#endif
    { std::ifstream i(argv[1], std::ios::binary); std::ofstream o(lib, std::ios::binary); o << i.rdbuf(); }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    setenv("FAKE_LIME_MODEL", "mini", 1);

    // the pure helpers
    {
        using namespace dect2::native;
        const LimeBoard a = limeBoard("LimeSDR Mini, media=USB 3.0, module=FT601, addr=24607:1027, serial=1D40CA8B2ED29A");
        CHECK(a.mini && !a.v2 && a.usb3 && a.model == "LimeSDR Mini" && a.serial == "1D40CA8B2ED29A", "Mini 1.x parsed as %s", a.model.c_str());
        const LimeBoard b = limeBoard("LimeSDR Mini v2.4, media=USB 2.0, module=FT601, addr=1:2, serial=AB");
        CHECK(b.mini && b.v2 && !b.usb3 && b.model == "LimeSDR Mini 2", "Mini 2 parsed as %s", b.model.c_str());
        CHECK(limeBoard("LimeSDR-Mini_v2, media=USB 3.0").model == "LimeSDR Mini 2", "old Mini 2 name");
        const LimeBoard u = limeBoard("LimeSDR-USB, media=USB 3.0, module=FX3, addr=1d50:6108, serial=1");
        CHECK(!u.mini && u.model == "LimeSDR-USB", "LimeSDR-USB is not a Mini");
        const std::vector<std::string> n = {"NONE", "LNAH", "LNAL_NC", "LNAW"};
        CHECK(limeAutoAntenna(a, 100e6, n) == 3 && limeAutoAntenna(a, 1699e6, n) == 3 && limeAutoAntenna(a, 1700e6, n) == 1 && limeAutoAntenna(a, 3400e6, n) == 1, "Mini input by frequency");
        const std::vector<std::string> m = {"NONE", "LNAH", "LNAL", "LNAW"};
        CHECK(limeAutoAntenna(u, 522e6, m) == 2 && limeAutoAntenna(u, 1499e6, m) == 2 && limeAutoAntenna(u, 1500e6, m) == 1, "LimeSDR-USB input by frequency");
        CHECK(!limeHasExtRef(a) && !limeHasExtRef(b) && limeHasExtRef(u), "reference clock option: Mini none, LimeSDR-USB yes");
        CHECK(limeAutoAntenna(a, 100e6, {"NONE"}) == -1, "no usable input");
        CHECK(limeGainDb(-5) == 0 && limeGainDb(30.4) == 30 && limeGainDb(30.6) == 31 && limeGainDb(200) == 73, "gain mapping");
        CHECK(has(limeOpenHint(a, false), "LimeUtil --update") && !has(limeOpenHint(a, false), "FTDI") && has(limeOpenHint(a, true), "FTDI") && !has(limeOpenHint(u, true), "FTDI"), "open hint");
        const auto r = limeMiniRates();
        CHECK(r.size() == 9 && r.front().first == 1e6 && r.back().first == 30.72e6, "rate list");
    }

    // discovery: by name, the capabilities, one entry per usable socket
    auto v = limes();
    CHECK(v.size() == 3, "Mini entries: %zu (automatic, LNAW, LNAH)", v.size());
    if (v.size() == 3) {
        const DeviceInfo& d = v[0];
        CHECK(has(d.name, "LimeSDR Mini (native") && !has(d.name, "LNA"), "name: %s", d.name.c_str());
        CHECK(d.serial == "1D40CA8B2ED29A", "serial %s", d.serial.c_str());
        CHECK(d.minFreqHz == 10e6 && d.maxFreqHz == 3.5e9 && d.maxRateHz == 30.72e6 && d.gainMaxDb == 73, "capabilities %.0f-%.0f Hz, %.0f sps", d.minFreqHz, d.maxFreqHz, d.maxRateHz);
        CHECK(d.rateRanges.size() == 9 && d.rateRanges[4].first == 8e6 && d.steadyRateHz == 0, "rates %zu, steady %.0f", d.rateRanges.size(), d.steadyRateHz);
        CHECK(has(v[1].name, "LNAW") && has(v[2].name, "LNAH"), "socket entries: %s / %s", v[1].name.c_str(), v[2].name.c_str());
    }
    setenv("FAKE_LIME_MODEL", "mini2", 1);
    v = limes();
    CHECK(!v.empty() && has(v[0].name, "LimeSDR Mini 2 (native"), "Mini 2 name: %s", v.empty() ? "-" : v[0].name.c_str());
    setenv("FAKE_LIME_MODEL", "mini-usb2", 1);
    v = limes();
    CHECK(!v.empty() && v[0].steadyRateHz == 15e6, "USB 2 steady rate %.0f", v.empty() ? -1.0 : v[0].steadyRateHz);
    setenv("FAKE_LIME_MODEL", "twomini", 1);
    v = limes();
    CHECK(v.size() == 6 && has(v[0].name, "ED29A") && has(v[3].name, "ED2FF") && v[0].name != v[3].name, "two Minis told apart: %s / %s", v.empty() ? "-" : v[0].name.c_str(), v.size() > 3 ? v[3].name.c_str() : "-");
    setenv("FAKE_LIME_MODEL", "none", 1);
    CHECK(limes().empty(), "no radio, no entry");

    // streaming: input by frequency, gain, calibration, USB link format
    setenv("FAKE_LIME_MODEL", "mini", 1);
    v = limes();
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);
    st = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    CHECK(st != nullptr, "fake library not loaded");
    if (!st || v.empty()) return 1;
    {
        IqRing ring(1 << 22);
        auto src = makeSource(v[0]);
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 8e6; t.gainDb = 45.4;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "start: %s", e.c_str());
        CHECK(st("lime.antenna") == 3, "100 MHz: antenna %.0f (LNAW, 3 expected: the Mini's LNAL is not connected)", st("lime.antenna"));
        CHECK(st("lime.gain") == 45 && st("lime.rate") == 8e6 && st("lime.linkfmt") == 2 && st("lime.fmt") == 0, "gain %.0f rate %.0f link %.0f", st("lime.gain"), st("lime.rate"), st("lime.linkfmt"));
        CHECK(st("lime.calibrations") == 1 && st("lime.calbw") >= 2.5e6, "calibrated at start: %.0f", st("lime.calibrations"));
        t.centerHz = 2400e6;
        CHECK(src->retune(t, e) && st("lime.antenna") == 1, "2.4 GHz: antenna %.0f (LNAH, 1 expected)", st("lime.antenna"));
        CHECK(st("lime.calibrations") == 2, "calibrated again after a big retune: %.0f", st("lime.calibrations"));
        t.centerHz = 2410e6; t.gainDb = 20;
        CHECK(src->retune(t, e) && st("lime.calibrations") == 2 && st("lime.gain") == 20, "a small retune does not recalibrate; gain %.0f", st("lime.gain"));
        t.centerHz = 4e9;
        e.clear();
        CHECK(src->retune(t, e) && has(e, "specified for 10 to 3500 MHz"), "above 3.5 GHz is passed on with a note: %s", e.c_str());
        src->stop();
        // a rate change restarts the stream and calibrates again
        t.centerHz = 100e6; t.sampleRate = 20e6;
        const double c0 = st("lime.calibrations");
        const bool ok2 = src->start(t, ring, e);
        CHECK(ok2 && st("lime.calibrations") == c0 + 1 && st("lime.rate") == 20e6, "20 Msps start: %s", e.c_str());
        src->stop();
        // the LNAH entry stays on LNAH at 100 MHz
        auto fixed = makeSource(v[2]);
        t.sampleRate = 8e6;
        CHECK(fixed->start(t, ring, e) && st("lime.antenna") == 1, "LNAH entry: antenna %.0f", st("lime.antenna"));
        fixed->stop();
    }
    // the USB 2 note
    setenv("FAKE_LIME_MODEL", "mini-usb2", 1);
    v = limes();
    if (!v.empty()) {
        IqRing ring(1 << 22);
        auto src = makeSource(v[0]);
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 20e6; t.gainDb = 30;
        std::string e;
        CHECK(src->start(t, ring, e) && has(e, "USB 2 port") && has(e, "USB 3"), "USB 2 note: %s", e.c_str());
        src->stop();
    }
    // error paths: the library's own words and what to do about them
    setenv("FAKE_LIME_MODEL", "mini", 1);
    v = limes();
    if (!v.empty()) {
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 8e6; t.gainDb = 30;
        std::string e;
        setenv("FAKE_LIME_INIT_FAIL", "1", 1);
        auto a = makeSource(v[0]);
        CHECK(!a->start(t, ring, e) && has(e, "initialisation failed") && has(e, "Gateware version mismatch") && has(e, "LimeUtil --update"), "init failure: %s", e.c_str());
        unsetenv("FAKE_LIME_INIT_FAIL");
        setenv("FAKE_LIME_OPEN_FAIL", "1", 1);
        e.clear();
        auto b = makeSource(v[0]);
        CHECK(!b->start(t, ring, e) && has(e, "cannot open the radio") && has(e, "LimeUtil --update"), "open failure: %s", e.c_str());
        unsetenv("FAKE_LIME_OPEN_FAIL");
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
