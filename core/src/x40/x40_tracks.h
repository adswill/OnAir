// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the intermodulation finder and the spur and image finder: Hann-windowed spectra of a wide capture, averaged into snapshots, the peaks of every snapshot, and tracks
// that follow a peak from snapshot to snapshot (level in dB over the noise, NaN when it is not there). A relation between two tracks (a product follows its parents with a known
// slope in dB per dB) is the evidence both decoders use.
#pragma once
#include "x40_dsp.h"
#include <functional>

namespace dect2::x40 {

struct Track {
    double hz = 0;
    int first = 0;                        // snapshot index of lv[0]
    std::vector<float> lv;                // dB over the noise; NaN: no peak there in that snapshot
    std::vector<float> fq;                // the peak's frequency in each snapshot (NaN when absent)
    float narrow = 0;                     // share of the power within +-8 bins that lies within +-2 bins of the peak (a carrier is near 1, a modulated signal is lower)
    int lastSeen = -1;
    float at(int snap) const { const int i = snap - first; return i >= 0 && i < (int)lv.size() ? lv[(size_t)i] : std::nanf(""); }
    float mean() const { double s = 0; int n = 0; for (float v : lv) if (!std::isnan(v)) { s += v; n++; } return n ? (float)(s / n) : -99.f; }
    float recent() const { for (size_t i = lv.size(); i-- > 0;) if (!std::isnan(lv[i])) return lv[i]; return -99.f; }
    int count() const { int n = 0; for (float v : lv) if (!std::isnan(v)) n++; return n; }
};

// y against x over the snapshots where both exist: slope, correlation, spread of x, number of points
struct Fit { int n = 0; double slope = 0, corr = 0, sdx = 0; };
inline Fit fitLevels(const std::function<float(int)>& x, const std::function<float(int)>& y, int from, int to) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0; int n = 0;
    for (int s = from; s < to; s++) { const float a = x(s), b = y(s); if (std::isnan(a) || std::isnan(b)) continue; sx += a; sy += b; sxx += (double)a * a; syy += (double)b * b; sxy += (double)a * b; n++; }
    Fit f; f.n = n; if (n < 3) return f;
    const double vx = sxx / n - (sx / n) * (sx / n), vy = syy / n - (sy / n) * (sy / n), cv = sxy / n - (sx / n) * (sy / n);
    f.sdx = std::sqrt(std::max(0.0, vx));
    if (vx > 1e-9) f.slope = cv / vx;
    if (vx > 1e-9 && vy > 1e-9) f.corr = cv / std::sqrt(vx * vy);
    return f;
}

// What a fit says about a candidate product: 1 follows the law (slope near 1 with a high correlation), 0 independent, 2 unclear, 3 the inputs did not change enough to tell
inline int judge(const Fit& f) {
    if (f.n < 12 || f.sdx < 1.0) return 3;
    if (f.slope > 0.7 && f.slope < 1.35 && f.corr > 0.8) return 1;
    if (std::fabs(f.slope) < 0.3 || f.corr < 0.4) return 0;
    return 2;
}

class PeakTracker {
public:
    static constexpr int kFft = 16384, kHist = 240;
    // thrDb: a peak must stand this far over the noise; snapSec: snapshot length
    bool configure(double fs, double snapSec, double thrDb) {
        fs_ = fs; thrDb_ = thrDb; frames_ = std::max(1, (int)std::lround(snapSec * fs / kFft));
        win_.resize(kFft); for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() { buf_.clear(); acc_.assign(kFft, 0.f); fill_ = 0; snap_ = 0; tracks_.clear(); noise_ = 1; lastPeaks_ = 0; }
    double binHz() const { return fs_ / kFft; }
    int snapshots() const { return snap_; }
    double noise() const { return noise_; }
    int lastPeaks() const { return lastPeaks_; }
    std::vector<Track>& tracks() { return tracks_; }
    const std::vector<Track>& tracks() const { return tracks_; }
    std::function<void()> onSnapshot;
    // level of the signal that is at frequency hz (Hz from the centre) in the last snapshot's spectrum, in dB over the noise (for a lookup of a place with no peak)
    float lastSpectrumDb(double hz) const { return spectrumDb(hz); }
    double lastPeakDbAt(double hz, double tolBins, double* where = nullptr) const {
        double best = -99, bw = 0; const int k0 = bin(hz), tb = (int)std::ceil(tolBins);
        for (int d = -tb; d <= tb; d++) { const int k = (k0 + d + kFft) % kFft; const double db = 10 * std::log10(std::max(1e-30, (double)last_[k] / noise_)); if (db > best) { best = db; bw = (k < kFft / 2 ? k : k - kFft) * binHz(); } }
        if (where) *where = bw; return best;
    }
    void feed(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) { buf_.push_back(x[i]); if ((int)buf_.size() == kFft) { frame(); buf_.clear(); } }
    }
private:
    int bin(double hz) const { int k = (int)std::lround(hz / binHz()); return (k % kFft + kFft) % kFft; }
    float spectrumDb(double hz) const { return (float)(10 * std::log10(std::max(1e-30, (double)last_[bin(hz)] / noise_))); }
    void frame() {
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = buf_[i] * win_[i];
        fftx(a); for (int k = 0; k < kFft; k++) acc_[k] += std::norm(a[k]);
        if (++fill_ < frames_) return;
        last_.resize(kFft); for (int k = 0; k < kFft; k++) { last_[k] = acc_[k] / (float)fill_; acc_[k] = 0; } fill_ = 0;
        snapshot();
    }
    void snapshot() {
        std::vector<float> s(last_); std::nth_element(s.begin(), s.begin() + kFft / 2, s.end()); noise_ = std::max(1e-20, (double)s[kFft / 2] / (1.0 - 1.0 / (3.0 * frames_)));
        const double thr = noise_ * std::pow(10.0, thrDb_ / 10);
        struct P { double hz; float db; float pw; float nar; }; std::vector<P> pk;
        for (int k = 0; k < kFft; k++) {
            const int j = k < kFft / 2 ? k : k - kFft; if (std::abs(j) <= 3) continue;
            if (last_[k] < thr) continue;
            bool top = true; for (int d = -4; d <= 4 && top; d++) if (d && last_[(k + d + kFft) % kFft] > last_[k]) top = false;
            if (!top) continue;
            const double la = std::log(last_[(k + kFft - 1) % kFft] + 1e-30), lb = std::log(last_[k] + 1e-30), lc = std::log(last_[(k + 1) % kFft] + 1e-30), den = la - 2 * lb + lc;
            const double dl = den < -1e-12 ? 0.5 * (la - lc) / den : 0.0;
            double pw = 0; for (int d = -2; d <= 2; d++) pw += last_[(k + d + kFft) % kFft];
            double pw17 = 0; for (int d = -8; d <= 8; d++) pw17 += last_[(k + d + kFft) % kFft];
            pk.push_back({(j + dl) * binHz(), (float)(10 * std::log10(pw / noise_)), (float)pw, (float)(pw / std::max(pw17, 1e-30))});
        }
        std::sort(pk.begin(), pk.end(), [](const P& a, const P& b) { return a.db > b.db; }); if (pk.size() > 200) pk.resize(200);
        lastPeaks_ = (int)pk.size();
        for (auto& t : tracks_) { t.lv.push_back(std::nanf("")); t.fq.push_back(std::nanf("")); }
        for (const P& p : pk) {
            Track* best = nullptr; double bd = 2.5 * binHz();
            for (auto& t : tracks_) { const double d = std::fabs(t.hz - p.hz); if (d < bd && std::isnan(t.lv.back())) { bd = d; best = &t; } }
            if (!best) { tracks_.push_back(Track()); best = &tracks_.back(); best->hz = p.hz; best->first = snap_; best->lv.assign(1, std::nanf("")); best->fq.assign(1, std::nanf("")); best->narrow = p.nar; }
            best->lv.back() = p.db; best->fq.back() = (float)p.hz; best->narrow = 0.9f * best->narrow + 0.1f * p.nar; best->hz = 0.8 * best->hz + 0.2 * p.hz; best->lastSeen = snap_;
        }
        for (auto& t : tracks_) if ((int)t.lv.size() > kHist) { t.lv.erase(t.lv.begin()); t.fq.erase(t.fq.begin()); t.first++; }
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return snap_ - t.lastSeen > 60; }), tracks_.end());
        snap_++;
        if (onSnapshot) onSnapshot();
    }
    double fs_ = 1, thrDb_ = 12, noise_ = 1; int frames_ = 1, fill_ = 0, snap_ = 0, lastPeaks_ = 0;
    std::vector<float> win_, acc_, last_; std::vector<cf32> buf_; std::vector<Track> tracks_;
};

} // namespace dect2::x40
