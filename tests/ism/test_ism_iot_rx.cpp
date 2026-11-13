// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors, smart-home buses over the air (synthetic): KNX RF and HomeMatic BidCoS in the 868.3 MHz scene (ismTestScene(3)) through the
// receiver with an offset, noise, a second transmitter on the same frequency and cut-off packets. Every reading must be one of the two test
// devices with its right values (a wrong reading fails every case); the clean cases must hear both. Simulated signals only.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_tel.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 2e6, kDial = -250e3;
struct Result { std::set<int> heard; int readings = 0, wrong = 0; };

static Result run(const std::vector<cf32>& x) {
    Result r;
    IsmReceiver rx;
    rx.configure(kRate);
    rx.setSignalOffset(kDial);
    rx.setReadingTap([&](const IsmReading& rd, double) {
        r.readings++;
        IsmTx w;
        if (ismTruthMatches(rd, &w)) r.heard.insert((int)w);
        else { r.wrong++; printf("    WRONG: %s id %s: %s\n", rd.model.c_str(), rd.id.c_str(), ismReadingText(rd.fields).c_str()); }
    });
    for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    std::vector<cf32> z(65536, cf32(0, 0));
    rx.feed(z.data(), z.size());
    return r;
}
static std::vector<cf32> make(const IsmGenConfig& c, double sec) {
    IsmGenerator g(c);
    std::vector<cf32> x((size_t)(sec * c.rate));
    g.generate(x.data(), x.size());
    return x;
}
static IsmGenConfig scene(double snr, uint32_t seed = 5) {
    IsmGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kDial; c.snrDb = snr; c.seed = seed;
    c.devs = ismTestScene(3);
    return c;
}

int main() {
    const size_t n = ismTestScene(3).size();
    auto report = [&](const char* name, const Result& r) { printf("  %-34s %zu/%zu heard, %3d readings, %d wrong\n", name, r.heard.size(), n, r.readings, r.wrong); };
    {
        const Result r = run(make(scene(30), 5.4));
        report("clean", r);
        CHECK(r.heard.size() == n && r.wrong == 0, "clean: %zu heard, %d wrong", r.heard.size(), r.wrong);
    }
    for (double off : {50e3, -50e3}) {
        auto x = make(scene(30), 5.4);
        impair::shift(x, off, kRate);
        const Result r = run(x);
        char nm[48]; snprintf(nm, sizeof nm, "offset %+.0f kHz", off / 1e3);
        report(nm, r);
        CHECK(r.heard.size() == n && r.wrong == 0, "%s: %zu heard, %d wrong", nm, r.heard.size(), r.wrong);
    }
    for (double snr : {20.0, 14.0}) {
        const Result r = run(make(scene(snr), 10.4));
        char nm[48]; snprintf(nm, sizeof nm, "%.0f dB C/N in 100 kHz", snr);
        report(nm, r);
        CHECK(r.wrong == 0 && (snr > 15 ? r.heard.size() == n : true), "%s: %zu heard, %d wrong", nm, r.heard.size(), r.wrong);
    }
    {   // cut-off packets: nothing wrong
        IsmGenConfig c = scene(30);
        for (auto& d : c.devs) d.truncate = 0.6;
        const Result r = run(make(c, 10.4));
        report("cut-off packets", r);
        CHECK(r.wrong == 0 && r.readings == 0, "cut-off: %d readings, %d wrong", r.readings, r.wrong);
    }
    {   // both on the same frequency at the same time: nothing wrong
        IsmGenConfig c = scene(30);
        for (auto& d : c.devs) { d.offsetHz = 0; d.phase = 0.4; }
        const Result r = run(make(c, 10.4));
        report("same frequency, same time", r);
        CHECK(r.wrong == 0, "collision: %d wrong", r.wrong);
    }
    {   // noise and zeros
        IsmGenConfig c = scene(20);
        c.devs.clear();
        const Result r = run(make(c, 3.0));
        CHECK(r.readings == 0, "noise: %d readings", r.readings);
        std::vector<cf32> z(200000, cf32(0, 0));
        CHECK(run(z).readings == 0, "zeros");
    }
    return testSummary("ism iot rx");
}
