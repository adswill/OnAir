// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specx/specx_measure.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace specx {

const double kRollOffs[3] = {0.20, 0.25, 0.35};

bool parseFreq(const std::string& text, double& hz) {
    std::string s;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '_') continue;
        s += c == ',' ? '.' : (char)std::tolower((unsigned char)c);
    }
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return false;
    std::string u(end);
    if (u.size() >= 2 && u.compare(u.size() - 2, 2, "hz") == 0) u.erase(u.size() - 2);
    double mul;
    if (u.empty()) mul = 1e6;
    else if (u == "g") mul = 1e9;
    else if (u == "m") mul = 1e6;
    else if (u == "k") mul = 1e3;
    else return false;
    if (std::string(end).find("hz") != std::string::npos && u.empty()) mul = 1;   // "915000000 Hz"
    hz = v * mul;
    return hz >= 0;
}

std::string freqText(double hz) {
    char b[48];
    const double a = std::fabs(hz);
    if (a >= 1e9) snprintf(b, sizeof b, "%.6f GHz", hz / 1e9);
    else if (a >= 1e6) snprintf(b, sizeof b, "%.4f MHz", hz / 1e6);
    else if (a >= 1e3) snprintf(b, sizeof b, "%.3f kHz", hz / 1e3);
    else snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}

const LnbPreset* lnbPresets(int& count) {
    static const LnbPreset k[] = {{"Universal LNB, low band (9750 MHz)", 9750, 0}, {"Universal LNB, high band (10600 MHz)", 10600, 0},
                                  {"QO-100 (9750 MHz)", 9750, 0}, {"Ku LNB, Bullseye / Universal Single, low (9750 MHz, IF 739-1950)", 9750, 0},
                                  {"Ku LNB, Bullseye / Universal Single, high (10600 MHz, IF 1100-2150)", 10600, 0}, {"C band LNB (5150 MHz, inverted)", 5150, 2}, {"Upconverter (+125 MHz)", 125, 1}};
    count = (int)(sizeof k / sizeof *k);
    return k;
}

Floor fitFloor(const SpecView& v) {
    Floor f;
    if (!v.db || v.n < 2) { if (v.n == 1) f.a = v.db[0]; return f; }
    // start from the median, then fit a line to the bins at or below the line + 1 dB, a few times
    std::vector<float> s(v.db, v.db + v.n);
    std::nth_element(s.begin(), s.begin() + v.n / 2, s.end());
    f.a = s[(size_t)v.n / 2]; f.b = 0;
    for (int it = 0; it < 6; it++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
        // a band around the line: low enough to leave out carriers, wide enough to keep the noise's own spread (a few dB)
        const double hi = it == 0 ? 0.0 : 1.5;
        for (int k = 0; k < v.n; k++) {
            const double y = v.db[k];
            if (y > f.at(k) + hi || y < -199) continue;
            sx += k; sy += y; sxx += (double)k * k; sxy += k * y; n++;
        }
        if (n < 2) break;
        const double d = n * sxx - sx * sx;
        if (std::fabs(d) < 1e-9) { f.a = sy / n; f.b = 0; continue; }
        f.b = (n * sxy - sx * sy) / d;
        f.a = (sy - f.b * sx) / n;
    }
    // the fit runs through the lower part of the noise: lift it to the mean of the bins near the line (log of averaged power)
    double acc = 0; int m = 0;
    for (int k = 0; k < v.n; k++) {
        const double r = v.db[k] - f.at(k);
        if (r < 4 && v.db[k] > -199) { acc += std::pow(10.0, r / 10); m++; }
    }
    if (m) f.a += 10 * std::log10(acc / m);
    return f;
}

static double lin(double db) { return std::pow(10.0, db / 10); }

Measure measure(const SpecView& v, double loHz, double hiHz, const Floor* fl) {
    Measure r;
    if (loHz > hiHz) std::swap(loHz, hiHz);
    r.loHz = loHz; r.hiHz = hiHz;
    if (!v.db || v.n < 3) return r;
    int i0 = (int)std::ceil(v.bin(loHz)), i1 = (int)std::floor(v.bin(hiHz));
    i0 = std::max(0, i0); i1 = std::min(v.n - 1, i1);
    if (i1 - i0 < 2) return r;
    const Floor F = fl ? *fl : fitFloor(v);
    const int n = i1 - i0 + 1;
    // power and peak from the raw bins
    double p = 0; int pk = i0;
    for (int k = i0; k <= i1; k++) { p += lin(v.db[k]); if (v.db[k] > v.db[pk]) pk = k; }
    r.powerDb = 10 * std::log10(std::max(1e-30, p / std::max(1e-9, v.enbw)));
    r.peakDb = v.db[pk]; r.peakHz = v.freq(pk);
    // a smoothed copy (power average over about 1/48 of the region) for the edges
    const int w = std::max(0, n / 96);
    std::vector<double> sm((size_t)n);
    for (int k = 0; k < n; k++) {
        double a = 0; int c = 0;
        for (int j = std::max(0, k - w); j <= std::min(n - 1, k + w); j++) { a += lin(v.db[i0 + j]); c++; }
        sm[(size_t)k] = 10 * std::log10(a / c);
    }
    int sp = 0;
    for (int k = 1; k < n; k++) if (sm[(size_t)k] > sm[(size_t)sp]) sp = k;
    const double top = sm[(size_t)sp];
    // the edge where the smoothed level falls below lvl, walking out from the top, with a linear interpolation between bins
    auto edge = [&](double lvl, int dir) {
        int k = sp;
        while (k + dir >= 0 && k + dir < n && sm[(size_t)(k + dir)] >= lvl) k += dir;
        if (k + dir < 0 || k + dir >= n) return (double)k;
        const double a = sm[(size_t)k], b = sm[(size_t)(k + dir)];
        return k + dir * (a - lvl) / std::max(1e-9, a - b);
    };
    // the reference is the mean level across the -3 dB band (the smoothed maximum of a noisy flat top lies above it): find the band
    // from the maximum, take its mean, and find it again from that, twice
    double ref = top, l3 = sp, h3 = sp;
    for (int it = 0; it < 3; it++) {
        l3 = edge(ref - 3, -1); h3 = edge(ref - 3, +1);
        double tp = 0; int tc = 0;
        for (int k = (int)std::ceil(l3); k <= (int)std::floor(h3); k++) { tp += lin(sm[(size_t)k]); tc++; }
        if (!tc) break;
        const double m = 10 * std::log10(tp / tc);
        if (it == 2 || std::fabs(m - ref) < 0.01) { ref = m; break; }
        ref = m;
    }
    r.topDb = ref;
    r.bw3Hz = (h3 - l3) * v.binHz;
    const double cBin = i0 + (l3 + h3) / 2;
    r.centreHz = v.freq(cBin);
    r.floorDb = F.at(cBin);
    double lv26 = ref - 26;
    if (lv26 < r.floorDb + 3) { lv26 = r.floorDb + 3; r.bw26AtFloor = true; }
    r.bw26Hz = (edge(lv26, +1) - edge(lv26, -1)) * v.binHz;
    double lv20 = ref - 20;
    if (lv20 < r.floorDb + 3) { lv20 = r.floorDb + 3; r.bw20AtFloor = true; }
    r.bw20Hz = (edge(lv20, +1) - edge(lv20, -1)) * v.binHz;
    r.snrDb = r.topDb - r.floorDb;
    {   // 99 % occupied bandwidth: 0.5 % of the power over the floor on each side
        std::vector<double> cum((size_t)n + 1, 0.0);
        for (int k = 0; k < n; k++) cum[(size_t)k + 1] = cum[(size_t)k] + std::max(0.0, lin(sm[(size_t)k]) - lin(F.at(i0 + k)));
        const double tot = cum[(size_t)n];
        if (tot > 0) {
            auto at = [&](double q) {
                int k = 0;
                while (k < n && cum[(size_t)k + 1] < q * tot) k++;
                const double a = cum[(size_t)k], b = cum[(size_t)std::min(n, k + 1)];
                return k + (b > a ? (q * tot - a) / (b - a) : 0.0);
            };
            const double lo = at(0.005), hi = at(0.995);
            r.obwHz = (hi - lo) * v.binHz;
            r.obwCentreHz = v.freq(i0 + (lo + hi) / 2 - 0.5);
        } else r.obwCentreHz = r.centreHz;
    }
    // flat top: the inner 70 % of the -3 dB band stays within 2 dB of its mean (and is wide enough to tell)
    const double span3 = h3 - l3;
    if (span3 >= 8 && r.snrDb > 3) {
        int bad = 0, cnt = 0;
        for (int k = (int)std::ceil(l3 + 0.15 * span3); k <= (int)std::floor(h3 - 0.15 * span3); k++) {
            cnt++;
            if (std::fabs(sm[(size_t)k] - r.topDb) > 2.0) bad++;
        }
        // a single carrier's -3 dB band is a thin spike: the top over the inner band must be most of the region's top
        r.flatTop = cnt > 0 && bad <= cnt / 10;
    }
    if (r.flatTop) {
        for (int j = 0; j < 3; j++) r.symRate[j] = r.bw26Hz / (1 + kRollOffs[j]);
        const double ro = r.bw3Hz > 0 ? r.bw26Hz / r.bw3Hz - 1 : 0;
        int best = 0;
        for (int j = 1; j < 3; j++) if (std::fabs(kRollOffs[j] - ro) < std::fabs(kRollOffs[best] - ro)) best = j;
        r.rollOff = kRollOffs[best];
        r.symRateBest = r.bw3Hz;
        r.looksDvbs = r.bw3Hz >= 100e3 && r.bw3Hz <= 72e6 && r.snrDb >= 4;
    }
    r.valid = true;
    return r;
}

std::vector<Measure> detect(const SpecView& v, double thrDb, int minBins) {
    std::vector<Measure> out;
    if (!v.db || v.n < 8) return out;
    const Floor F = fitFloor(v);
    std::vector<double> sm((size_t)v.n);
    for (int k = 0; k < v.n; k++) {
        double a = 0; int c = 0;
        for (int j = std::max(0, k - 1); j <= std::min(v.n - 1, k + 1); j++) { a += lin(v.db[j]); c++; }
        sm[(size_t)k] = 10 * std::log10(a / c);
    }
    std::vector<std::pair<int, int>> runs;
    int s = -1, gap = 0;
    for (int k = 0; k <= v.n; k++) {
        const bool up = k < v.n && sm[(size_t)k] > F.at(k) + thrDb;
        if (up) { if (s < 0) s = k; gap = 0; }
        else if (s >= 0) {
            if (++gap > 2 || k == v.n) { const int e = k - gap; if (e - s + 1 >= minBins) runs.push_back({s, e}); s = -1; gap = 0; }
        }
    }
    for (auto& rn : runs) {
        const int w = rn.second - rn.first + 1, m = std::max(2, w / 4);
        const int a = std::max(0, rn.first - m), b = std::min(v.n - 1, rn.second + m);
        Measure ms = measure(v, v.freq(a), v.freq(b), &F);
        if (ms.valid) out.push_back(ms);
    }
    return out;
}

std::vector<double> sweepCentres(double loHz, double hiHz, double spanHz, double keepFrac) {
    std::vector<double> c;
    if (hiHz < loHz) std::swap(loHz, hiHz);
    const double step = std::max(1.0, spanHz * keepFrac);
    const int n = std::max(1, (int)std::ceil((hiHz - loHz) / step - 1e-9));
    for (int i = 0; i < n; i++) c.push_back(loHz + step * (i + 0.5));
    return c;
}

void Panorama::init(double lo, double hi, int bins) {
    loHz = std::min(lo, hi); hiHz = std::max(lo, hi);
    bins = std::max(1, bins);
    binHz = (hiHz - loHz) / bins;
    db.assign((size_t)bins, -200.f);
    seen.assign((size_t)bins, 0);
}

void Panorama::add(const SpecView& v, double keepFrac) {
    if (!v.db || v.n < 2 || db.empty()) return;
    const double mid = v.freq((v.n - 1) / 2.0), half = v.n * v.binHz * keepFrac / 2;
    const double a = std::max(loHz, mid - half), b = std::min(hiHz, mid + half);
    for (int k = (int)std::floor((a - loHz) / binHz); k < (int)db.size(); k++) {
        if (k < 0) continue;
        const double f0 = loHz + k * binHz, f1 = f0 + binHz;
        if (f0 >= b) break;
        if (f1 <= a) continue;
        // the mean power of the source bins under this panorama bin (one at least: the nearest)
        int s0 = (int)std::floor(v.bin(f0) + 0.5), s1 = (int)std::floor(v.bin(f1) - 0.5);
        if (s1 < s0) s0 = s1 = (int)std::floor(v.bin((f0 + f1) / 2) + 0.5);
        s0 = std::max(0, s0); s1 = std::min(v.n - 1, s1);
        if (s1 < s0) continue;
        double p = 0;
        for (int j = s0; j <= s1; j++) p += lin(v.db[j]);
        db[(size_t)k] = (float)(10 * std::log10(p / (s1 - s0 + 1)));
        seen[(size_t)k] = 1;
    }
}

bool Panorama::complete() const {
    for (unsigned char s : seen) if (!s) return false;
    return !seen.empty();
}

} // namespace specx
} // namespace dect2
