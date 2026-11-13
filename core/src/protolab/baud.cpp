// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: baud rate estimate (transition intervals) and the bit slicer with a clock that follows the transitions.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>

namespace dect2::protolab {

namespace {
float pctile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    size_t k = (size_t)(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

struct Run { int level; double len; };

// quantised state per sample: 2-level channels at the mid-range, 4-level at the three thresholds, two channels as a 2-bit state
std::vector<int> quantise(const Soft& s, int levels) {
    const size_t n = s.a.size();
    std::vector<int> q(n);
    if (s.twoChannels()) {
        for (size_t i = 0; i < n; i++) q[i] = (s.a[i] > 0 ? 2 : 0) | (s.b[i] > 0 ? 1 : 0);
        return q;
    }
    const float lo = pctile(s.a, 0.05), hi = pctile(s.a, 0.95);
    if (levels == 4) {
        const float t0 = lo + (hi - lo) / 6, t1 = lo + (hi - lo) / 2, t2 = lo + (hi - lo) * 5 / 6;
        for (size_t i = 0; i < n; i++) q[i] = s.a[i] > t2 ? 3 : s.a[i] > t1 ? 2 : s.a[i] > t0 ? 1 : 0;
    } else {
        const float mid = 0.5f * (lo + hi);
        for (size_t i = 0; i < n; i++) q[i] = s.a[i] > mid ? 1 : 0;
    }
    return q;
}

std::vector<float> boxcar(const std::vector<float>& x, size_t w) {
    if (w <= 1 || x.empty()) return x;
    std::vector<float> y(x.size());
    double acc = 0;
    const size_t h = w / 2;
    // centred moving average with edge replication
    for (size_t i = 0; i < w; i++) acc += x[std::min(x.size() - 1, i > h ? i - h : 0)];
    for (size_t i = 0; i < x.size(); i++) {
        y[i] = (float)(acc / (double)w);
        const size_t add = std::min(x.size() - 1, i + w - h), sub = i >= h ? i - h : 0;
        acc += x[add] - x[sub];
    }
    return y;
}

std::vector<Run> runsOf(const std::vector<int>& q) {
    std::vector<Run> r;
    for (size_t i = 0; i < q.size(); i++) {
        if (!r.empty() && r.back().level == q[i]) r.back().len += 1;
        else r.push_back({q[i], 1});
    }
    return r;
}
}   // namespace

namespace {
BaudEstimate estimateWith(const std::vector<const Soft*>& bursts, double fs, size_t smoothW) {
    BaudEstimate est;
    std::vector<double> runs;
    std::vector<double> allIntervals;
    double ac = 0;
    for (const Soft* b : bursts) {
        if (!b || b->a.size() < 32) continue;
        Soft sm;
        sm.a = boxcar(b->a, smoothW);
        if (b->twoChannels()) sm.b = boxcar(b->b, smoothW);
        std::vector<int> q = quantise(sm, 2);   // 2-level view is enough: the symbol edges are in it
        if (b->twoChannels()) q = quantise(sm, 4);
        std::vector<Run> r = runsOf(q);
        for (size_t i = 1; i + 1 < r.size(); i++) if (r[i].len >= 1.5) runs.push_back(r[i].len);
    }
    if (runs.size() < 6) return est;
    std::vector<double> sorted = runs;
    std::sort(sorted.begin(), sorted.end());
    const double p10 = sorted[sorted.size() / 10];
    const double umax = std::max(3.0, p10 * 1.35);
    double bestU = 0, bestS = -1;
    std::vector<std::pair<double, double>> scan;
    for (double u = 2.0; u <= umax; u *= 1.004) {
        double s = 0;
        for (double r : runs) s += std::cos(2 * 3.14159265358979 * r / u);
        s /= (double)runs.size();
        scan.push_back({u, s});
        if (s > bestS) { bestS = s; bestU = u; }
    }
    // the largest unit that explains the runs nearly as well as the best (the best may be a fraction of the true symbol)
    double pick = bestU;
    for (auto& p : scan) if (p.second >= 0.93 * bestS && p.first > pick * 0.97 && p.second > 0.6) pick = std::max(pick, p.first);
    // among the neighbours of `pick`, the top of that peak
    double top = pick, topS = -2;
    for (auto& p : scan) if (std::abs(p.first - pick) < pick * 0.04 && p.second > topS) { topS = p.second; top = p.first; }
    // mean symbol length over the short runs (1 to 3 symbols: the whole number is certain there), each weighted by its symbols
    double num = 0, den = 0;
    for (double r : runs) {
        const double k = std::max(1.0, std::round(r / top));
        if (k <= 3) { num += r; den += k; }
    }
    if (den <= 0) return est;
    const double u = num / den;
    est.baud = fs / u;
    double sc = 0;
    for (double r : runs) sc += std::cos(2 * 3.14159265358979 * r / u);
    est.quality = std::clamp(sc / (double)runs.size(), 0.0, 1.0);

    // autocorrelation of the transition impulses of the first burst, a second opinion
    for (const Soft* b : bursts) {
        if (!b || b->a.size() < 256) continue;
        Soft sm;
        sm.a = boxcar(b->a, smoothW);
        std::vector<int> q = quantise(sm, 2);
        const size_t n = std::min<size_t>(q.size(), 20000);
        std::vector<float> t(n, 0.f);
        for (size_t i = 1; i < n; i++) if (q[i] != q[i - 1]) t[i] = 1.f;
        const size_t maxL = std::min<size_t>(n / 2, (size_t)(umax * 12));
        std::vector<double> a(maxL, 0.0);
        double mx = 0;
        for (size_t L = 2; L < maxL; L++) {
            double s = 0;
            for (size_t i = 0; i + L < n; i++) s += t[i] * t[i + L];
            a[L] = s;
            mx = std::max(mx, s);
        }
        for (size_t L = 3; L + 1 < maxL; L++)
            if (a[L] >= 0.6 * mx && a[L] >= a[L - 1] && a[L] >= a[L + 1]) { ac = fs / (double)L; break; }
        break;
    }
    est.fromAutocorr = ac;
    est.smoothing = (int)smoothW;
    return est;
}
}   // namespace

// The frequency or envelope of a noisy burst chatters around the slicing level, so the transition intervals are measured on several
// smoothing widths and the one whose intervals are the cleanest multiples of one symbol wins.
BaudEstimate estimateBaud(const std::vector<const Soft*>& bursts, double fs, int levels) {
    BaudEstimate best;
    for (size_t w : {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48}) {
        BaudEstimate e = estimateWith(bursts, fs, w);
        // the smoothing must stay well inside one symbol, or it makes up its own pattern
        if (e.baud > 0 && e.baud < fs / 1.6 && (double)w <= std::max(1.0, 0.4 * fs / e.baud) && e.quality > best.quality + 0.01) best = e;
    }
    if (best.baud <= 0) return best;
    // Refinement: the edges of a burst lie on a line, time = start + symbol index * length. Counting the symbols between the edges with the
    // coarse length and fitting that line over every edge of every burst gives the length much more precisely, because the long
    // baseline averages out the edge-to-edge timing noise (and, with four levels, the different delays of the different steps).
    std::vector<Soft> sm;
    for (const Soft* b : bursts) {
        if (!b || b->a.size() < 64) continue;
        Soft x;
        x.a = boxcar(b->a, (size_t)best.smoothing);
        if (b->twoChannels()) x.b = boxcar(b->b, (size_t)best.smoothing);
        sm.push_back(std::move(x));
    }
    double u = fs / best.baud;
    for (int iter = 0; iter < 4; iter++) {
        double sxy = 0, sxx = 0;
        for (const Soft& x : sm) {
            std::vector<Run> r = runsOf(quantise(x, x.twoChannels() ? 4 : levels));
            std::vector<double> t, n;   // edge time in samples, symbols since the first edge
            // the edges are the starts of the runs that are not slivers
            double pos = 0, cnt = 0, lastEdge = 0;
            bool have = false;
            for (size_t i = 0; i < r.size(); i++) {
                const bool sliver = r[i].len < 0.5 * u && i > 0 && i + 1 < r.size();
                if (!sliver && i > 0) {
                    if (have) cnt += std::max(1.0, std::round((pos - lastEdge) / u));
                    t.push_back(pos);
                    n.push_back(cnt);
                    lastEdge = pos;
                    have = true;
                }
                pos += r[i].len;
            }
            if (t.size() < 4) continue;
            double mt = 0, mn = 0;
            for (size_t j = 0; j < t.size(); j++) { mt += t[j]; mn += n[j]; }
            mt /= (double)t.size();
            mn /= (double)t.size();
            for (size_t j = 0; j < t.size(); j++) { sxy += (n[j] - mn) * (t[j] - mt); sxx += (n[j] - mn) * (n[j] - mn); }
        }
        if (sxx <= 0) break;
        const double nu = sxy / sxx;
        if (nu < 0.85 * u || nu > 1.15 * u) break;   // not a refinement: leave it
        u = nu;
    }
    best.baud = fs / u;
    return best;
}

std::vector<uint8_t> sliceBits(const Soft& s, double fs, double baud, int levels, bool invert) {
    std::vector<uint8_t> bits;
    if (s.a.size() < 8 || baud <= 0 || fs <= 0) return bits;
    double spb = fs / baud;
    if (spb < 1.5) return bits;
    Soft f;
    const size_t w = (size_t)std::max(1.0, std::round(spb * 0.5));
    f.a = boxcar(s.a, w);
    if (s.twoChannels()) f.b = boxcar(s.b, w);
    std::vector<int> q = quantise(f, levels);
    std::vector<Run> runs = runsOf(q);
    // absorb glitches shorter than 0.4 symbol into the run before them
    std::vector<Run> r;
    for (const Run& x : runs) {
        if (!r.empty() && (x.len < 0.4 * spb || r.back().level == x.level)) { r.back().len += x.len; continue; }
        r.push_back(x);
    }
    std::vector<Run> m;   // after absorbing, two neighbours can hold the same level: merge again
    for (const Run& x : r) {
        if (!m.empty() && m.back().level == x.level) m.back().len += x.len;
        else m.push_back(x);
    }
    const bool two = s.twoChannels();
    const int top = two ? 3 : levels - 1;
    static const int gray[4] = {0, 1, 3, 2};
    auto emit = [&](int level, int count) {
        if (invert) level = top - level;
        for (int i = 0; i < count; i++) {
            if (two) { bits.push_back((uint8_t)((level >> 1) & 1)); bits.push_back((uint8_t)(level & 1)); }
            else if (levels == 4) { const int g = gray[level & 3]; bits.push_back((uint8_t)((g >> 1) & 1)); bits.push_back((uint8_t)(g & 1)); }
            else bits.push_back((uint8_t)(level & 1));
        }
    };
    // run lengths -> symbol counts; the symbol length is nudged by long clean runs
    for (size_t i = 0; i < m.size(); i++) {
        int k = std::max(1, (int)std::lround(m[i].len / spb));
        if (i + 1 == m.size()) k = std::min(k, 16);
        if (i > 0 && i + 1 < m.size() && k >= 2 && k <= 8) spb += 0.05 * (m[i].len / (double)k - spb);
        emit(m[i].level, k);
    }
    return bits;
}

}   // namespace dect2::protolab
