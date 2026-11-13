// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM burst classifier (4.0): for a 433 / 868 / 915 MHz burst the decoder does not know, work out how it is coded from the pulse and gap lengths alone,
// so the user (or a protocol author) gets the line coding, the short and long lengths, the row separator, the number of repeated rows and the bits per
// row, instead of a raw pulse list. Pure code; the input is the same pulse train (on / off in microseconds) the ISM receiver already produces.
//   PWM: two pulse lengths, one gap length        (each bit is a pulse, the length carries the bit)
//   PPM: one pulse length, two gap lengths        (the gap carries the bit)
//   Manchester / biphase: every length is one or two units, pulses and gaps both
//   PCM / NRZ-like: lengths are many multiples of one unit
// Also gives a "flex" description in the rtl_433 key=value form (m=OOK_PWM,s=,l=,r=), the way a protocol is typed into a decoder.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct IsmTimedPulse { double onUs = 0, offUs = 0; };
struct IsmClass {
    std::string coding;          // "PWM", "PPM", "Manchester", "PCM", "unknown"
    double shortUs = 0, longUs = 0;   // PWM: pulse lengths; PPM: gap lengths; Manchester / PCM: the unit and 2 x unit
    double resetUs = 0;               // gap that separates the repeated rows
    int rows = 0;                     // repeated rows (the same frame sent again)
    int bitsPerRow = 0;               // of the longest row; Manchester counts a bit per two units
    std::string flex;
    double confidence = 0;            // 0..1
};

namespace ismdetail {
// 1-D clusters: sort, split where the next value is 1.4 times the previous (and not within 12 us)
inline std::vector<double> clusters(std::vector<double> v) {
    std::vector<double> c; if (v.empty()) return c;
    std::sort(v.begin(), v.end());
    double sum = v[0]; int n = 1;
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i] > v[i - 1] * 1.4 && v[i] - v[i - 1] > 12) { c.push_back(sum / n); sum = 0; n = 0; }
        sum += v[i]; n++;
    }
    c.push_back(sum / n);
    return c;
}
// share of the values within 25 % of their nearest cluster centre: noise has no tight clusters
inline double fit(const std::vector<double>& v, const std::vector<double>& c) {
    if (v.empty() || c.empty()) return 0;
    int ok = 0;
    for (double x : v) { double best = 1e9; for (double k : c) best = std::min(best, std::fabs(x - k) / k); if (best <= 0.25) ok++; }
    return (double)ok / v.size();
}
} // namespace ismdetail

inline IsmClass ismClassify(const std::vector<IsmTimedPulse>& p) {
    IsmClass r; r.coding = "unknown";
    if (p.size() < 8) return r;
    // gaps above 4 x the median gap separate rows (the last pulse's gap is the end of the burst)
    std::vector<double> offs; for (const auto& x : p) offs.push_back(x.offUs);
    std::vector<double> so = offs; std::sort(so.begin(), so.end());
    const double med = so[so.size() / 2];
    const double sepLimit = std::max(med * 4.0, 1500.0);
    // rows
    std::vector<std::vector<IsmTimedPulse>> rows(1);
    double resetSum = 0; int resetN = 0;
    for (const auto& x : p) {
        if (x.offUs > sepLimit) { rows.back().push_back({x.onUs, 0}); resetSum += x.offUs; resetN++; rows.emplace_back(); }
        else rows.back().push_back(x);
    }
    if (rows.back().empty()) rows.pop_back();
    r.rows = (int)rows.size();
    std::vector<double> on, off, all;
    size_t longest = 0;
    for (const auto& row : rows) {
        longest = std::max(longest, row.size());
        for (size_t i = 0; i < row.size(); i++) { on.push_back(row[i].onUs); all.push_back(row[i].onUs); if (i + 1 < row.size()) { off.push_back(row[i].offUs); all.push_back(row[i].offUs); } }
    }
    if (resetN) r.resetUs = std::round(resetSum / resetN * 0.5 / 10) * 10;   // half the typical separator: the decoder's reset limit
    else r.resetUs = std::round(std::max(sepLimit, 1500.0) / 10) * 10;
    const auto con = ismdetail::clusters(on), cof = ismdetail::clusters(off), cal = ismdetail::clusters(all);
    const bool tightOn = ismdetail::fit(on, con) >= 0.9, tightOff = ismdetail::fit(off, cof) >= 0.9, tightAll = ismdetail::fit(all, cal) >= 0.9;
    char b[160];
    if (con.size() == 1 && cof.size() == 2 && tightOn && tightOff) {
        r.coding = "PPM"; r.shortUs = cof[0]; r.longUs = cof[1]; r.bitsPerRow = (int)longest; r.confidence = 0.85;
        std::snprintf(b, sizeof b, "n=unknown,m=OOK_PPM,s=%.0f,l=%.0f,r=%.0f", r.shortUs, r.longUs, r.resetUs); r.flex = b;
    } else if (con.size() == 2 && cof.size() == 1 && tightOn && tightOff) {
        r.coding = "PWM"; r.shortUs = con[0]; r.longUs = con[1]; r.bitsPerRow = (int)longest; r.confidence = 0.85;
        std::snprintf(b, sizeof b, "n=unknown,m=OOK_PWM,s=%.0f,l=%.0f,r=%.0f", r.shortUs, r.longUs, r.resetUs); r.flex = b;
    } else if (tightAll && cal.size() == 2 && cal[1] / cal[0] > 1.6 && cal[1] / cal[0] < 2.5) {
        r.coding = "Manchester"; r.shortUs = cal[0]; r.longUs = cal[1];
        // units in the longest row ~ its duration / short
        double rowDur = 0; for (const auto& row : rows) { double d = 0; for (const auto& x : row) d += x.onUs + x.offUs; rowDur = std::max(rowDur, d); }
        r.bitsPerRow = (int)std::lround(rowDur / r.shortUs / 2.0); r.confidence = 0.7;
        std::snprintf(b, sizeof b, "n=unknown,m=OOK_MC_ZEROBIT,s=%.0f,l=%.0f,r=%.0f", r.shortUs, r.longUs, r.resetUs); r.flex = b;
    } else if (tightAll && cal.size() >= 3 && cal.size() <= 8) {
        // lengths are multiples of one unit: PCM
        const double u = cal[0]; bool mult = true;
        for (double c : cal) { const double k = c / u; if (std::fabs(k - std::round(k)) > 0.3) mult = false; }
        if (mult) {
            r.coding = "PCM"; r.shortUs = u; r.longUs = cal.back();
            double rowDur = 0; for (const auto& row : rows) { double d = 0; for (const auto& x : row) d += x.onUs + x.offUs; rowDur = std::max(rowDur, d); }
            r.bitsPerRow = (int)std::lround(rowDur / u); r.confidence = 0.5;
            std::snprintf(b, sizeof b, "n=unknown,m=OOK_PCM,s=%.0f,l=%.0f,r=%.0f", u, u, r.resetUs); r.flex = b;
        }
    }
    return r;
}

} // namespace dect2
