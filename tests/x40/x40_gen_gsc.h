// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "x40_gen.h"
#include "dect2/x40/gsc_proto.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace x40gen {

struct GscPath { double offsetHz = 0, driftHzPerSec = 0, ppm = 0, ppmPerSec = 0, devHz = 4500, lead = 1.0, tail = 1.0; bool invert = false; };

struct GscSeg { std::vector<int> bits; double voiceSec = 0; };

inline std::vector<cf32> gscSignal(const std::vector<GscSeg>& segs, double fs, const GscPath& p, unsigned seed = 7) {
    std::vector<float> fr;
    auto hold = [&](double sec, double hz) { const size_t n = (size_t)(sec * fs); for (size_t i = 0; i < n; i++) fr.push_back((float)hz); };
    hold(p.lead, 0);
    double t = p.lead;
    Rng r(seed);
    for (const GscSeg& s : segs) {
        double acc = 0;
        for (int v : s.bits) {
            const double ppm = p.ppm + p.ppmPerSec * t;
            const double dur = (1.0 / dect2::gsc::kFastBaud) * (1 + ppm * 1e-6);
            acc += dur * fs;
            const size_t n = (size_t)std::floor(acc); acc -= (double)n;
            const double hz = ((v ? 1 : -1) * (p.invert ? -1 : 1)) * p.devHz;
            for (size_t i = 0; i < n; i++) fr.push_back((float)hz);
            t += dur;
        }
        if (s.voiceSec > 0) {
            const size_t n = (size_t)(s.voiceSec * fs);
            double ph1 = 0, ph2 = 0, env = 0;
            for (size_t i = 0; i < n; i++) {
                if (i % 2000 == 0) env = 0.3 + 0.7 * std::fabs(r.g());
                ph1 += 2 * kPi * 430 / fs; ph2 += 2 * kPi * 1170 / fs;
                fr.push_back((float)(env * 1800 * (std::sin(ph1) + 0.6 * std::sin(ph2))));
            }
            t += s.voiceSec;
        }
    }
    hold(p.tail, 0);
    std::vector<float> sm(fr.size());
    const double a = 1 - std::exp(-2 * kPi * 900 / fs);
    double y = 0;
    for (size_t i = 0; i < fr.size(); i++) { y += a * (fr[i] - y); sm[i] = (float)y; }
    std::vector<cf32> x(sm.size());
    double ph = 0;
    for (size_t i = 0; i < sm.size(); i++) { x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * kPi * sm[i] / fs; if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi; }
    mix(x, p.offsetHz, p.driftHzPerSec, fs);
    return x;
}

inline void gscNoise(std::vector<cf32>& x, double snrDb, double fs, unsigned seed) { addNoise(x, std::pow(10.0, -snrDb / 10) * fs / 12500.0, seed); }

enum class GscType { Tone, Numeric, Alpha, Voice };
struct GscPage { int code = 0; int function = 1; GscType type = GscType::Tone; std::string text; double voiceSec = 2.0; };

inline void gscAddressOf(std::vector<int>& b, const GscPage& pg) {
    const auto w = dect2::gsc::wordsOfCode(pg.code);
    dect2::gsc::pushAddress(b, w.w1Index, w.w2, pg.function);
}
inline void gscFill(std::vector<int>& b, int n) { dect2::gsc::pushComma(b, n, 1); }

inline std::vector<GscSeg> gscSingle(const std::vector<GscPage>& pages) {
    std::vector<GscSeg> s;
    for (const GscPage& pg : pages) {
        GscSeg g;
        dect2::gsc::pushPreamble(g.bits, dect2::gsc::wordsOfCode(pg.code).preamble, false);
        dect2::gsc::pushStartCode(g.bits);
        gscAddressOf(g.bits, pg);
        if (pg.type == GscType::Numeric || pg.type == GscType::Alpha) dect2::gsc::pushMessage(g.bits, pg.text, pg.type == GscType::Numeric);
        if (pg.type == GscType::Voice) {
            dect2::gsc::pushActivation(g.bits);
            g.voiceSec = pg.voiceSec;
            s.push_back(g);
            g = GscSeg();
            dect2::gsc::pushActivation(g.bits);
        }
        gscFill(g.bits, 2 * dect2::gsc::kBlockBits);
        s.push_back(g);
    }
    return s;
}

inline std::vector<GscSeg> gscBatch(int preamble, const std::vector<GscPage>& pages) {
    GscSeg g;
    dect2::gsc::pushPreamble(g.bits, preamble, true);
    dect2::gsc::pushStartCode(g.bits);
    int blocks = 0;
    for (const GscPage& pg : pages) {
        gscAddressOf(g.bits, pg); blocks++;
        if (pg.type == GscType::Numeric || pg.type == GscType::Alpha) blocks += dect2::gsc::pushMessage(g.bits, pg.text, pg.type == GscType::Numeric);
    }
    while (blocks < 16) { gscFill(g.bits, dect2::gsc::kBlockBits); blocks++; }
    gscFill(g.bits, dect2::gsc::kBlockBits);
    return {g};
}

inline std::vector<GscSeg> gscNoPreamble(const std::vector<GscPage>& pages) {
    GscSeg g;
    for (int i = 0; i < 225; i++) { const int v = (i / 2) & 1; g.bits.push_back(v); g.bits.push_back(v); g.bits.push_back(v); g.bits.push_back(v); }
    for (const GscPage& pg : pages) {
        gscAddressOf(g.bits, pg);
        if (pg.type == GscType::Numeric || pg.type == GscType::Alpha) dect2::gsc::pushMessage(g.bits, pg.text, pg.type == GscType::Numeric);
    }
    gscFill(g.bits, 2 * dect2::gsc::kBlockBits);
    return {g};
}

inline std::vector<cf32> randomFsk(double fs, double seconds, double baud, double devHz, unsigned seed) {
    std::mt19937 r(seed);
    const size_t n = (size_t)(seconds * fs);
    std::vector<cf32> x(n);
    double ph = 0, hz = devHz, next = 0;
    for (size_t i = 0; i < n; i++) {
        if ((double)i >= next) { hz = (r() & 1) ? devHz : -devHz; next += fs / baud; }
        x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * kPi * hz / fs; if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    return x;
}

inline double gscSnrAt(const std::vector<std::pair<double, double>>& pts, double level) {
    for (size_t i = 0; i < pts.size(); i++) {
        if (pts[i].second >= level) {
            if (i == 0) return pts[0].first;
            const double a = pts[i - 1].second, b = pts[i].second;
            return pts[i - 1].first + (level - a) / std::max(1e-9, b - a) * (pts[i].first - pts[i - 1].first);
        }
    }
    return NAN;
}

}
