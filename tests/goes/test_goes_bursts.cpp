// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Full-scale bursts after silence into the three receivers that share one Gardner timing loop (GOES HRIT/LRIT, Arktika GGAK, FY-4 LRIT and
// FY-2 S-VISSR): a spike far above the level the AGC has settled on (or a burst after silence) gives a huge timing error. A negative step sent the loop backwards one
// sample at a time from far below zero (GOES hung for good under fuzz_engine_modes). Each receiver must take the whole input and keep
// going; ctest's timeout catches a hang, and the time is printed.
#include "dect2/elektro/elektro_ggak.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/goes/goes_rx.h"
#include <chrono>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

static std::vector<cf32> bursts(double rate) {
    std::mt19937 g(3);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> v;
    // normal-level noise (the AGC settles), silence (it decays towards zero), then full-scale bursts and single spikes of up to 1e5, which
    // the engine's input guard lets through (below 2^20): each gives a timing error of 1e10 and more against the settled AGC
    for (int b = 0; b < 30; b++) {
        for (int k = 0; k < 20000; k++) v.push_back(cf32(nd(g), nd(g)) * 0.05f);
        if (b % 3 == 0) v.insert(v.end(), (size_t)(rate * 0.05), cf32(0, 0));
        for (int k = 0; k < 500 + (int)(g() % 3000); k++) v.push_back(cf32(g() % 2 ? 1.f : -1.f, g() % 2 ? 1.f : -1.f));
        for (int k = 0; k < 4; k++) v.push_back(cf32(g() % 2 ? 1e5f : -1e5f, g() % 2 ? 3e4f : -3e4f));
    }
    return v;
}

template <class R>
static void run(const char* name, R& rx, double rate) {
    const std::vector<cf32> v = bursts(rate);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < v.size(); i += 8192) rx.feed(v.data() + i, std::min<size_t>(8192, v.size() - i));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  %-10s %.2f s of bursts in %.2f s\n", name, v.size() / rate, s);
    CHECK(s < 60, "%s: %.1f s for %.1f s of signal", name, s, v.size() / rate);
}

int main() {
    { GoesReceiver rx; const double r = goesTuning().sampleRate; rx.configure(r); run("GOES", rx, r); }
    { GgakReceiver rx; const double r = 2.4e6; rx.configure(r); run("GGAK", rx, r); }
    for (int link : {0, 3}) { FygeoReceiver rx; rx.setLink(link); const double r = (link == 3 ? fy2SvissrTuning() : fy4LritTuning()).sampleRate; rx.configure(r); run(link == 3 ? "FY-2" : "FY-4 LRIT", rx, r); }
    return testSummary("goes_bursts");
}
