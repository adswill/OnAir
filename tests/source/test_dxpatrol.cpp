// The DXpatrol SDR (an RTL2832U dongle) against the fake librtlsdr (native_fakes.cpp, "rtl.dxp" = its USB strings): naming, the HF input
// (direct sampling) chosen by frequency, the range limits, the 3.2 Msps ceiling and the manual "Treat as DXpatrol" option. No radio is involved.
#include "dect2/source/source.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
using namespace dect2;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }
static DeviceInfo rtl() {
    std::string e;
    for (const auto& d : listNativeDevices(e)) if (d.board == "rtlsdr") return d;
    return DeviceInfo();
}
static bool hasSetting(const DeviceInfo& d, const char* key) { for (const auto& s : d.settings) if (s.key == key) return true; return false; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_dxpatrol <fake library>\n"); return 2; }
    char tmpl[] = "/tmp/onair-dxp-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
#ifdef __APPLE__
    const std::string lib = std::string(dir) + "/librtlsdr.dylib";
#else
    const std::string lib = std::string(dir) + "/librtlsdr.so";
#endif
    { std::ifstream i(argv[1], std::ios::binary); std::ofstream o(lib, std::ios::binary); o << i.rdbuf(); }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    void* h = nullptr;
    // load the library once through the listing, then drive the fake
    (void)rtl();
    h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    auto st = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    CHECK(set && st, "fake library not loaded");
    if (!set || !st) return 1;

    // a generic dongle first: no DXpatrol, 24 MHz floor, manual option offered
    {
        const DeviceInfo g = rtl();
        CHECK(!has(g.name, "DXpatrol") && g.minFreqHz == 24e6 && g.maxRateHz == 3.2e6, "generic: %s %.0f", g.name.c_str(), g.minFreqHz);   // (3.5: every RTL-SDR offers up to 3.2 Msps, c9e49a4f1)
        CHECK(hasSetting(g, "dxp"), "the manual 'Treat as DXpatrol SDR' option is missing");
    }
    set("rtl.dxp", 1);
    const DeviceInfo d = rtl();
    CHECK(has(d.name, "DXpatrol SDR (RTL2832U)") && has(d.name, "DXP00001") && d.serial == "DXP00001", "name: %s", d.name.c_str());
    CHECK(d.minFreqHz == 0.1e6 && d.maxFreqHz == 1766e6, "range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
    CHECK(d.maxRateHz == 3.2e6 && !d.rateRanges.empty() && d.rateRanges.back().second == 3.2e6, "rates up to %.0f", d.maxRateHz);
    CHECK(hasSetting(d, "ds") && hasSetting(d, "dxp"), "settings missing");
    for (const auto& s : d.settings) if (s.key == "ds") CHECK(s.label == "HF input (direct sampling)", "ds label %s", s.label.c_str());

    IqRing ring(1 << 22);
    auto src = makeSource(d);
    TuneSettings t;
    t.centerHz = 7.1e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
    std::string e;
    const bool ok = src && src->start(t, ring, e);
    CHECK(ok, "start at 7.1 MHz: %s", e.c_str());
    if (ok) {
        CHECK(st("rtl.ds") == 2 && st("rtl.freq") == 7.1e6 && !has(e, "stock RTL-SDR"), "7.1 MHz: Q-branch direct sampling %.0f (%s)", st("rtl.ds"), e.c_str());
        t.centerHz = 26e6;
        CHECK(src->retune(t, e) && st("rtl.ds") == 2, "26 MHz still on the HF input (%.0f)", st("rtl.ds"));
        t.centerHz = 28.1e6;
        CHECK(src->retune(t, e) && st("rtl.ds") == 0 && st("rtl.freq") == 28.1e6, "28.1 MHz: tuner (%.0f)", st("rtl.ds"));
        t.centerHz = 100e6;
        CHECK(src->retune(t, e) && st("rtl.ds") == 0 && st("rtl.freq") == 100e6, "100 MHz: tuner");
        t.centerHz = 0.15e6; e.clear();
        CHECK(src->retune(t, e) && st("rtl.ds") == 2 && !has(e, "specified for"), "150 kHz reachable by direct sampling: %s", e.c_str());
        t.centerHz = 0.05e6; e.clear();
        CHECK(src->retune(t, e) && has(e, "specified for 0.1 to 1766 MHz"), "50 kHz: note about the range: %s", e.c_str());
        t.centerHz = 1900e6; e.clear();
        CHECK(src->retune(t, e) && st("rtl.ds") == 0 && has(e, "specified for 0.1 to 1766 MHz"), "1.9 GHz: passed on with a note: %s", e.c_str());
        t.radio["ds"] = "off"; t.centerHz = 7.1e6; e.clear();
        CHECK(src->retune(t, e) && st("rtl.ds") == 0, "HF input off: tuner only (%.0f)", st("rtl.ds"));
        t.radio["ds"] = "q";
        CHECK(src->retune(t, e) && st("rtl.ds") == 2, "HF input on");
        src->stop();
    }
    src.reset();
    // up to 3.2 Msps is accepted
    {
        auto s3 = makeSource(d);
        TuneSettings u; u.centerHz = 14e6; u.sampleRate = 3.2e6; u.gainDb = 30; u.bandwidthMhz = 0.2;
        e.clear();
        const bool o3 = s3->start(u, ring, e);
        CHECK(o3 && st("rtl.rate") == 3.2e6, "3.2 Msps: %s (rate %.0f)", e.c_str(), st("rtl.rate"));
        if (o3) s3->stop();
    }
    // a generic-named dongle with the manual option behaves the same
    set("rtl.dxp", 0);
    {
        const DeviceInfo g = rtl();
        auto sg = makeSource(g);
        TuneSettings u; u.centerHz = 27e6; u.sampleRate = 2.4e6; u.gainDb = 30; u.bandwidthMhz = 0.2; u.radio["dxp"] = "1";
        e.clear();
        const bool og = sg->start(u, ring, e);
        CHECK(og && st("rtl.ds") == 2 && !has(e, "stock RTL-SDR"), "manual DXpatrol at 27 MHz: ds %.0f (%s)", st("rtl.ds"), e.c_str());
        if (og) sg->stop();
    }
    // the Blog V4 is not a DXpatrol (its upconverter handles HF): no direct sampling
    set("rtl.v4", 1);
    CHECK(!has(rtl().name, "DXpatrol") && !hasSetting(rtl(), "dxp"), "Blog V4 untouched");
    printf(failures ? "dxpatrol: %d FAILED\n" : "dxpatrol: ok\n", failures);
    return failures ? 1 : 0;
}
