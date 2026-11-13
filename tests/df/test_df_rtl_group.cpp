// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hub plumbing with fake dongles: hub detection and label, identical configuration of every link, noise switch around the calibration,
// and a calibrated bearing from u8 IQ streams of a synthetic array whose devices start at different offsets.
#include "dect2/df/rtl_group.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dect2::df;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double FREQ = 433e6, LAM = kC / FREQ;
static bool gNoise = false;
static int gConfigured = 0;

struct FakeLink : RtlLink {
    std::vector<uint8_t> d; size_t p = 0; double f = 0, r = 0; int g = 0;
    bool configure(double fr, double ra, int ga) override { f = fr; r = ra; g = ga; gConfigured++; return true; }
    int readSync(uint8_t* b, int n) override { int k = (int)std::min<size_t>(n, d.size() - p); std::copy(d.begin() + p, d.begin() + p + k, b); p += k; return k; }
    bool setGpio0(bool on) override { gNoise = on; return true; }
};
struct SwitchOnData : NoiseSwitch {   // the noise block is the first part of every stream; nothing to do but record the order
    std::vector<int> log;
    bool set(bool on) override { log.push_back(on); return true; }
};

static uint8_t q(double v) { return (uint8_t)std::lround(std::clamp(v * 40.0 + 127.5, 0.0, 255.0)); }

int main() {
    // detection
    std::vector<RtlInfo> all = {{0, "00000001", "RTL2838UHIDIR"}, {1, "1002", "RTL2832U"}, {2, "1000", "RTL2832U"}, {3, "1001", "RTL2832U"}, {4, "1003", "RTL2832U"}, {5, "1004", "RTL2832U"}};
    auto hub = findHub(all);
    CHECK(hub.size() == 5 && hub[0].serial == "1000" && hub[4].serial == "1004" && hubLabel(hub) == "KrakenSDR (5 ch)", "hub %zu %s", hub.size(), hubLabel(hub).c_str());
    CHECK(findHub({all[0], all[1]}).empty(), "a lone dongle is not a hub");

    Array arr = Array::uca(5, Array::ucaRadiusForSpacing(5, LAM, 0.5));
    const int lead[5] = {2, 0, 7, 4, 9};
    const size_t NOISE = 16384;
    double worst = 0;
    for (int trial = 0; trial < 4; trial++) {
        double b = 360.0 * (trial + 0.4) / 4;
        SimConfig cn; cn.array = arr; cn.freqHz = FREQ; cn.sampleRate = 250e3; cn.seed = 5 + trial; cn.calibrationNoise = true;
        for (int k = 0; k < 5; k++) cn.elementGains.push_back(std::polar(0.8 + 0.05 * k, 1.1 * k + 0.4));
        SimConfig cs = cn; cs.calibrationNoise = false; cs.seed = 40 + trial;
        SimSource s; s.bearingDeg = b; s.snrDb = 12; cs.sources.push_back(s);
        Frame fn, fs;
        SimMultiChannelSource a(cn), c(cs); a.read(fn, NOISE); c.read(fs, 6000);
        Rng rng(3 + trial);
        std::vector<std::unique_ptr<RtlLink>> links;
        for (int k = 0; k < 5; k++) {
            auto l = std::make_unique<FakeLink>();
            auto push = [&](cd z) { l->d.push_back(q(z.real())); l->d.push_back(q(z.imag())); };
            for (int i = 0; i < lead[k]; i++) push(cd(rng.gauss(), rng.gauss()) * 0.3);
            for (auto z : fn[k]) push(z * 0.3);
            for (auto z : fs[k]) push(z * 0.3);
            links.push_back(std::move(l));
        }
        gConfigured = 0;
        Hub h;
        HubStart hs; hs.freqHz = FREQ; hs.rateHz = 250e3; hs.gainTenthsDb = 207;
        CHECK(startHub(std::move(links), hs, h), "start");
        CHECK(gConfigured == 5, "every link configured");
        for (auto& l : h.links) { auto* f = static_cast<FakeLink*>(l.get()); CHECK(f->f == FREQ && f->r == 250e3 && f->g == 207, "same settings"); }
        SwitchOnData sw;
        CHECK(calibrateHub(h, sw, 8192, 32), "calibration (trial %d, coherence %.3f)", trial, h.group->calibration().coherence);
        CHECK(sw.log.size() == 2 && sw.log[0] == 1 && sw.log[1] == 0, "noise source on then off");
        Frame junk; h.group->read(junk, NOISE - h.group->calibration().consumed + 64);
        Frame x; h.group->read(x, 4096);
        Options o; o.method = Method::Music; o.sources = 1;
        worst = std::max(worst, std::abs(angDiff(estimate(covariance(x), arr, LAM, o).bearingDeg, b)));
    }
    printf("rtl_group: worst MUSIC error %.2f deg\n", worst);
    CHECK(worst < 2.0, "bearing error %.2f", worst);
    printf(fails ? "df_rtl_group: %d FAILED\n" : "df_rtl_group: all passed\n", fails);
    return fails ? 1 : 0;
}
