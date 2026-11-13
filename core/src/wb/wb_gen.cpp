// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_gen.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace wb {

namespace {
using dect2::math::kPi;

std::vector<float> rrcTaps(double alpha, double sps, int spanSyms) {
    const int n = (int)(spanSyms * sps) | 1;
    std::vector<float> h((size_t)n);
    const double c = (n - 1) / 2.0;
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double t = (i - c) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / kPi;
        else if (alpha > 0 && std::fabs(std::fabs(t) - 1 / (4 * alpha)) < 1e-6)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
        else {
            const double num = std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha));
            const double den = kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t));
            v = num / den;
        }
        h[(size_t)i] = (float)v;
        e += v * v;
    }
    for (float& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

}  // namespace

void addNoise(std::vector<cf32>& x, double power, uint32_t seed) {
    std::mt19937 rng(seed * 7919u + 13u);
    dect2::normal_distribution<float> nd(0.f, (float)std::sqrt(power / 2));
    for (cf32& v : x) v += cf32{nd(rng), nd(rng)};
}

GenResult generate(const GenSpec& g) {
    GenResult r;
    r.rate = g.rate;
    const double sps = g.rate / g.baud;
    std::mt19937 rng(g.seed);
    int bps = 1;
    switch (g.kind) { case KQpsk: case KFsk4: bps = 2; break; case KPsk8: bps = 3; break; default: break; }
    size_t nb = (g.nbits / (size_t)bps) * (size_t)bps;
    r.bits.resize(nb);
    for (auto& b : r.bits) b = (uint8_t)(rng() & 1);
    const size_t nsym = nb / (size_t)bps;
    const size_t N = (size_t)std::ceil((double)nsym * sps) + 1;
    std::vector<cf32> x;
    auto symAt = [&](size_t n) { return std::min(nsym ? nsym - 1 : 0, (size_t)((double)n / sps)); };
    const double w0 = 2 * kPi / g.rate;
    switch (g.kind) {
    case KFsk2: case KFsk4: case KMsk: case KGmsk: {
        std::vector<float> lev(N);   // the frequency waveform in units of the deviation (+-1 for 2-level)
        for (size_t n = 0; n < N; n++) {
            const size_t s = symAt(n);
            float v;
            if (g.kind == KFsk4) { const int b1 = r.bits[2 * s], b0 = r.bits[2 * s + 1]; v = (b1 == 0 ? (b0 ? 3.f : 1.f) : (b0 ? -3.f : -1.f)) / 3.f; }   // 01 +3, 00 +1, 10 -1, 11 -3
            else v = r.bits[s] ? 1.f : -1.f;
            lev[n] = v;
        }
        double dev = g.devHz;
        if (dev <= 0) dev = g.kind == KMsk || g.kind == KGmsk ? g.baud / 4 : g.kind == KFsk4 ? 0.75 * g.baud : 0.6 * g.baud;
        if (g.kind == KGmsk) {   // Gaussian filter on the frequency pulses
            const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * g.gauss) * sps;
            const int hl = (int)(3 * sigma) + 1;
            std::vector<float> gt((size_t)(2 * hl + 1));
            double s = 0;
            for (int i = -hl; i <= hl; i++) { const double v = std::exp(-0.5 * i * i / (sigma * sigma)); gt[(size_t)(i + hl)] = (float)v; s += v; }
            std::vector<float> f(N, 0.f);
            for (size_t n = 0; n < N; n++) {
                double a = 0;
                for (int i = -hl; i <= hl; i++) { const long m = (long)n + i; const float v = lev[(size_t)std::min<long>(std::max<long>(m, 0), (long)N - 1)]; a += gt[(size_t)(i + hl)] * v; }
                f[n] = (float)(a / s);
            }
            lev.swap(f);
        }
        x.resize(N);
        double ph = 0;
        for (size_t n = 0; n < N; n++) { x[n] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w0 * dev * lev[n]; }
        break;
    }
    case KAfsk: {
        std::vector<float> a(N);
        double ph = 0;
        for (size_t n = 0; n < N; n++) { a[n] = (float)(0.5 * std::cos(ph)); ph += w0 * (r.bits[symAt(n)] ? g.markHz : g.spaceHz); }
        x.resize(N);
        if (g.afskOnFm) {
            const double dev = g.devHz > 0 ? g.devHz : 3000;
            double p2 = 0;
            for (size_t n = 0; n < N; n++) { x[n] = cf32((float)std::cos(p2), (float)std::sin(p2)); p2 += w0 * dev * (2.0 * a[n]); }
        } else for (size_t n = 0; n < N; n++) x[n] = cf32(a[n], 0);
        break;
    }
    case KAsk: case KCw: {
        std::vector<float> a(N);
        for (size_t n = 0; n < N; n++) a[n] = r.bits[symAt(n)] ? 1.f : 0.05f;
        const int L = std::max(1, (int)(sps / 4));
        x.resize(N);
        double acc = 0;
        for (size_t n = 0; n < N + (size_t)L; n++) {
            if (n < N) acc += a[n];
            if (n >= (size_t)L) acc -= a[n - (size_t)L];
            if (n >= (size_t)L - 1 && n - ((size_t)L - 1) < N) x[n - ((size_t)L - 1)] = cf32((float)(acc / L), 0);
        }
        break;
    }
    case KBpsk: case KQpsk: case KPsk8: {
        const int M = g.kind == KBpsk ? 2 : g.kind == KQpsk ? 4 : 8;
        std::vector<cf32> sy(nsym);
        int prev = 0;
        for (size_t s = 0; s < nsym; s++) {
            int v = 0;
            for (int k = 0; k < bps; k++) v = v << 1 | r.bits[s * (size_t)bps + (size_t)k];
            int idx;
            if (M == 2) idx = v ? 0 : 1;
            else if (M == 4) { static const int fromGray[4] = {0, 3, 1, 2}; idx = fromGray[v]; }   // 00 -> 0, 01 -> 3, 10 -> 1, 11 -> 2
            else { int sec = 0; for (int t = v; t; t >>= 1) sec ^= t; idx = sec; }                  // inverse Gray
            if (g.differential) { idx = (prev + idx) % M; prev = idx; }
            const double ph = M == 2 ? idx * kPi : M == 4 ? idx * kPi / 2 + kPi / 4 : idx * kPi / 4;
            sy[s] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        const std::vector<float> h = rrcTaps(std::max(0.05, g.rolloff), sps, 10);
        const size_t T = h.size();
        x.assign(N + T, cf32(0, 0));
        for (size_t s = 0; s < nsym; s++) {
            const size_t pos = (size_t)std::lround((double)s * sps);
            for (size_t k = 0; k < T; k++) x[pos + k] += sy[s] * h[k];
        }
        x.erase(x.begin(), x.begin() + (long)(T / 2));
        x.resize(N);
        break;
    }
    case KAm: case KFm: case KUsb: case KLsb: default: {
        x.resize(N);
        double ph = 0;
        for (size_t n = 0; n < N; n++) {
            const double t = (double)n / g.rate;
            if (g.kind == KAm) x[n] = cf32((float)(0.5 * (1 + g.depth * std::cos(2 * kPi * g.toneHz * t))), 0);
            else if (g.kind == KFm) { x[n] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w0 * (g.devHz > 0 ? g.devHz : 3000) * std::cos(2 * kPi * g.toneHz * t); }
            else { const double f = g.kind == KUsb ? g.toneHz : -g.toneHz; x[n] = cf32((float)std::cos(2 * kPi * f * t), (float)std::sin(2 * kPi * f * t)); }
        }
        break;
    }
    }
    // power 1 (what is actually sent)
    double pw = 0;
    for (const cf32& v : x) pw += (double)std::norm(v);
    pw /= std::max<size_t>(1, x.size());
    const float sc = (float)(1.0 / std::sqrt(std::max(pw, 1e-12)) * (g.kind == KAfsk && !g.afskOnFm ? 0.5 : 0.7));
    for (cf32& v : x) v *= sc;
    pw *= (double)sc * sc;
    if (g.offsetHz != 0 && !(g.kind == KAfsk && !g.afskOnFm)) for (size_t n = 0; n < x.size(); n++) x[n] *= cf32((float)std::cos(w0 * g.offsetHz * (double)n), (float)std::sin(w0 * g.offsetHz * (double)n));
    // lead and tail
    const size_t lead = (size_t)(g.leadSec * g.rate), tail = (size_t)(g.tailSec * g.rate);
    std::vector<cf32> all(lead + x.size() + tail, cf32(0, 0));
    std::copy(x.begin(), x.end(), all.begin() + (long)lead);
    r.signalStart = lead; r.signalEnd = lead + x.size();
    const double np = pw / std::pow(10.0, g.snrDb / 10.0);
    if (g.snrDb < 200) addNoise(all, np, g.seed);
    r.real = g.realOut || (g.kind == KAfsk && !g.afskOnFm);
    if (r.real) for (cf32& v : all) v = cf32(v.real(), 0);
    r.x = std::move(all);
    return r;
}

} // namespace wb
} // namespace dect2
