// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Local display / HDMI emission: pixel clock harmonics and the line-rate comb, matched against the CEA-861 and VESA timings
// (dect2/rfi/display_emission.h, docs/HDMI-EMISSION-3.9.md).
#include "dect2/rfi/display_emission.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
using dect2::math::kPi;

namespace dect2 {
namespace dispem {

const char* label() { return "Local display / HDMI emission, not your SDR"; }

// Pixel clock, total pixels per line and lines per frame of each format. CEA-861 (VIC numbers) and VESA DMT / CVT reduced blanking;
// the test checks that every row's refresh rate follows from its clock and totals.
const std::vector<Timing>& timings() {
    static const std::vector<Timing> t = {
        {"640x480 60 Hz", "CEA-861 VIC 1 / VESA DMT", 640, 480, 59.94, false, 25.175e6, 800, 525},
        {"720x480 60 Hz (480p)", "CEA-861 VIC 2", 720, 480, 59.94, false, 27.0e6, 858, 525},
        {"720x576 50 Hz (576p)", "CEA-861 VIC 17", 720, 576, 50, false, 27.0e6, 864, 625},
        {"1280x720 60 Hz", "CEA-861 VIC 4", 1280, 720, 60, false, 74.25e6, 1650, 750},
        {"1280x720 59.94 Hz", "CEA-861 VIC 4 (1/1.001)", 1280, 720, 59.94, false, 74.25e6 / 1.001, 1650, 750},
        {"1280x720 50 Hz", "CEA-861 VIC 19", 1280, 720, 50, false, 74.25e6, 1980, 750},
        {"1920x1080 60 Hz interlaced", "CEA-861 VIC 5", 1920, 1080, 60, true, 74.25e6, 2200, 1125},
        {"1920x1080 50 Hz interlaced", "CEA-861 VIC 20", 1920, 1080, 50, true, 74.25e6, 2640, 1125},
        {"1920x1080 30 Hz", "CEA-861 VIC 34", 1920, 1080, 30, false, 74.25e6, 2200, 1125},
        {"1920x1080 25 Hz", "CEA-861 VIC 33", 1920, 1080, 25, false, 74.25e6, 2640, 1125},
        {"1920x1080 24 Hz", "CEA-861 VIC 32", 1920, 1080, 24, false, 74.25e6, 2750, 1125},
        {"1920x1080 60 Hz", "CEA-861 VIC 16", 1920, 1080, 60, false, 148.5e6, 2200, 1125},
        {"1920x1080 59.94 Hz", "CEA-861 VIC 16 (1/1.001)", 1920, 1080, 59.94, false, 148.5e6 / 1.001, 2200, 1125},
        {"1920x1080 50 Hz", "CEA-861 VIC 31", 1920, 1080, 50, false, 148.5e6, 2640, 1125},
        {"1920x1080 120 Hz", "CEA-861 VIC 63", 1920, 1080, 120, false, 297.0e6, 2200, 1125},
        {"1920x1080 100 Hz", "CEA-861 VIC 64", 1920, 1080, 100, false, 297.0e6, 2640, 1125},
        {"3840x2160 30 Hz", "CEA-861 VIC 95", 3840, 2160, 30, false, 297.0e6, 4400, 2250},
        {"3840x2160 29.97 Hz", "CEA-861 VIC 95 (1/1.001)", 3840, 2160, 29.97, false, 297.0e6 / 1.001, 4400, 2250},
        {"3840x2160 25 Hz", "CEA-861 VIC 94", 3840, 2160, 25, false, 297.0e6, 5280, 2250},
        {"3840x2160 24 Hz", "CEA-861 VIC 93", 3840, 2160, 24, false, 297.0e6, 5500, 2250},
        {"3840x2160 60 Hz", "CEA-861 VIC 97", 3840, 2160, 60, false, 594.0e6, 4400, 2250},
        {"3840x2160 59.94 Hz", "CEA-861 VIC 97 (1/1.001)", 3840, 2160, 59.94, false, 594.0e6 / 1.001, 4400, 2250},
        {"3840x2160 50 Hz", "CEA-861 VIC 96", 3840, 2160, 50, false, 594.0e6, 5280, 2250},
        {"640x480 72 Hz", "VESA DMT", 640, 480, 72.81, false, 31.5e6, 832, 520},
        {"640x480 75 Hz", "VESA DMT", 640, 480, 75, false, 31.5e6, 840, 500},
        {"800x600 60 Hz", "VESA DMT", 800, 600, 60.32, false, 40.0e6, 1056, 628},
        {"800x600 72 Hz", "VESA DMT", 800, 600, 72.19, false, 50.0e6, 1040, 666},
        {"800x600 75 Hz", "VESA DMT", 800, 600, 75, false, 49.5e6, 1056, 625},
        {"1024x768 60 Hz", "VESA DMT", 1024, 768, 60.0, false, 65.0e6, 1344, 806},
        {"1024x768 70 Hz", "VESA DMT", 1024, 768, 70.07, false, 75.0e6, 1328, 806},
        {"1024x768 75 Hz", "VESA DMT", 1024, 768, 75.03, false, 78.75e6, 1312, 800},
        {"1152x864 75 Hz", "VESA DMT", 1152, 864, 75, false, 108.0e6, 1600, 900},
        {"1280x800 60 Hz (reduced blanking)", "CVT-RB", 1280, 800, 59.91, false, 71.0e6, 1440, 823},
        {"1280x800 60 Hz", "CVT", 1280, 800, 59.81, false, 83.5e6, 1680, 831},
        {"1280x960 60 Hz", "VESA DMT", 1280, 960, 60, false, 108.0e6, 1800, 1000},
        {"1280x1024 60 Hz", "VESA DMT", 1280, 1024, 60.02, false, 108.0e6, 1688, 1066},
        {"1280x1024 75 Hz", "VESA DMT", 1280, 1024, 75.02, false, 135.0e6, 1688, 1066},
        {"1366x768 60 Hz", "VESA DMT", 1366, 768, 59.79, false, 85.5e6, 1792, 798},
        {"1440x900 60 Hz (reduced blanking)", "CVT-RB", 1440, 900, 59.90, false, 88.75e6, 1600, 926},
        {"1440x900 60 Hz", "CVT", 1440, 900, 59.89, false, 106.5e6, 1904, 934},
        {"1600x900 60 Hz (reduced blanking)", "VESA DMT (RB)", 1600, 900, 60, false, 108.0e6, 1800, 1000},
        {"1600x1200 60 Hz", "VESA DMT", 1600, 1200, 60, false, 162.0e6, 2160, 1250},
        {"1680x1050 60 Hz (reduced blanking)", "CVT-RB", 1680, 1050, 59.88, false, 119.0e6, 1840, 1080},
        {"1680x1050 60 Hz", "CVT", 1680, 1050, 59.95, false, 146.25e6, 2240, 1089},
        {"1920x1080 60 Hz (reduced blanking)", "CVT-RB", 1920, 1080, 59.93, false, 138.5e6, 2080, 1111},
        {"1920x1200 60 Hz (reduced blanking)", "CVT-RB", 1920, 1200, 59.95, false, 154.0e6, 2080, 1235},
        {"2560x1440 60 Hz (reduced blanking)", "CVT-RB", 2560, 1440, 59.95, false, 241.5e6, 2720, 1481},
        {"2560x1600 60 Hz (reduced blanking)", "CVT-RB", 2560, 1600, 59.97, false, 268.5e6, 2720, 1646},
        {"3840x2160 60 Hz (reduced blanking)", "CVT-RB", 3840, 2160, 60.0, false, 533.25e6, 4000, 2222},
    };
    return t;
}

int findTiming(const char* name) {
    const auto& t = timings();
    for (size_t i = 0; i < t.size(); i++) if (!strcmp(t[i].name, name)) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------------------------------- lines

std::vector<Line> findLines(const float* db, int n, double f0Hz, double binHz, double thrDb, size_t maxLines) {
    std::vector<Line> out;
    if (!db || n < 16 || binHz <= 0) return out;
    // the local level: medians of blocks, interpolated between their centres
    const int B = std::max(32, std::min(512, n / 16));
    const int nb = (n + B - 1) / B;
    std::vector<float> med((size_t)nb), tmp;
    for (int b = 0; b < nb; b++) {
        tmp.assign(db + (size_t)b * B, db + std::min(n, (b + 1) * B));
        std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
        med[(size_t)b] = tmp[tmp.size() / 2];
    }
    auto floorAt = [&](int k) {
        const double p = (k + 0.5) / B - 0.5;
        const int i = std::max(0, std::min(nb - 1, (int)std::floor(p)));
        const int j = std::min(nb - 1, i + 1);
        const double w = std::max(0.0, std::min(1.0, p - i));
        return (float)(med[(size_t)i] * (1 - w) + med[(size_t)j] * w);
    };
    for (int k = 2; k < n - 2; k++) {
        const float v = db[k];
        if (!(v > db[k - 1] && v >= db[k + 1])) continue;
        const float lvl = v - floorAt(k);
        if (lvl < thrDb) continue;
        // narrow: it falls by 3 dB within 6 bins on both sides (a hump or a wide signal is not a line)
        bool lft = false, rgt = false;
        for (int d = 1; d <= 6; d++) {
            if (k - d >= 0 && db[k - d] < v - 3) lft = true;
            if (k + d < n && db[k + d] < v - 3) rgt = true;
        }
        if (!lft || !rgt) continue;
        const double a = db[k - 1], b = v, c = db[k + 1];
        const double den = a - 2 * b + c;
        const double off = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
        out.push_back({f0Hz + (k + off) * binHz, lvl});
    }
    std::sort(out.begin(), out.end(), [](const Line& x, const Line& y) { return x.levelDb > y.levelDb; });
    if (out.size() > maxLines) out.resize(maxLines);
    return out;
}

// ---------------------------------------------------------------------------------------------------- matching

namespace {

struct Sorted {
    std::vector<Line> v;   // by frequency
    int closest(double f, double tol) const {
        auto it = std::lower_bound(v.begin(), v.end(), f - tol, [](const Line& l, double x) { return l.hz < x; });
        int best = -1;
        for (; it != v.end() && it->hz <= f + tol; ++it) if (best < 0 || it->levelDb > v[(size_t)best].levelDb) best = (int)(it - v.begin());
        return best;
    }
};

struct Harm { double f = 0, ppm = 0; std::vector<int> k; std::vector<int> idx; int expected = 0; };

// The harmonics of a clock near p (within tolPpm): seeded from each line, the seed that explains the most harmonics
Harm harmonicsOf(const Sorted& s, double p, double lo, double hi, double binHz, const Config& c) {
    Harm best;
    for (const Line& L : s.v) {
        const int k = (int)std::lround(L.hz / p);
        if (k < 1 || k > c.maxHarmonic) continue;
        const double f = L.hz / k, ppm = (f / p - 1) * 1e6;
        if (std::fabs(ppm) > c.tolPpm) continue;
        Harm h;
        h.f = f; h.ppm = ppm;
        for (int j = 1; j <= c.maxHarmonic; j++) {
            const double fj = j * f;
            if (fj < lo || fj > hi) continue;
            h.expected++;
            const double tol = 1.5 * binHz * std::max(1.0, (double)j / k) + 5e-6 * fj;
            const int i = s.closest(fj, tol);
            if (i >= 0) { h.k.push_back(j); h.idx.push_back(i); }
        }
        if (h.k.size() > best.k.size() || (h.k.size() == best.k.size() && std::fabs(h.ppm) < std::fabs(best.ppm))) best = h;
    }
    if (best.k.size() >= 2) {   // the clock from all of them (least squares through zero)
        double num = 0, den = 0;
        for (size_t i = 0; i < best.k.size(); i++) { num += best.k[i] * s.v[(size_t)best.idx[i]].hz; den += (double)best.k[i] * best.k[i]; }
        best.f = num / den; best.ppm = (best.f / p - 1) * 1e6;
    }
    return best;
}

// Lines spaced by sp on both sides of the lines at idx: hits and misses (positions inside the span only)
// extra: lines inside the comb's reach that are not on it (a comb at twice the true spacing explains only half of them)
void combAt(const Sorted& s, const std::vector<int>& idx, double sp, double lo, double hi, double binHz, int maxComb, int& hits, int& miss, std::vector<int>* members,
            int* extra = nullptr) {
    hits = miss = 0;
    if (extra) *extra = 0;
    const double tol = std::max(1.5 * binHz, 2e-3 * sp);
    for (int i : idx) {
        const double c0 = s.v[(size_t)i].hz;
        if (extra) {
            const double reach = maxComb * sp + tol;
            auto it = std::lower_bound(s.v.begin(), s.v.end(), c0 - reach, [](const Line& l, double x) { return l.hz < x; });
            for (; it != s.v.end() && it->hz <= c0 + reach; ++it) {
                const double d = (it->hz - c0) / sp;
                const double r = std::round(d);
                if (std::fabs(it->hz - c0) <= tol) continue;   // the harmonic itself
                if (r == 0 || std::fabs(d - r) * sp > tol) (*extra)++;
            }
        }
        for (int m = 1; m <= maxComb; m++) for (int sg = -1; sg <= 1; sg += 2) {
            const double f = c0 + sg * m * sp;
            if (f < lo || f > hi) continue;
            const int j = s.closest(f, tol);
            if (j >= 0) { hits++; if (members) members->push_back(j); } else miss++;
        }
    }
}

// The best line spacing between sMin and sMax around the lines at idx (for a clock not in the table)
double bestComb(const Sorted& s, const std::vector<int>& idx, double sMin, double sMax, double lo, double hi, double binHz, int maxComb, int& hitsOut) {
    double best = 0;
    int bestScore = 0;
    hitsOut = 0;
    for (int i : idx) {
        const double c0 = s.v[(size_t)i].hz;
        for (const Line& L : s.v) {
            const double d = std::fabs(L.hz - c0);
            for (int m = 1; m <= 3; m++) {
                const double sp = d / m;
                if (sp < sMin || sp > sMax || sp < 4 * binHz) continue;
                int h, ms;
                combAt(s, idx, sp, lo, hi, binHz, maxComb, h, ms, nullptr);
                const int sc = 2 * h - ms;
                if (sc > bestScore) { bestScore = sc; best = sp; hitsOut = h; }
            }
        }
    }
    return best;
}

std::string mhz(double hz) { char b[48]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }
std::string khz(double hz) { char b[48]; snprintf(b, sizeof b, "%.3f kHz", hz / 1e3); return b; }

} // namespace

Result analyse(const std::vector<Line>& lines, double lo, double hi, double binHz, const Config& c) {
    Result R;
    Sorted s;
    for (const Line& l : lines) if (l.levelDb >= c.minLevelDb && l.hz >= lo && l.hz <= hi) s.v.push_back(l);
    std::sort(s.v.begin(), s.v.end(), [](const Line& a, const Line& b) { return a.hz < b.hz; });
    if (s.v.empty() || binHz <= 0) return R;
    const auto& T = timings();
    std::map<double, Harm> cache;   // by table clock
    struct Cand { Match m; std::vector<int> mem; };
    std::vector<Cand> cands;
    for (size_t ti = 0; ti < T.size(); ti++) {
        const Timing& t = T[ti];
        auto it = cache.find(t.pclkHz);
        if (it == cache.end()) it = cache.emplace(t.pclkHz, harmonicsOf(s, t.pclkHz, lo, hi, binHz, c)).first;
        const Harm& h = it->second;
        if (h.k.empty()) continue;
        Cand cd;
        Match& m = cd.m;
        m.timing = (int)ti; m.pclkHz = h.f; m.ppm = h.ppm; m.harmonics = h.k; m.expected = std::max(1, h.expected);
        const int nh = (int)h.k.size();
        // a lower harmonic is the likelier reading of the same line (the fundamental is the strongest): a small tie-breaker
        double score = 12.0 * nh + (nh >= 3 ? 10 : 0) - 1.5 * std::min(10, m.expected - nh) - 6 * std::fabs(h.ppm) / c.tolPpm - std::min(4, h.k[0] - 1);
        const double fh = h.f / t.htotal;
        if (binHz * 4 <= fh) {   // the comb can be resolved
            int hits, miss, extra;
            combAt(s, h.idx, fh, lo, hi, binHz, c.maxComb, hits, miss, &cd.mem, &extra);
            m.combLines = hits;
            if (hits >= 2) m.combHz = fh;
            score += std::min(90.0, std::max(0.0, 5.0 * hits - 2.5 * miss - 2.5 * extra));
        }
        m.score = score;
        cd.mem.insert(cd.mem.end(), h.idx.begin(), h.idx.end());
        cands.push_back(std::move(cd));
    }
    // a display-like clock that is not in the table: at least four harmonics of one clock between 20 and 700 MHz
    {
        std::vector<Line> strong = s.v;
        std::sort(strong.begin(), strong.end(), [](const Line& a, const Line& b) { return a.levelDb > b.levelDb; });
        if (strong.size() > 30) strong.resize(30);
        Harm bestH;
        for (const Line& L : strong) for (int k = 1; k <= 8; k++) {
            const double f = L.hz / k;
            if (f < 20e6 || f > 700e6) continue;
            bool inTable = false;
            for (const Timing& t : T) if (std::fabs(f / t.pclkHz - 1) * 1e6 <= c.tolPpm * 1.5) inTable = true;
            if (inTable) continue;
            Config c2 = c; c2.tolPpm = 1;   // only this clock
            Harm h = harmonicsOf(s, f, lo, hi, binHz, c2);
            if (h.k.size() > bestH.k.size()) bestH = h;
        }
        if (bestH.k.size() >= 4) {
            Cand cd;
            Match& m = cd.m;
            m.timing = -1; m.pclkHz = bestH.f; m.harmonics = bestH.k; m.expected = bestH.expected;
            int hits = 0;
            const double sp = bestComb(s, bestH.idx, 15e3, 200e3, lo, hi, binHz, c.maxComb, hits);
            if (sp > 0 && hits >= 3) { m.combHz = sp; m.combLines = hits; }
            const int nh = (int)bestH.k.size();
            m.score = std::min(45.0, 8.0 * nh - 1.5 * std::min(10, m.expected - nh) + (m.combLines >= 3 ? 10 : 0));
            cd.mem = bestH.idx;
            cands.push_back(std::move(cd));
        }
    }
    if (cands.empty()) return R;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.m.score > b.m.score; });
    const double best = cands[0].m.score;
    R.confidence = 1 - std::exp(-std::max(0.0, best) / 30);
    if (cands[0].m.timing < 0) R.confidence = std::min(R.confidence, 0.6);
    double sum = 0;
    for (const Cand& cd : cands) sum += std::exp((cd.m.score - best) / 8);
    for (Cand& cd : cands) {
        cd.m.confidence = R.confidence * std::exp((cd.m.score - best) / 8) / sum;
        if (R.matches.size() < 6) R.matches.push_back(cd.m);   // the alternatives stay listed, however unlikely
    }
    R.found = R.confidence >= 0.45;
    const Match& b = R.matches[0];
    R.pclkHz = b.pclkHz;
    R.tableMatch = b.timing >= 0;
    std::vector<int> mem = cands[0].mem;
    std::sort(mem.begin(), mem.end());
    mem.erase(std::unique(mem.begin(), mem.end()), mem.end());
    for (int i : mem) R.members.push_back(s.v[(size_t)i].hz);
    // evidence
    char e[200];
    std::string hs;
    for (size_t i = 0; i < b.harmonics.size() && i < 12; i++) hs += (i ? ", " : "") + std::to_string(b.harmonics[i]);
    snprintf(e, sizeof e, "harmonic%s %s of %s (%+.0f ppm), %zu of the %d inside the range", b.harmonics.size() == 1 ? "" : "s", hs.c_str(), mhz(b.pclkHz).c_str(), b.ppm,
             b.harmonics.size(), b.expected);
    R.evidence.push_back(e);
    if (b.combLines > 0 && b.combHz > 0) {
        snprintf(e, sizeof e, "%d lines of a comb every %s around them (the line rate: %.0f pixels per line)", b.combLines, khz(b.combHz).c_str(), b.pclkHz / b.combHz);
        R.evidence.push_back(e);
    } else if (binHz * 4 > 27e3) {
        R.evidence.push_back("the resolution is too coarse to see the line-rate comb (zoom in on a harmonic to tell the timings apart)");
    } else R.evidence.push_back("no line-rate comb around the clock");
    if (b.timing >= 0) {
        const Timing& t = T[(size_t)b.timing];
        snprintf(e, sizeof e, "%s (%s): %dx%d total, line rate %s", t.name, t.standard, t.htotal, t.vtotal, khz(t.lineRateHz()).c_str());
        R.evidence.push_back(e);
        snprintf(e, sizeof e, "%s (%s), pixel clock %s, %.0f %%", t.name, t.standard, mhz(b.pclkHz).c_str(), b.confidence * 100);
    } else {
        snprintf(e, sizeof e, "Display-like clock %s, not a standard timing%s, %.0f %%", mhz(b.pclkHz).c_str(),
                 b.combHz > 0 ? (", about " + std::to_string((int)std::lround(b.pclkHz / b.combHz)) + " pixels per line").c_str() : "", b.confidence * 100);
    }
    R.summary = e;
    return R;
}

Result analyseSpectrum(const float* db, int n, double f0Hz, double binHz, const Config& c) {
    const std::vector<Line> l = findLines(db, n, f0Hz, binHz, std::max(6.0, c.minLevelDb));
    return analyse(l, f0Hz, f0Hz + n * binHz, binHz, c);
}

Result analyseIq(const cf32* x, size_t n, double fs, double rf, const Config& c) {
    if (!x || n < 4096 || fs <= 0) return Result();
    int N = 65536;
    while ((size_t)N > n) N >>= 1;
    const size_t use = std::min<size_t>(n, 1u << 20);
    std::vector<float> win((size_t)N);
    for (int i = 0; i < N; i++) {   // Blackman-Harris (4 terms)
        const double a = 2 * kPi * i / (N - 1);
        win[(size_t)i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a));
    }
    std::vector<double> acc((size_t)N, 0.0);
    std::vector<cf32> buf((size_t)N);
    Fft fft(N);
    int segs = 0;
    for (size_t s0 = 0; s0 + (size_t)N <= use && segs < 16; s0 += (size_t)N / 2, segs++) {
        for (int i = 0; i < N; i++) buf[(size_t)i] = x[s0 + (size_t)i] * win[(size_t)i];
        fft.forward(buf.data());
        for (int i = 0; i < N; i++) acc[(size_t)i] += std::norm(buf[(size_t)i]);
    }
    if (!segs) return Result();
    std::vector<float> db((size_t)N);
    for (int k = 0; k < N; k++) db[(size_t)k] = (float)(10 * std::log10(acc[(size_t)((k + N / 2) % N)] / segs + 1e-30));
    // the band edges (anti-alias filter roll-off) are left out
    const int edge = N / 40;
    const double bin = fs / N;
    // few spectra averaged scatter more: a higher threshold keeps noise peaks out
    const double thr = std::max(c.minLevelDb, segs >= 8 ? 8.0 : segs >= 4 ? 10.0 : 12.0);
    const std::vector<Line> l = findLines(db.data() + edge, N - 2 * edge, rf - fs / 2 + edge * bin, bin, thr);
    return analyse(l, rf - fs / 2 + edge * bin, rf + fs / 2 - edge * bin, bin, c);
}

std::string toText(const Result& r) {
    std::string t = std::string(label()) + ": " + (r.found ? "" : "unlikely; ") + r.summary + "\n";
    for (const std::string& e : r.evidence) t += "  " + e + "\n";
    const auto& T = timings();
    for (size_t i = 1; i < r.matches.size(); i++) {
        const Match& m = r.matches[i];
        char b[200];
        snprintf(b, sizeof b, "  or %s, %.0f %%\n", m.timing >= 0 ? T[(size_t)m.timing].name : "an unknown timing", m.confidence * 100);
        t += b;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------------- generators

std::vector<cf32> generateIq(const GenConfig& g) {
    const auto& T = timings();
    std::vector<cf32> out;
    if (g.timing < 0 || (size_t)g.timing >= T.size() || g.rateHz <= 0) return out;
    const Timing& t = T[(size_t)g.timing];
    const size_t n = (size_t)std::min(4e6, g.seconds * g.rateHz);
    out.assign(n, cf32(0, 0));
    std::mt19937_64 rng(g.seed);
    std::normal_distribution<float> nd(0.f, (float)std::sqrt(0.5));
    std::uniform_real_distribution<double> ud(0, 1);
    const double p = t.pclkHz * (1 + g.ppm * 1e-6), fh = p / t.htotal, fv = fh / t.vtotal * (t.interlaced ? 2 : 1);
    const double A = std::sqrt(std::pow(10.0, g.levelDb / 10.0) / 65536.0);
    struct Tone { double f, a, ph; };
    std::vector<Tone> tones;
    const double half = 0.45 * g.rateHz;
    for (int k = 1; k <= 64; k++) {
        const double d = k * p - g.rfCentreHz;
        if (std::fabs(d) > half) continue;
        const double ak = A * (k % 2 ? 1.0 : 0.5);
        tones.push_back({d, ak, ud(rng) * 2 * kPi});
        // the picture data repeating every line: a comb at the line rate, falling off away from the clock
        for (int m = 1; m <= 400; m++) for (int sg = -1; sg <= 1; sg += 2) {
            const double f = d + sg * m * fh;
            if (std::fabs(f) > half) continue;
            const double a = ak * std::pow(10.0, g.combDb / 20.0) * std::exp(-m / 15.0) * (0.5 + ud(rng));
            tones.push_back({f, a, ud(rng) * 2 * kPi});
        }
        // the frame: weak lines at the frame rate next to the clock
        for (int sg = -1; sg <= 1; sg += 2) tones.push_back({d + sg * fv, ak * 0.03, ud(rng) * 2 * kPi});
    }
    for (const Tone& tn : tones) {
        const std::complex<double> w = std::polar(1.0, 2 * kPi * tn.f / g.rateHz);
        std::complex<double> z = std::polar(tn.a, tn.ph);
        for (size_t i = 0; i < n; i++) {
            out[i] += cf32((float)z.real(), (float)z.imag());
            z *= w;
            if ((i & 4095) == 4095) z *= tn.a / std::abs(z);
        }
    }
    for (size_t i = 0; i < n; i++) out[i] += cf32(nd(rng), nd(rng));
    return out;
}

std::vector<float> simulateSpectrum(int timing, double f0Hz, double binHz, int n, double ppm, float floorDb, float levelDb, uint64_t seed) {
    std::vector<float> db((size_t)std::max(0, n));
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> nd(0.f, 1.2f), lv(0.f, 2.f);
    for (float& v : db) v = floorDb + nd(rng);
    const auto& T = timings();
    if (timing < 0 || (size_t)timing >= T.size() || binHz <= 0) return db;
    const double p = T[(size_t)timing].pclkHz * (1 + ppm * 1e-6);
    for (int k = 1; k <= 64; k++) {
        const double b = (k * p - f0Hz) / binHz;
        if (b < 1 || b > n - 2) continue;
        const float l = floorDb + levelDb - 8.f * (float)std::log10((double)k) - (k % 2 ? 0.f : 6.f) + lv(rng);
        const int i = (int)std::floor(b);
        // a Blackman-Harris main lobe sampled at the line's position
        for (int d = -2; d <= 3; d++) {
            const int j = i + d;
            if (j < 0 || j >= n) continue;
            const double x = j - b;
            const double att = 6.0 * x * x;   // about -6 dB one bin away
            db[(size_t)j] = std::max(db[(size_t)j], (float)(l - att));
        }
    }
    return db;
}

} // namespace dispem
} // namespace dect2
