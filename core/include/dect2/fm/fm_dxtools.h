// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM DX helpers that look at what the census / scanner / listener already collected (pure rules, no GUI):
//  1. PiWatch      co-channel interference: on ONE frequency two different PI codes alternate (two stations sharing the channel, often a tropo or sporadic-E opening);
//                  reports both PIs with their share and which one is stronger by SNR. A single wrong PI from a bit error is not an alternation: a PI must be seen
//                  at least `minSeen` times and by at least `minShare` of the reads.
//  2. adjacentRisk marks stations in a list that sit 100 / 200 kHz from a much stronger one (the weaker one will splatter and lose RDS): the survey table gets a warning.
//  3. bestHeading  antenna direction helper: the user (or a rotator) records the level at some headings; the result is the heading of the best lobe (circular, smoothed over
//                  neighbouring samples) with the gain over the average and a width, so a DXer turns the antenna once and gets the answer.
// Tests: tests/fmdx40/test_fm_dxtools.cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::fmdx {

struct PiShare { int pi = 0; int seen = 0; double share = 0; double snrDb = -99; };

class PiWatch {
public:
    int minSeen = 4;
    double minShare = 0.15;
    // one RDS PI read on a frequency (kHz resolution), with the SNR at that moment
    void add(double mhz, int pi, double snrDb) {
        if (pi <= 0) return;
        Per& p = per_[key(mhz)];
        p.total++;
        Acc& a = p.pi[pi];
        a.n++;
        a.snr = a.n == 1 ? snrDb : a.snr + (snrDb - a.snr) / (double)a.n;
    }
    // frequencies with two or more PIs that qualify, strongest-share first within each frequency
    struct Co { double mhz = 0; std::vector<PiShare> pis; };
    std::vector<Co> coChannel() const {
        std::vector<Co> out;
        for (const auto& f : per_) {
            Co c;
            c.mhz = (double)f.first / 1000.0;
            for (const auto& q : f.second.pi) {
                const double sh = (double)q.second.n / (double)f.second.total;
                if (q.second.n >= minSeen && sh >= minShare) c.pis.push_back({q.first, q.second.n, sh, q.second.snr});
            }
            if (c.pis.size() >= 2) {
                std::sort(c.pis.begin(), c.pis.end(), [](const PiShare& a, const PiShare& b) { return a.share > b.share; });
                out.push_back(c);
            }
        }
        return out;
    }
    void clear() { per_.clear(); }
private:
    struct Acc { int n = 0; double snr = 0; };
    struct Per { int total = 0; std::map<int, Acc> pi; };
    static long key(double mhz) { return std::lround(mhz * 1000.0); }
    std::map<long, Per> per_;
};

struct ListedStation { double mhz = 0; double snrDb = 0; };
// For every station: the offset (kHz) and the excess level (dB) of the strongest station 100 or 200 kHz away if it is at least `minOverDb` stronger; 0 = no risk.
struct AdjRisk { int index = 0; double neighbourKhz = 0; double overDb = 0; };
inline std::vector<AdjRisk> adjacentRisk(const std::vector<ListedStation>& s, double minOverDb = 20.0) {
    std::vector<AdjRisk> out;
    for (size_t i = 0; i < s.size(); i++) {
        AdjRisk best;
        best.index = (int)i;
        for (size_t j = 0; j < s.size(); j++) {
            if (i == j) continue;
            const double dk = (s[j].mhz - s[i].mhz) * 1000.0;
            const double ad = std::fabs(dk);
            if (!(std::fabs(ad - 100) < 20 || std::fabs(ad - 200) < 20)) continue;
            const double over = s[j].snrDb - s[i].snrDb;
            if (over >= minOverDb && over > best.overDb) { best.overDb = over; best.neighbourKhz = dk; }
        }
        if (best.overDb > 0) out.push_back(best);
    }
    return out;
}

struct HeadingSample { double deg = 0; double levelDb = 0; };
struct BestHeading { bool ok = false; double deg = 0; double gainDb = 0; double widthDeg = 0; };
// Samples at any headings (several per heading are averaged in power). Needs at least 4 distinct headings spread over at least 180 degrees. The lobe is the peak of the
// 3-point circular smoothing of the headings sorted by angle; widthDeg is the angular span around it that stays within 3 dB.
inline BestHeading bestHeading(std::vector<HeadingSample> in) {
    BestHeading r;
    if (in.size() < 4) return r;
    std::sort(in.begin(), in.end(), [](const HeadingSample& a, const HeadingSample& b) { return std::fmod(std::fmod(a.deg, 360) + 360, 360) < std::fmod(std::fmod(b.deg, 360) + 360, 360); });
    std::vector<double> d, p;   // distinct headings and mean power
    std::vector<int> cnt;
    for (const HeadingSample& h : in) {
        const double a = std::fmod(std::fmod(h.deg, 360) + 360, 360);
        const double pw = std::pow(10.0, h.levelDb / 10.0);
        if (!d.empty() && std::fabs(d.back() - a) < 1.0) { p.back() += pw; cnt.back()++; }
        else { d.push_back(a); p.push_back(pw); cnt.push_back(1); }
    }
    const size_t n = d.size();
    if (n < 4) return r;
    double maxGap = 0;
    for (size_t i = 0; i < n; i++) maxGap = std::max(maxGap, std::fmod(d[(i + 1) % n] - d[i] + 360, 360) == 0 ? 360.0 : std::fmod(d[(i + 1) % n] - d[i] + 360, 360));
    if (maxGap > 180) return r;   // the headings do not go around enough to know where the lobe is
    std::vector<double> lv(n), sm(n);
    double mean = 0;
    for (size_t i = 0; i < n; i++) { lv[i] = 10.0 * std::log10(p[i] / cnt[i]); mean += p[i] / cnt[i]; }
    mean = 10.0 * std::log10(mean / (double)n);
    for (size_t i = 0; i < n; i++) sm[i] = 10.0 * std::log10((std::pow(10, lv[(i + n - 1) % n] / 10) + 2 * std::pow(10, lv[i] / 10) + std::pow(10, lv[(i + 1) % n] / 10)) / 4.0);
    size_t b = 0;
    for (size_t i = 1; i < n; i++) if (sm[i] > sm[b]) b = i;
    r.ok = true;
    r.deg = d[b];
    r.gainDb = sm[b] - mean;
    // width: walk both ways while within 3 dB
    auto span = [&](int dir) { double w = 0; size_t i = b; for (size_t k = 0; k < n; k++) { const size_t j = (size_t)(((long)i + dir + (long)n) % (long)n); if (sm[j] < sm[b] - 3.0) break; w += std::fabs(std::fmod(d[j] - d[i] + 540, 360) - 180); i = j; } return w; };
    r.widthDeg = span(1) + span(-1);
    return r;
}

}  // namespace dect2::fmdx
