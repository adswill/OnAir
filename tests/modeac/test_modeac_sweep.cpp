// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply detector under real-world impairments, against the generator's own list: weaker and weaker signals, carrier errors of the
// whole capture, a narrow radio filter, a dense airspace, odd sample rates, a busy sky of Mode S traffic.
#include "dect2/modeac/modeac_gen.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Result { std::set<uint16_t> trueBits; size_t sent = 0, found = 0, bitsOk = 0, phantom = 0, spiOk = 0, spiSent = 0; double perSec = 0; ModeAcTelemetry tel; };

static Result run(ModeAcGenConfig c, double seconds, int confirm = 1) {
    ModeAcGenerator g(c);
    ModeAcReceiver rx;
    rx.configure(c.rate);
    rx.setConfirm(confirm);
    std::vector<ModeAcDemod::Raw> got;
    rx.setReplyCallback([&](const ModeAcDemod::Raw& r) { got.push_back(r); });
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(seconds * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        g.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    Result r;
    const double tEnd = (double)total / c.rate - 0.01;
    std::vector<bool> used(got.size(), false);
    for (const auto& t : g.sent()) {
        r.trueBits.insert(t.bits);
        if (t.t > tEnd || t.t < 0.05) continue;
        r.sent++;
        if (t.spi) r.spiSent++;
        for (size_t i = 0; i < got.size(); i++) {
            if (used[i] || std::fabs(got[i].timeSec - t.t) > 2e-6) continue;
            used[i] = true;
            r.found++;
            if (got[i].bits == t.bits) r.bitsOk++;
            if (t.spi && got[i].spi) r.spiOk++;
            break;
        }
    }
    for (size_t i = 0; i < got.size(); i++) if (!used[i] && got[i].timeSec > 0.05) r.phantom++;
    r.perSec = (double)got.size() / seconds;
    ModeAcTelemetry t;
    rx.telemetry(t, 0);
    r.tel = t;
    return r;
}

int main() {
    struct Case { const char* name; double rate; double snr; double cfo; double filter; int aircraft; double minFound; };
    const Case cases[] = {
        {"snr 40", 4e6, 40, 0, 0, 12, 0.85}, {"snr 20", 4e6, 20, 0, 0, 12, 0.25}, {"snr 12", 4e6, 12, 0, 0, 12, 0.05},
        {"cfo +300 kHz", 4e6, 30, 300e3, 0, 12, 0.80}, {"cfo -300 kHz", 4e6, 30, -300e3, 0, 12, 0.80},
        {"narrow filter 1.75 MHz at 4 Msps", 4e6, 30, 0, 1.75, 12, 0.80}, {"2.4 Msps", 2.4e6, 30, 0, 0, 12, 0.70}, {"2.56 Msps", 2.56e6, 30, 0, 0, 12, 0.70},
        {"8 Msps", 8e6, 30, 0, 0, 12, 0.85}, {"100 aircraft", 4e6, 30, 0, 0, 100, 0.60},
    };
    for (const Case& k : cases) {
        ModeAcGenConfig c; c.rate = k.rate; c.snrDb = k.snr; c.cfoHz = k.cfo; c.filterMHz = k.filter; c.aircraft = k.aircraft; c.seed = 21;
        const Result r = run(c, 8.0);
        printf("  %-34s sent %4zu found %4zu bits %4zu phantom %3zu\n", k.name, r.sent, r.found, r.bitsOk, r.phantom);
        CHECK(r.sent > 50, "%s: only %zu sent", k.name, r.sent);
        CHECK(r.found >= k.minFound * (double)r.sent, "%s: found %zu of %zu", k.name, r.found, r.sent);
        CHECK(r.bitsOk * 100 >= r.found * 96, "%s: %zu of %zu found replies have wrong bits", k.name, r.found - r.bitsOk, r.found);
        CHECK(r.phantom * 20 <= r.sent, "%s: %zu phantoms against %zu sent", k.name, r.phantom, r.sent);
    }
    return testSummary("modeac sweep");
}
