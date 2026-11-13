// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WINMOR pilot detection: the pilot format is from the protocol specification; the 4FSK frame-type symbols are measured as raw tones. Simulated signals only.
#include "dect2/xlink/winmor.h"
#include "dect2/xlink/xlink_rx.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::xlink::winmor;

static XlinkTelemetry runRx(std::vector<cf32>& s, double fs) {
    XlinkReceiver rx;
    rx.setProtocols(xlink::Winmor);
    rx.configure(fs);
    for (size_t i = 0; i < s.size(); i += 2048) rx.feed(s.data() + i, std::min<size_t>(2048, s.size() - i));
    XlinkTelemetry t;
    rx.telemetry(t, ~0ull);
    return t;
}

int main() {
    const std::array<int, 4> tn{{1, 6, 3, 4}};
    for (double off : {0.0, 42.0, -83.0, 150.0}) {
        for (double sigma : {0.03, 0.15}) {
            auto f = txPilotFrame(24, tn, off, 12);
            std::vector<cf32> s(9000, cf32(0, 0));
            s.insert(s.end(), f.begin(), f.end());
            s.resize(s.size() + 6000, cf32(0, 0));
            std::mt19937 g(77);
            std::normal_distribution<float> n(0, (float)sigma);
            for (auto& v : s) v += cf32(n(g), n(g));
            auto t = runRx(s, 12000);
            int ok = 0;
            for (auto& fr : t.frames) if (fr.proto == xlink::Winmor) {
                ok++;
                if (std::fabs(fr.freqHz - off) > 6) { printf("FAIL offset %.0f: est %.1f (%s)\n", off, fr.freqHz, fr.detail.c_str()); return 1; }
                if (fr.detail.find("tones 1 6 3 4") == std::string::npos) { printf("FAIL tones off %.0f sigma %.2f: %s\n", off, sigma, fr.detail.c_str()); return 1; }
            }
            if (ok != 1) { printf("FAIL off %.0f sigma %.2f: %d pilots (%zu frames)\n", off, sigma, ok, t.frames.size()); return 1; }
        }
    }
    // through the resampler from a 96 kHz capture with a tuning error
    {
        auto f = txPilotFrame(30, tn, 60, 8);
        std::vector<cf32> up;
        up.resize(8 * f.size() + 8000, cf32(0, 0));
        for (size_t i = 0; i + 1 < f.size(); i++) for (int k = 0; k < 8; k++) { const float a = (float)k / 8; up[4000 + 8 * i + (size_t)k] = f[i] * (1 - a) + f[i + 1] * a; }
        auto t = runRx(up, 96000);
        if (t.frames.size() != 1) { printf("FAIL resampled: %zu\n", t.frames.size()); return 1; }
    }
    // noise only and a CW carrier: nothing
    {
        std::mt19937 g(11);
        std::normal_distribution<float> n(0, 0.2f);
        std::vector<cf32> s(600000);
        double ph = 0;
        for (auto& v : s) { v = cf32(n(g), n(g)); }
        auto t = runRx(s, 12000);
        if (!t.frames.empty()) { printf("FAIL noise gave pilots\n"); return 1; }
        for (auto& v : s) { v = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * 3.141592653589793 * 1500.0 / 12000; }
        auto t2 = runRx(s, 12000);
        if (!t2.frames.empty()) { printf("FAIL carrier gave pilots\n"); return 1; }
    }
    printf("winmor ok\n");
    return 0;
}
