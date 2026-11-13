// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the fourth block of 4.0 signal modes (families 779 and up). Generators exist only in the tests; none is reachable from the transmit side of the application.
#pragma once
#include "x40_gen3.h"
#include <functional>

namespace x40gen {

// ---- keyed tone on an AM carrier (ILS marker beacons) ----
// elements: (on seconds, off seconds) repeated; tone in Hz; depth of the tone in percent of the carrier; lo: the carrier's offset from the tuned frequency; fade: slow fading depth (0..1) at fadeHz
struct KeyedAm {
    std::vector<std::pair<double, double>> elems; double toneHz = 400, depth = 0.95, lo = 0, noise = 0, fadeDepth = 0, fadeHz = 0.3, startSec = 1.0, stopSec = 1e9, tone2Hz = 0, tone2Depth = 0; double fs = 12000; unsigned seed = 1;
    Rng rng{1}; double ph = 0, pt = 0, pt2 = 0; std::vector<double> edges; double cycle = 0;
    void prep() { cycle = 0; for (auto& e : elems) cycle += e.first + e.second; rng = Rng(seed); }
    bool keyed(double t) const {
        if (t < startSec || t >= stopSec || elems.empty()) return false;
        double u = std::fmod(t - startSec, cycle);
        for (auto& e : elems) { if (u < e.first) return true; u -= e.first; if (u < e.second) return false; u -= e.second; }
        return false;
    }
    std::vector<cf32> operator()(double t0, size_t n) {
        if (cycle == 0) prep();
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            pt += 2 * kPi * toneHz / fs; if (pt > 2 * kPi) pt -= 2 * kPi;
            pt2 += 2 * kPi * tone2Hz / fs; if (pt2 > 2 * kPi) pt2 -= 2 * kPi;
            double env = 1.0;
            if (keyed(t)) env += depth * std::sin(pt);
            if (tone2Hz > 0 && keyed(t)) env += tone2Depth * std::sin(pt2);
            env *= 1.0 + fadeDepth * std::sin(2 * kPi * fadeHz * t);
            ph += 2 * kPi * lo / fs; if (ph > 2 * kPi) ph -= 2 * kPi; else if (ph < -2 * kPi) ph += 2 * kPi;
            x[i] = (float)env * cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(rng, sg, sg);
        }
        return x;
    }
};
// the three markers of Annex 10 (the element lengths are the generator's assumption: a dash is three dots)
inline std::vector<std::pair<double, double>> outerMarker() { return {{0.30, 0.20}}; }                    // two dashes per second
inline std::vector<std::pair<double, double>> middleMarker() { return {{0.10, 0.10}, {0.30, 0.13}}; }      // 95 dot-dash pairs per minute (period 0.63 s)
inline std::vector<std::pair<double, double>> innerMarker() { return {{0.08, 0.0867}}; }                    // six dots per second


// ---- asynchronous FSK modem tones (Bell 103, V.21, V.23, Bell 202) in a single-sideband channel ----
struct ModemTones { const char* name; double mark, space, baud; };
inline const ModemTones& modemStd(int i) {
    static const ModemTones k[] = {{"Bell 103 originate", 1270, 1070, 300}, {"Bell 103 answer", 2225, 2025, 300}, {"V.21 channel 1", 980, 1180, 300}, {"V.21 channel 2", 1650, 1850, 300}, {"V.23 mode 2", 1300, 2100, 1200}, {"Bell 202", 1200, 2200, 1200}};
    return k[i];
}
// a burst: idle mark, the bytes (start, 8 data LSB first, stopBits marks), idle mark. dial: error of the tuned frequency in Hz (all tones move); lower: the lower sideband (negative frequencies); gapBits: extra idle marks between bytes
inline std::vector<cf32> modemBurst(int std_, const std::vector<uint8_t>& bytes, double fs, double leadSec, double tailSec, double idleSec, double dial = 0, bool lower = false, double noise = 0, unsigned seed = 1, double clockPpm = 0, int stopBits = 1, int gapBits = 0, double amp = 0.5, double baudScale = 1.0) {
    const ModemTones& m = modemStd(std_);
    std::vector<int> bits;
    for (uint8_t b : bytes) { bits.push_back(0); for (int k = 0; k < 8; k++) bits.push_back((b >> k) & 1); for (int k = 0; k < stopBits + gapBits; k++) bits.push_back(1); }
    const double T = fs / (m.baud * baudScale) * (1 + clockPpm * 1e-6);
    const size_t lead = (size_t)(leadSec * fs), idle = (size_t)(idleSec * fs), tail = (size_t)(tailSec * fs);
    const size_t body = (size_t)(bits.size() * T) + 2;
    std::vector<cf32> x(lead + idle + body + idle + tail, cf32(0, 0));
    double ph = 0; const double sg = lower ? -1.0 : 1.0;
    for (size_t i = lead; i < lead + idle + body + idle; i++) {
        int bit = 1; const double pos = (double)(i - lead - idle);
        if (pos >= 0) { const size_t k = (size_t)(pos / T); bit = k < bits.size() ? bits[k] : 1; }
        const double f = sg * ((bit ? m.mark : m.space) + dial);
        ph += 2 * kPi * f / fs; if (ph > 2 * kPi) ph -= 2 * kPi; else if (ph < -2 * kPi) ph += 2 * kPi;
        x[i] = (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    if (noise > 0) addNoise(x, noise, seed);
    return x;
}


// ---- secondary surveillance radar interrogations on 1030 MHz ----
// kind: 0 Mode 1, 1 Mode 2, 2 Mode A, 3 Mode B, 4 Mode C, 5 Mode D, 6 Mode S (56 bit), 7 Mode S (112 bit); p4: 0 none, 1 short (0.8 us), 2 long (1.6 us); p2: amplitude of P2 relative to P1 (0: none)
struct SsrTrain { int kind = 2; double prf = 300, amp = 1.0, p2 = 0, p3amp = 1.0; int p4 = 0; double start = 0.01, stop = 1e9, jitterUs = 0.05; };
inline double ssrSpacing(int kind) { static const double sp[] = {3, 5, 8, 17, 21, 25}; return kind < 6 ? sp[kind] : 0; }
inline std::vector<cf32> ssrScene(double fs, double seconds, const std::vector<SsrTrain>& trains, double noise, double lo = 0, unsigned seed = 1, double riseUs = 0.1) {
    const size_t n = (size_t)(fs * seconds); std::vector<float> env(n, 0.f); Rng rng(seed);
    auto pulse = [&](double t0us, double widthUs, double amp, double t0s) {
        const double a = t0s + t0us * 1e-6, b = a + widthUs * 1e-6, r = riseUs * 1e-6;
        const size_t i0 = (size_t)std::max(0.0, (a - r) * fs), i1 = std::min(n, (size_t)((b + r) * fs) + 1);
        for (size_t i = i0; i < i1; i++) {
            const double t = i / fs; double g = 1;
            if (t < a) g = 0.5 - 0.5 * std::cos(kPi * std::max(0.0, (t - (a - r))) / r); else if (t > b) g = 0.5 + 0.5 * std::cos(kPi * std::min(1.0, (t - b) / r));
            else g = 1.0;
            if (t < a - r || t > b + r) g = 0;
            env[i] = std::max(env[i], (float)(amp * g));
        }
    };
    for (const SsrTrain& tr : trains) {
        const double per = 1.0 / tr.prf;
        for (double t0 = tr.start; t0 < std::min(seconds - 1e-4, tr.stop); t0 += per * (1.0 + 0.04 * std::max(-1.0f, std::min(1.0f, rng.g() * 0.5f)))) {
            const double j = tr.jitterUs * rng.g() * 0.5;
            pulse(0, 0.8, tr.amp, t0);
            if (tr.p2 > 0) pulse(2.0, 0.8, tr.amp * tr.p2, t0);
            if (tr.kind < 6) { pulse(ssrSpacing(tr.kind) + j, 0.8, tr.amp * tr.p3amp, t0); if (tr.p4) pulse(ssrSpacing(tr.kind) + 2.0 + j, tr.p4 == 2 ? 1.6 : 0.8, tr.amp * tr.p3amp, t0); }
            else { pulse(2.0, 0.8, tr.amp, t0); pulse(4.75 + j, tr.kind == 6 ? 16.25 : 30.25, tr.amp, t0); }
        }
    }
    std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2); double ph = 0;
    for (size_t i = 0; i < n; i++) { ph += 2 * kPi * lo / fs; x[i] = env[i] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(rng, sg, sg); }
    return x;
}

}
