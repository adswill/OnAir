// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/transponders.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dect2 {

double snapRollOff(double a) {
    if (!(a > 0) || a > 0.7) return 0;
    static const double k[] = {0.05, 0.10, 0.15, 0.20, 0.25, 0.35};
    double best = k[0];
    for (double v : k) if (std::fabs(v - a) < std::fabs(best - a)) best = v;
    return best;
}

namespace {
inline double toDb(double v) { return 10.0 * std::log10(std::max(v, 1e-30)); }
struct Run { int s, e; };
}

std::vector<Transponder> detectTransponders(const std::vector<float>& psdDb, double rate, const TransponderParams& p) {
    std::vector<Transponder> out;
    const int N = (int)psdDb.size();
    if (N < 256 || rate <= 0) return out;
    const double bin = rate / N;
    const int lo = std::max(0, (int)std::lround(N / 2 - p.edgeFrac * N)), hi = std::min(N, (int)std::lround(N / 2 + p.edgeFrac * N));
    if (hi - lo < 64) return out;
    std::vector<double> lin((size_t)N);
    for (int k = 0; k < N; k++) lin[(size_t)k] = std::pow(10.0, psdDb[(size_t)k] / 10.0);

    // the SDR's DC spike: a few bins in the middle far above the bins around them are replaced by the median of those
    {
        const int c = N / 2, w = std::max(2, (int)std::lround(15e3 / bin)), side = std::max(w + 3, 3 * w);
        if (c - side > 0 && c + side < N) {
            std::vector<double> nb;
            for (int k = c - side; k < c - w; k++) nb.push_back(lin[(size_t)k]);
            for (int k = c + w + 1; k <= c + side; k++) nb.push_back(lin[(size_t)k]);
            std::nth_element(nb.begin(), nb.begin() + (long)nb.size() / 2, nb.end());
            const double med = nb[nb.size() / 2];
            double pk = 0;
            for (int k = c - w; k <= c + w; k++) pk = std::max(pk, lin[(size_t)k]);
            if (pk > 2.0 * med) for (int k = c - w; k <= c + w; k++) lin[(size_t)k] = med;   // 3 dB and more above the neighbours: a spike (or the flat top of a carrier on DC: the median is its level too)
        }
    }

    // smoothing in linear power: a box of about a sixteenth of the narrowest carrier
    const int ws = std::max(1, std::min(N / 64, (int)std::lround(p.minRsHz / bin / 16.0))) | 1;
    std::vector<double> sm((size_t)N, 0.0);
    {
        std::vector<double> cum((size_t)N + 1, 0.0);
        for (int k = 0; k < N; k++) cum[(size_t)k + 1] = cum[(size_t)k] + lin[(size_t)k];
        for (int k = 0; k < N; k++) {
            const int a = std::max(0, k - ws / 2), b = std::min(N, k + ws / 2 + 1);
            sm[(size_t)k] = (cum[(size_t)b] - cum[(size_t)a]) / (b - a);
        }
    }
    std::vector<double> smDb((size_t)N);
    for (int k = 0; k < N; k++) smDb[(size_t)k] = toDb(sm[(size_t)k]);

    // the floor: the 10th percentile of each segment, a line fitted through them, segments well above the line (signals) dropped one by one
    const int seg = std::max(16, (hi - lo) / 32), M = std::max(1, (hi - lo) / seg);
    std::vector<double> xs, ys;
    for (int s = 0; s < M; s++) {
        const int a = lo + s * seg, b = s == M - 1 ? hi : a + seg;
        std::vector<double> v(smDb.begin() + a, smDb.begin() + b);
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 10), v.end());
        xs.push_back(0.5 * (a + b)); ys.push_back(v[v.size() / 10]);
    }
    std::vector<bool> act(xs.size(), true);
    double fa = 0, fb = 0;
    for (;;) {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < xs.size(); i++) if (act[i]) { n++; sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; }
        const double den = n * sxx - sx * sx;
        fb = n >= 3 && std::fabs(den) > 1e-9 ? (n * sxy - sx * sy) / den : 0;
        const double maxSlope = 20.0 / (hi - lo);   // the floor does not tilt by more than 20 dB across the span
        fb = std::max(-maxSlope, std::min(maxSlope, fb));
        fa = n > 0 ? (sy - fb * sx) / n : -200;
        size_t worst = SIZE_MAX;
        double wr = 1.5;
        for (size_t i = 0; i < xs.size(); i++) if (act[i]) { const double r = ys[i] - (fa + fb * xs[i]); if (r > wr) { wr = r; worst = i; } }
        if (worst == SIZE_MAX || n <= std::max(3.0, (double)xs.size() / 6)) break;
        act[worst] = false;
    }
    auto floorDb = [&](int k) { return fa + fb * k; };

    // runs above floor + threshold, ending 2 dB lower
    std::vector<Run> runs;
    {
        bool in = false;
        int s = 0;
        for (int k = lo; k < hi; k++) {
            const double above = smDb[(size_t)k] - floorDb(k);
            if (!in && above >= p.thresholdDb) { in = true; s = k; }
            else if (in && above < p.thresholdDb - 2) { in = false; runs.push_back({s, k}); }
        }
        if (in) runs.push_back({s, hi});
    }
    // a run is cut at every valley deeper than splitDb (adjacent transponders); valleys are judged on a heavier smoothing so the ripple of a flat top is no valley
    std::vector<Run> parts;
    {
        const int w2 = std::max(ws, (int)std::lround(p.minRsHz / bin / 6.0)) | 1;
        std::vector<double> cum((size_t)N + 1, 0.0);
        for (int k = 0; k < N; k++) cum[(size_t)k + 1] = cum[(size_t)k] + lin[(size_t)k];
        auto hv = [&](int k) { const int a = std::max(0, k - w2 / 2), b = std::min(N, k + w2 / 2 + 1); return toDb((cum[(size_t)b] - cum[(size_t)a]) / (b - a)); };
        for (const Run& r : runs) {
            int s = r.s;
            double runMax = -1e9, minV = 1e9;
            int minK = r.s;
            for (int k = r.s; k < r.e; k++) {
                const double v = hv(k);
                if (v > runMax) { runMax = v; minV = v; minK = k; }
                else if (v < minV) { minV = v; minK = k; }
                else if (v > minV + p.splitDb && runMax - minV >= p.splitDb && minK - s > w2) {   // came back up out of a valley: cut at its bottom
                    parts.push_back({s, minK});
                    s = minK; runMax = v; minV = v; minK = k;
                }
            }
            parts.push_back({s, r.e});
        }
    }
    for (const Run& r : parts) {
        if (r.e - r.s < 4) continue;
        Transponder t;
        t.clipped = r.s <= lo + 1 || r.e >= hi - 1;
        const int len = r.e - r.s, q = len / 4;
        std::vector<double> v;
        for (int k = r.s + q; k < r.e - q; k++) v.push_back(smDb[(size_t)k]);
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
        t.levelDb = v[v.size() / 2];
        const int mid = (r.s + r.e) / 2;
        t.snrDb = t.levelDb - floorDb(mid);
        if (t.snrDb < p.thresholdDb) continue;
        // flat top: most of the middle half lies within 3 dB of the plateau (a noise hump or a lone peak is not a transponder)
        int flat = 0;
        for (double x : v) if (std::fabs(x - t.levelDb) <= 3.0) flat++;
        if ((double)flat / (double)v.size() < p.minFlatness) continue;
        const double P = std::pow(10.0, t.levelDb / 10) - std::pow(10.0, floorDb(mid) / 10);
        if (P <= 0) continue;
        auto sig = [&](int k) { return k < 0 || k >= N ? 0.0 : sm[(size_t)k] - std::pow(10.0, floorDb(k) / 10); };
        const int a0 = std::max(0, r.s - std::max(4, 2 * ws)), b0 = std::min(N - 1, r.e + std::max(4, 2 * ws));
        auto edgeL = [&](double frac) {   // walking in from outside the part, linear interpolation between bins
            const double th = frac * P;
            for (int k = a0; k <= mid; k++) { const double v1 = sig(k); if (v1 >= th) { const double v0 = sig(k - 1); return v1 > v0 ? (k - 1) + (th - v0) / (v1 - v0) : (double)k; } }
            return (double)mid;
        };
        auto edgeR = [&](double frac) {
            const double th = frac * P;
            for (int k = b0; k >= mid; k--) { const double v1 = sig(k); if (v1 >= th) { const double v0 = sig(k + 1); return v1 > v0 ? (k + 1) - (th - v0) / (v1 - v0) : (double)k; } }
            return (double)mid;
        };
        // the neighbours' skirts must not pull the edge of a split part outward: clamp at the part's own limits
        const double l3 = std::max(edgeL(0.5), (double)r.s - 0.5 * ws), r3 = std::min(edgeR(0.5), (double)r.e + 0.5 * ws);
        t.rsHz = (r3 - l3) * bin;
        t.centerHz = (0.5 * (l3 + r3) - N / 2) * bin;
        const double l9 = edgeL(0.9), r9 = edgeR(0.9), l1 = std::max(edgeL(0.1), (double)r.s - ws), r1 = std::min(edgeR(0.1), (double)r.e + ws);
        // a raised cosine shoulder from 90 % to 10 % of the power is 0.5904 alpha Rs wide; the smoothing widens it by about its box
        const double shoulder = std::max(0.0, 0.5 * ((l9 - l1) + (r1 - r9)) * bin - 0.5 * ws * bin);
        t.rollOff = t.snrDb >= 10 && t.rsHz > 0 ? snapRollOff(shoulder / (0.5904 * t.rsHz)) : 0;
        t.bwHz = t.rollOff > 0 ? t.rsHz * (1 + t.rollOff) : (r1 - l1) * bin;
        if (t.rsHz < p.minRsHz || t.rsHz > p.maxRsHz) continue;
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const Transponder& a, const Transponder& b) { return a.centerHz < b.centerHz; });
    return out;
}

} // namespace dect2
