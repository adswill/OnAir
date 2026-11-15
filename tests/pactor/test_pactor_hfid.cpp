// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The PACTOR-I screen names a PACTOR-II / -III signal on the channel (physical layer only, generator signals at 250 ksps) and says the data is not
// decodable; noise alone names nothing.
#include "dect2/pactorhf/pactorhf_gen.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_tel.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250000;

static PactorTelemetry run(int variant, int level, double sec, bool noiseOnly) {
    PactorHfGenConfig c; c.variant = variant; c.level = level; c.snrDb = 20; c.amp = 0.08;
    c.rate = kRate; c.analytic = true; c.dialOffsetHz = -20000;
    PactorHfGenerator g(c);
    PactorReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(-20000);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 0.02f);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)(sec * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) {
        if (noiseOnly) for (auto& v : buf) v = cf32(nd(rng), nd(rng)); else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
    }
    r.flush();
    PactorTelemetry t;
    r.telemetry(t, 0);
    return t;
}

int main() {
    {
        const PactorTelemetry t = run(2, 0, 40, false);
        const std::string s = pactorSummary(t);
        CHECK(t.hfVariant == 2 && t.hfCarriers == 2 && t.blocksOk == 0, "PACTOR-II: variant %d carriers %d", t.hfVariant, t.hfCarriers);
        CHECK(s.find("PACTOR-II signal detected") != std::string::npos && s.find("not decodable") != std::string::npos, "summary: %s", s.c_str());
    }
    {
        const PactorTelemetry t = run(3, 6, 40, false);
        const std::string s = pactorSummary(t);
        CHECK(t.hfVariant == 3 && t.hfLevel == 6 && t.hfCarriers == 18, "PACTOR-III: variant %d level %d carriers %d", t.hfVariant, t.hfLevel, t.hfCarriers);
        CHECK(s.find("PACTOR-III signal detected, speed level 6") != std::string::npos, "summary: %s", s.c_str());
    }
    {
        const PactorTelemetry t = run(2, 0, 30, true);
        CHECK(t.hfVariant == 0 && pactorSummary(t) == "PACTOR-I: no signal", "noise: %s", pactorSummary(t).c_str());
    }
    return testSummary("pactor hfid");
}
