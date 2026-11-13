// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the 4.0 signal modes. These generators exist only in the tests; none is reachable from the transmit side of the application.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/x40/x40_rx.h"
#include "dect2/x40/x40_tel.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace x40gen {
using dect2::cf32;
inline constexpr double kPi = 3.14159265358979323846;

// Gaussian numbers that are the same on every platform. std::mt19937 is fixed by the standard but std::normal_distribution is not: libc++ (macOS)
// and libstdc++ (Linux, MinGW) draw different numbers from the same seed, so a marginal test saw another noise on each. The sensitivity tests
// were tuned on the CI runners (libstdc++), so this follows libstdc++'s float polar method step by step (the second number of a pair first); the
// maths functions are the platform's (a last-bit difference in logf does not matter for noise).
struct Rng {
    std::mt19937 r; float saved = 0; bool have = false;
    explicit Rng(unsigned s) : r(s) {}
    float canon() { float v = (float)(uint32_t)r() / 4294967296.0f; if (v >= 1.0f) v = std::nextafter(1.0f, 0.0f); return v; }
    float g() {
        if (have) { have = false; return saved; }
        float x, y, r2;
        do { x = 2.0f * canon() - 1.0f; y = 2.0f * canon() - 1.0f; r2 = x * x + y * y; } while (r2 > 1.0f || r2 == 0.0f);
        const float mult = std::sqrt(-2.0f * std::log(r2) / r2);
        saved = x * mult; have = true;
        return y * mult;
    }
};

// One complex Gaussian sample with the given scales. The two draws are made in a fixed order: cf32(s * r.g(), s * r.g()) is not, because the
// order in which the arguments are evaluated is unspecified (GCC draws the second first, clang the first), so the real and the imaginary part
// were swapped between Linux and macOS and the marginal tests saw another noise. The order here is GCC's, in which the sensitivity tests were
// tuned on the CI runners: the imaginary part is drawn first.
template <class R> inline cf32 cg(R& r, double sre, double sim) { const float im = (float)(sim * (double)r.g()); const float re = (float)(sre * (double)r.g()); return cf32(re, im); }

// Complex Gaussian noise with the given power per sample (added in place)
inline void addNoise(std::vector<cf32>& x, double power, unsigned seed) {
    Rng r(seed); const float s = (float)std::sqrt(power / 2);
    for (auto& v : x) v += cg(r, s, s);
}
// Carrier offset with a linear drift (Hz and Hz per second)
inline void mix(std::vector<cf32>& x, double hz, double driftHzPerSec, double fs) {
    double p = 0, f = hz; const double df = driftHzPerSec / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(p), (float)std::sin(p)); p += 2 * kPi * f / fs; f += df; }
}

// RC PPM: amplitude of the carrier over time. notchUs: the pulse; invert: the carrier is high only during the pulse. clockPpm scales every time.
struct Ppm { std::vector<double> chMs{1.5, 1.5, 1.5, 1.5}; double frameMs = 20.0, notchUs = 350, jitterUs = 0, clockPpm = 0, depth = 1.0; bool invert = false; };
inline std::vector<cf32> ppm(const Ppm& p, double fs, double seconds, unsigned seed = 1) {
    Rng r(seed); std::vector<cf32> x((size_t)(fs * seconds), cf32(1, 0));
    const double k = 1 + p.clockPpm * 1e-6;
    double t = 0.0013;
    auto pulse = [&](double t0) {
        const size_t a = (size_t)(t0 * k * fs), b = (size_t)((t0 + p.notchUs * 1e-6) * k * fs);
        for (size_t i = a; i < b && i < x.size(); i++) x[i] = cf32((float)(1 - p.depth), 0);
    };
    if (p.invert) for (auto& v : x) v = cf32((float)(1 - p.depth), 0);
    auto setPulse = [&](double t0) {
        const size_t a = (size_t)(t0 * k * fs), b = (size_t)((t0 + p.notchUs * 1e-6) * k * fs);
        for (size_t i = a; i < b && i < x.size(); i++) x[i] = p.invert ? cf32(1, 0) : cf32((float)(1 - p.depth), 0);
    };
    (void)pulse;
    while (t * k < seconds) {
        double fr = 0;
        for (double c : p.chMs) { setPulse(t + fr * 1e-3); fr += c + p.jitterUs * 1e-3 * r.g(); }
        setPulse(t + fr * 1e-3);
        t += p.frameMs * 1e-3;
    }
    return x;
}

// UVB-76: AM carrier; the buzz is a 30 Hz harsh tone burst (sawtooth), buzzLen long, every period. Optional breaks (no buzz) and a long transmission.
struct Buzz { double period = 2.4, buzzLen = 1.2, depth = 0.6, ppm = 0; double breakAt = -1, breakLen = 0, voiceAt = -1, voiceLen = 0; };
inline std::vector<cf32> buzzer(const Buzz& b, double fs, double seconds, unsigned seed = 1) {
    Rng r(seed); std::vector<cf32> x((size_t)(fs * seconds));
    const double per = b.period * (1 + b.ppm * 1e-6);
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs; double a = 0;
        const bool inBreak = b.breakAt >= 0 && t >= b.breakAt && t < b.breakAt + b.breakLen;
        const bool voice = b.voiceAt >= 0 && t >= b.voiceAt && t < b.voiceAt + b.voiceLen;
        if (voice) a = 0.5 * std::sin(2 * kPi * 300 * t) * (0.6 + 0.4 * std::sin(2 * kPi * 3.1 * t)) + 0.3 * std::sin(2 * kPi * 1100 * t) * std::sin(2 * kPi * 2.3 * t);
        else if (!inBreak && std::fmod(t, per) < b.buzzLen) { const double ph = std::fmod(t * 30.0, 1.0); a = (ph * 2 - 1) * 0.7; }
        x[i] = cf32((float)(1 + b.depth * a), 0);
    }
    return x;
}

// GRAVES: carrier at carrierHz, meteor echoes as short tones at carrierHz + dop with amplitude amp (relative to the carrier of 1)
struct Echo { double start, dur, dop, amp, driftHzPerSec = 0; };
inline std::vector<cf32> graves(double fs, double seconds, double carrierHz, double carrierAmp, const std::vector<Echo>& e, unsigned seed = 1) {
    std::vector<cf32> x((size_t)(fs * seconds)); double pc = 0;
    std::vector<double> pe(e.size(), 0.0);
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs;
        cf32 v = (float)carrierAmp * cf32((float)std::cos(pc), (float)std::sin(pc)); pc += 2 * kPi * carrierHz / fs;
        for (size_t k = 0; k < e.size(); k++) if (t >= e[k].start && t < e[k].start + e[k].dur) {
            const double f = carrierHz + e[k].dop + e[k].driftHzPerSec * (t - e[k].start);
            pe[k] += 2 * kPi * f / fs;
            const double env = std::sin(kPi * (t - e[k].start) / e[k].dur);   // smooth start and end
            v += (float)(e[k].amp * env) * cf32((float)std::cos(pe[k]), (float)std::sin(pe[k]));
        }
        x[i] = v;
    }
    (void)seed;
    return x;
}

// Radio Jove: noise of power 1 is added by the caller; this makes bursts of extra noise power `extra` at the given times (start, length)
struct Burst { double start, len, extra; };
inline std::vector<cf32> jove(double fs, double seconds, const std::vector<Burst>& b, unsigned seed = 1) {
    Rng r(seed); std::vector<cf32> x((size_t)(fs * seconds));
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs; double pw = 1;
        for (auto& e : b) if (t >= e.start && t < e.start + e.len) pw += e.extra;
        const float s = (float)std::sqrt(pw / 2);
        x[i] = cg(r, s, s);
    }
    return x;
}

inline dect2::X40Telemetry run(int variant, const std::vector<cf32>& x, double fs, double offset = 0) {
    dect2::X40Receiver rx; rx.setVariant(variant); rx.configure(fs); rx.setSignalOffset(offset);
    for (size_t i = 0; i < x.size(); i += 4096) rx.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    dect2::X40Telemetry t; rx.telemetry(t, 0);
    return t;
}
inline int countKind(const dect2::X40Telemetry& t, const char* kind) { int n = 0; for (auto& r : t.rows) if (r.kind == kind) n++; return n; }
}
