// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 satellite TV (DVB-S / S2 / S2X) helpers (pure, no GUI). Tests: tests/d40/test_sat_d40.cpp
//  1. blindScan     a power spectrum of an LNB IF span becomes a transponder list: centre, -3 dB width, symbol rate (width / 1.35 for the 0.35 roll-off, snapped to a standard rate within 3 %,
//                   else the raw value) and the C/N in the occupied band. Neighbouring carriers that touch are split at the dip between them.
//  2. modcodFor     the DVB-S2 normal-frame modulation / code rates (EN 302 307 table of ideal Es/N0 for quasi error free reception) that fit the measured Es/N0 with a margin, best spectral
//                   efficiency first: "what could this transponder be carrying" before the demodulator has locked, and the margin of the one it does carry.
//  3. PointingAid   dish alignment: feeds the live C/N, keeps the peak seen, reports the distance to it, the trend and a tone rate for the beep (the faster the better), so one person turns the dish.
//  4. LnbDrift      beacon-based alignment of the LNB: the measured beacon IF against the expected one gives the LO error in kHz and ppm; a linear fit over time gives the drift per minute and
//                   says when the LNB has settled.
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace dect2::d40 {

struct Transponder { double centreHz = 0, bwHz = 0, symbolRateHz = 0, cnDb = 0; bool snapped = false; };

inline double snapSymbolRate(double sr, bool* snapped = nullptr) {
    static const double std_[] = {1e6, 1.5e6, 2e6, 2.5e6, 3e6, 4e6, 4.167e6, 4.8e6, 5e6, 6e6, 6.111e6, 6.5e6, 6.667e6, 7.5e6, 8e6, 9e6, 10e6, 11e6, 12e6, 13.33e6, 15e6, 15.555e6, 18e6, 20e6, 22e6, 23e6, 24e6, 25e6, 27.5e6, 28e6, 29.5e6, 30e6, 30.0e6, 33e6, 35e6, 36e6, 37.5e6, 40e6, 43.2e6, 45e6};
    double best = sr; double bd = 0.03;
    for (double s : std_) { const double d = std::fabs(sr - s) / s; if (d < bd) { bd = d; best = s; } }
    if (snapped) *snapped = best != sr;
    return best;
}

// db: power per bin (dB), bin 0 = lowest frequency; binHz; startHz: the frequency of bin 0
inline std::vector<Transponder> blindScan(const std::vector<float>& db, double binHz, double startHz, double minBwHz = 1.0e6, double rolloff = 0.35) {
    std::vector<Transponder> out;
    const int n = (int)db.size();
    if (n < 16 || !(binHz > 0)) return out;
    // smooth over ~ 1 MHz (the periodogram ripple of a single sweep would split a plateau)
    const int h = std::max(1, (int)(500e3 / binHz));
    std::vector<double> lin((size_t)n), sm((size_t)n);
    for (int i = 0; i < n; i++) lin[(size_t)i] = std::pow(10.0, (double)db[(size_t)i] / 10.0);
    { double acc = 0; int lo = 0, hi = -1;   // running mean
      for (int i = 0; i < n; i++) { while (hi < std::min(n - 1, i + h)) acc += lin[(size_t)++hi]; while (lo < i - h) acc -= lin[(size_t)lo++]; sm[(size_t)i] = acc / (hi - lo + 1); } }
    std::vector<double> s2 = sm; std::sort(s2.begin(), s2.end());
    const double floorLin = s2[(size_t)n / 10 + 1];       // the quiet gaps between carriers
    const double thr = floorLin * 2.0;                    // +3 dB
    int i = 0;
    while (i < n) {
        if (sm[(size_t)i] < thr) { i++; continue; }
        int j = i; while (j < n && sm[(size_t)j] >= thr) j++;
        // region [i, j): split at deep dips (>1.5 dB below both sides' plateau) when it is wide
        std::vector<std::pair<int, int>> parts = {{i, j}};
        for (size_t pi = 0; pi < parts.size(); pi++) {
            const int a = parts[pi].first, b = parts[pi].second;
            if ((double)(b - a) * binHz < 2 * minBwHz) continue;
            int mn = a + (b - a) / 4; double mv = sm[(size_t)mn];
            for (int k = a + (b - a) / 4; k < b - (b - a) / 4; k++) if (sm[(size_t)k] < mv) { mv = sm[(size_t)k]; mn = k; }
            double l = 0, r = 0;
            for (int k = a; k < mn; k++) l = std::max(l, sm[(size_t)k]);
            for (int k = mn; k < b; k++) r = std::max(r, sm[(size_t)k]);
            if (mv < 0.5 * std::min(l, r)) { parts[pi] = {a, mn}; parts.push_back({mn, b}); }
        }
        for (auto pr : parts) {
            const int a = pr.first, b = pr.second;
            if ((double)(b - a) * binHz < minBwHz) continue;
            std::vector<double> inner(sm.begin() + a, sm.begin() + b); std::sort(inner.begin(), inner.end());
            const double plateau = inner[inner.size() * 3 / 4];
            const double half = plateau / 2.0;
            int e0 = a, e1 = b - 1;
            while (e0 < b - 1 && sm[(size_t)e0] < half) e0++;
            while (e1 > a && sm[(size_t)e1] < half) e1--;
            if (e1 <= e0) continue;
            Transponder t;
            t.bwHz = (double)(e1 - e0 + 1) * binHz;
            if (t.bwHz < minBwHz) continue;
            t.centreHz = startHz + 0.5 * (double)(e0 + e1 + 1) * binHz;
            t.symbolRateHz = snapSymbolRate(t.bwHz / (1.0 + rolloff), &t.snapped);
            double mean = 0; int m = 0;
            for (int k = e0; k <= e1; k++) { mean += lin[(size_t)k]; m++; }
            mean /= m;
            t.cnDb = 10 * std::log10(std::max(1e-9, (mean - floorLin) / floorLin));
            out.push_back(t);
        }
        i = j;
    }
    return out;
}

struct ModcodFit { std::string name; double reqEsN0Db = 0; double efficiency = 0; double marginDb = 0; };
inline std::vector<ModcodFit> modcodFor(double esN0Db, double minMarginDb = 0.5) {
    struct R { const char* n; double req; double bps; };
    static const R tab[] = {
        {"QPSK 1/4", -2.35, 0.5}, {"QPSK 1/3", -1.24, 0.667}, {"QPSK 2/5", -0.30, 0.8}, {"QPSK 1/2", 1.00, 1.0}, {"QPSK 3/5", 2.23, 1.2}, {"QPSK 2/3", 3.10, 1.333},
        {"QPSK 3/4", 4.03, 1.5}, {"QPSK 4/5", 4.68, 1.6}, {"QPSK 5/6", 5.18, 1.667}, {"QPSK 8/9", 6.20, 1.778}, {"QPSK 9/10", 6.42, 1.8},
        {"8PSK 3/5", 5.50, 1.8}, {"8PSK 2/3", 6.62, 2.0}, {"8PSK 3/4", 7.91, 2.25}, {"8PSK 5/6", 9.35, 2.5}, {"8PSK 8/9", 10.69, 2.667}, {"8PSK 9/10", 10.98, 2.7},
        {"16APSK 2/3", 8.97, 2.667}, {"16APSK 3/4", 10.21, 3.0}, {"16APSK 4/5", 11.03, 3.2}, {"16APSK 5/6", 11.61, 3.333}, {"16APSK 8/9", 12.89, 3.556}, {"16APSK 9/10", 13.13, 3.6},
        {"32APSK 3/4", 12.73, 3.75}, {"32APSK 4/5", 13.64, 4.0}, {"32APSK 5/6", 14.28, 4.167}, {"32APSK 8/9", 15.69, 4.444}, {"32APSK 9/10", 16.05, 4.5}};
    std::vector<ModcodFit> out;
    for (const auto& r : tab) if (esN0Db - r.req >= minMarginDb) out.push_back({r.n, r.req, r.bps, esN0Db - r.req});
    std::sort(out.begin(), out.end(), [](const ModcodFit& a, const ModcodFit& b) { if (a.efficiency != b.efficiency) return a.efficiency > b.efficiency; return a.reqEsN0Db < b.reqEsN0Db; });
    return out;
}

class PointingAid {
public:
    // one C/N reading; returns the beep rate in Hz (0 = no signal): 2 Hz at 10 dB under the peak up to 20 Hz at the peak
    double feed(double cnDb) {
        hist_.push_back(cnDb); if (hist_.size() > 8) hist_.pop_front();
        if (cnDb > peak_) peak_ = cnDb; else peak_ -= 0.01;   // forget very slowly so a better peak found later replaces a lucky spike
        cn_ = cnDb;
        if (cnDb < 1) return 0;
        const double d = std::max(0.0, std::min(10.0, peak_ - cnDb));
        return 20.0 - 18.0 * d / 10.0;
    }
    double peakDb() const { return peak_; }
    double belowPeakDb() const { return peak_ - cn_; }
    bool atPeak() const { return peak_ - cn_ < 0.3 && hist_.size() >= 3; }
    int trend() const {     // +1 rising, -1 falling, 0 steady (0.3 dB over the last readings)
        if (hist_.size() < 4) return 0;
        const double a = (hist_[0] + hist_[1]) / 2, b = (hist_[hist_.size() - 1] + hist_[hist_.size() - 2]) / 2;
        return b - a > 0.3 ? 1 : (b - a < -0.3 ? -1 : 0);
    }
    void reset() { hist_.clear(); peak_ = -99; cn_ = -99; }
private:
    std::deque<double> hist_; double peak_ = -99, cn_ = -99;
};

class LnbDrift {
public:
    // expected / measured beacon frequency at the IF (Hz), t in seconds
    void add(double tSec, double expectedIfHz, double measuredIfHz, double loHz) {
        errHz_.push_back({tSec, measuredIfHz - expectedIfHz}); lo_ = loHz;
        if (errHz_.size() > 400) errHz_.pop_front();
    }
    double errorKhz() const { return errHz_.empty() ? 0 : errHz_.back().second / 1e3; }
    double errorPpm() const { return lo_ > 0 && !errHz_.empty() ? errHz_.back().second / lo_ * 1e6 : 0; }
    // slope over the last windowSec, kHz per minute; ok = false with fewer than 4 points or under half the window covered
    double driftKhzPerMin(double windowSec, bool* ok = nullptr) const {
        if (ok) *ok = false;
        if (errHz_.size() < 4) return 0;
        const double t1 = errHz_.back().first;
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0; double t0 = t1;
        for (const auto& p : errHz_) if (p.first >= t1 - windowSec) { t0 = std::min(t0, p.first); sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; n++; }
        if (n < 4 || t1 - t0 < windowSec / 2) return 0;
        const double den = n * sxx - sx * sx;
        if (std::fabs(den) < 1e-9) return 0;
        if (ok) *ok = true;
        return (n * sxy - sx * sy) / den * 60.0 / 1e3;
    }
    bool settled(double windowSec = 120, double maxKhzPerMin = 5) const { bool ok = false; const double d = driftKhzPerMin(windowSec, &ok); return ok && std::fabs(d) < maxKhzPerMin; }
private:
    std::deque<std::pair<double, double>> errHz_; double lo_ = 0;
};

}  // namespace dect2::d40
