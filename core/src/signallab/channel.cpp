// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 1: bursts, bandwidth, carrier offset, SNR, duty cycle.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::signallab {

namespace {
double pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}
}

std::vector<Burst> findBursts(const std::vector<cf>& x, double fs, double minGapSec, double minBurstSec, double* noiseOut) {
    std::vector<Burst> out;
    const size_t n = x.size();
    if (n < 16) return out;
    const size_t w = std::max<size_t>(8, (size_t)(fs * minBurstSec / 4));
    std::vector<double> p(n);
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += std::norm(x[i]);
        if (i >= w) acc -= std::norm(x[i - w]);
        p[i] = std::max(0.0, acc) / (double)std::min(i + 1, w);
    }
    // the smoothed power lags by w/2: centre it
    std::vector<double> pc(n);
    for (size_t i = 0; i < n; i++) pc[i] = p[std::min(n - 1, i + w / 2)];
    std::vector<double> sub;
    for (size_t i = 0; i < n; i += std::max<size_t>(1, n / 20000)) sub.push_back(pc[i]);
    const double noise = std::max(1e-30, pct(sub, 0.10)), sig = pct(sub, 0.995);
    if (noiseOut) *noiseOut = noise;
    if (sig < noise * 6) { out.push_back({0, n}); return out; }   // no on/off contrast: continuous (or nothing)
    const double hi = std::sqrt(noise * sig), lo = std::pow(noise, 0.6) * std::pow(sig, 0.4);
    bool on = false;
    size_t st = 0;
    for (size_t i = 0; i < n; i++) {
        if (!on && pc[i] > hi) { on = true; st = i; }
        else if (on && pc[i] < lo) { on = false; out.push_back({st, i}); }
    }
    if (on) out.push_back({st, n});
    // merge short gaps, drop short bursts
    const size_t minGap = (size_t)(minGapSec * fs), minLen = (size_t)(minBurstSec * fs);
    std::vector<Burst> m;
    for (const Burst& b : out) {
        if (!m.empty() && b.start - m.back().end < minGap) m.back().end = b.end;
        else m.push_back(b);
    }
    // frames separated by long silences, with short symbol-level gaps inside (OOK zeros): merge gaps well below the long ones
    if (m.size() >= 4) {
        std::vector<double> g;
        for (size_t i = 1; i < m.size(); i++) g.push_back((double)(m[i].start - m[i - 1].end));
        const double gl = pct(g, 0.85);
        std::vector<Burst> mm;
        for (const Burst& b : m) {
            if (!mm.empty() && (double)(b.start - mm.back().end) < gl / 4) mm.back().end = b.end;
            else mm.push_back(b);
        }
        m.swap(mm);
    }
    out.clear();
    for (const Burst& b : m) if (b.end - b.start >= std::max<size_t>(minLen, 8)) out.push_back(b);
    return out;
}

ChannelInfo measureChannel(const std::vector<cf>& x, double fs, const ChannelOptions& o) {
    ChannelInfo ci;
    if (x.size() < 64 || fs <= 0) return ci;
    ci.bursts = findBursts(x, fs, o.minGapSec, o.minBurstSec);
    size_t on = 0;
    for (const Burst& b : ci.bursts) on += b.end - b.start;
    ci.dutyCycle = (double)on / (double)x.size();
    ci.continuous = ci.bursts.size() == 1 && ci.dutyCycle > 0.9;
    ci.meanBurstSec = ci.bursts.empty() ? 0 : (double)on / (double)ci.bursts.size() / fs;
    if (ci.bursts.size() >= 3) {
        std::vector<double> d;
        for (size_t i = 1; i < ci.bursts.size(); i++) d.push_back((double)(ci.bursts[i].start - ci.bursts[i - 1].start));
        const double med = pct(d, 0.5);
        std::vector<double> dev;
        for (double v : d) dev.push_back(std::fabs(v - med));
        if (pct(dev, 0.5) < 0.05 * med) ci.burstPeriodSec = med / fs;
    }
    // spectrum of the active samples only, so silence does not dilute it
    std::vector<cf> act;
    if (ci.dutyCycle < 0.9) { for (const Burst& b : ci.bursts) act.insert(act.end(), x.begin() + (long)b.start, x.begin() + (long)b.end); }
    const std::vector<cf>& src = act.empty() ? x : act;
    const size_t nfft = std::min(o.nfft, nextPow2(src.size()) / 2 < 64 ? (size_t)64 : nextPow2(src.size()) / 2);
    std::vector<double> P = psd(src.data(), src.size(), nfft);
    const size_t N = P.size();
    std::vector<double> S(N);
    const int sm = std::max(1, (int)(N / 512));
    for (size_t i = 0; i < N; i++) {
        double a = 0; int c = 0;
        for (int k = -sm; k <= sm; k++) { const long j = (long)i + k; if (j >= 0 && j < (long)N) { a += P[j]; c++; } }
        S[i] = a / c;
    }
    const double floor = std::max(1e-30, pct(S, 0.15));
    ci.noiseFloor = floor;
    // the occupied region comes from a wider smoothing: a strong narrow line (a preamble tone) must not decide it, and a wide weak
    // signal (FSK with a large index) stands clear of the noise only when averaged over more bins
    std::vector<double> S2(N);
    {
        const int sm2 = std::max(2, (int)(N / 128));
        std::vector<double> cs(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) cs[i + 1] = cs[i] + P[i];
        for (size_t i = 0; i < N; i++) { const size_t a = i > (size_t)sm2 ? i - sm2 : 0, e = std::min(N, i + sm2 + 1); S2[i] = (cs[e] - cs[a]) / (double)(e - a); }
    }
    const double floor2 = std::max(1e-30, pct(S2, 0.15));
    size_t pk = (size_t)(std::max_element(S2.begin(), S2.end()) - S2.begin());
    // around the peak while above 1.6x the floor, crossing dips (between FSK tones)
    const double thr = floor2 * 1.6;
    const size_t dip = std::max<size_t>(2, N / 32);
    size_t lo = pk, hi = pk;
    for (size_t miss = 0; lo > 0 && miss <= dip; ) { lo--; if (S2[lo] > thr) miss = 0; else miss++; }
    while (lo < pk && S2[lo] <= thr) lo++;
    for (size_t miss = 0; hi + 1 < N && miss <= dip; ) { hi++; if (S2[hi] > thr) miss = 0; else miss++; }
    while (hi > pk && S2[hi] <= thr) hi--;
    pk = lo + (size_t)(std::max_element(S.begin() + (long)lo, S.begin() + (long)hi + 1) - (S.begin() + (long)lo));
    double tot = 0;
    for (size_t i = lo; i <= hi; i++) tot += std::max(0.0, P[i] - floor);
    const double binHz = fs / (double)N;
    if (tot <= 0) return ci;
    double c = 0;
    size_t a = lo, b = hi;
    for (size_t i = lo; i <= hi; i++) { c += std::max(0.0, P[i] - floor); if (c >= 0.005 * tot) { a = i; break; } }
    c = 0;
    for (size_t i = hi + 1; i-- > lo;) { c += std::max(0.0, P[i] - floor); if (c >= 0.005 * tot) { b = i; break; } }
    if (b < a) std::swap(a, b);
    ci.bw99Hz = (double)(b - a + 1) * binHz;
    ci.offsetHz = ((double)(a + b) / 2.0 - (double)N / 2.0) * binHz;
    size_t a3 = pk, b3 = pk;
    for (size_t i = lo; i <= hi; i++) if (S[i] >= S[pk] / 2) { a3 = std::min(a3, i); b3 = std::max(b3, i); }
    ci.bw3dBHz = (double)(b3 - a3 + 1) * binHz;
    ci.snrDb = 10 * std::log10(std::max(1e-12, tot / (floor * (double)(b - a + 1))));
    return ci;
}

} // namespace dect2::signallab
