// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spur and image finder for the user's own SDR: from one wide capture it sorts what is not a real signal. The DC spike of the radio (the bins around 0 Hz); for each strong signal its
// mirror image at -f (the signature of an IQ gain and phase mismatch: the image rejection in dB, and the mismatch that would cause it), and its second and third harmonics folded back
// into the span; and constant spurs at simple fractions of the sample rate (fs / 16 steps), common converter spurs. A peak counts as an image or harmonic only if its level follows
// the signal's in 0.25 s snapshots (slope 1 for the image, 1 against 2 x or 3 x the signal's level for the harmonics, correlation 0.8); with a steady signal it says "cannot tell" and
// the way to settle it is to change the gain or the tuning and see whether the peak moves. A real signal at -f is told apart only by not following. The spur list is of this radio at
// this setting (gain, sample rate, tuning): it changes with them. Receive only.
#include "x40_tracks.h"

namespace dect2::x40 {

namespace {
constexpr double kSnapSec = 0.25, kThrDb = 8.0, kTolBins = 2.5;
constexpr int kMinSnaps = 16, kEvery = 8;

struct Find { int kind = 0; double hz = 0, fromHz = 0; float below = 0; int cls = 3; Fit fit; int stable = 0, reported = -1; double ratio = 0; };   // kind 0 image, 1 2nd harmonic, 2 3rd harmonic, 3 fixed spur

class SpurDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; if (!tr_.configure(fs, kSnapSec, kThrDb)) return false; tr_.onSnapshot = [this]() { snapshot(); }; reset(); return true; }
    void reset() override { tr_.reset(); finds_.clear(); signals_ = 0; dcDb_ = -99; have_ = false; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { tr_.feed(x, n); }
    void fill(X40Telemetry& t) const override {
        if (tr_.snapshots() < kMinSnaps) { t.headline = "collecting spectra"; return; }
        int nImg = 0, nHar = 0, nSp = 0; for (auto& f : finds_) { if (f.kind == 0 && f.cls != 0) nImg++; else if (f.kind == 1 || f.kind == 2) { if (f.cls != 0) nHar++; } else if (f.kind == 3) nSp++; }
        t.headline = fmt("%d signals; DC spike %s; %d images, %d harmonics, %d fixed spurs", signals_, dcDb_ > 6 ? fmt("%.0f dB over the noise", dcDb_).c_str() : "none", nImg, nHar, nSp);
        t.facts.push_back({"DC spike", dcDb_ > 6 ? fmt("%.0f dB over the noise (the radio's own offset)", dcDb_) : std::string("none seen")});
        t.facts.push_back({"signals", fmt("%d strong signals tracked", signals_)});
        for (size_t i = 0; i < finds_.size() && i < 8; i++) t.facts.push_back({fmt("finding %zu", i + 1), describe(finds_[i])});
        t.facts.push_back({"snapshots", fmt("%d of %.2f s", tr_.snapshots(), kSnapSec)});
    }
private:
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.1f kHz", hz / 1000); }
    static const char* clsText(int c) { return c == 1 ? "follows the signal" : c == 0 ? "does not follow: a signal of its own" : c == 2 ? "unclear" : "signal steady, cannot tell"; }
    std::string describe(const Find& x) const {
        if (x.kind == 3) return fmt("fixed spur at %s (%+.0f kHz, a multiple of fs / 16), %.0f dB over the noise, steady: a converter spur, or a real signal that happens to be there", f(x.hz).c_str(), x.hz / 1000, x.below);
        const char* nm = x.kind == 0 ? "image" : x.kind == 1 ? "2nd harmonic" : "3rd harmonic";
        std::string s = fmt("%s of %s at %s, %.0f dB below it: %s", nm, f(x.fromHz).c_str(), f(x.hz).c_str(), x.below, clsText(x.cls));
        if (x.kind == 0 && x.cls != 0) { const double e = 2 * x.ratio; s += fmt("; image rejection %.0f dB, the same as a gain mismatch of %.2f dB or a phase mismatch of %.1f degrees", x.below, 8.686 * e, 57.3 * e); }
        return s;
    }
    void snapshot() {
        const int s = tr_.snapshots();
        if (s < kMinSnaps || (s - kMinSnaps) % kEvery != 0) return;
        have_ = true; evaluate(s);
    }
    double wrap(double hz) const { while (hz > fs_ / 2) hz -= fs_; while (hz <= -fs_ / 2) hz += fs_; return hz; }
    void evaluate(int snap) {
        auto& T = tr_.tracks();
        dcDb_ = (float)tr_.lastPeakDbAt(0, 2.0);
        std::vector<int> idx; for (size_t i = 0; i < T.size(); i++) if (T[i].lastSeen >= snap - 3 && T[i].count() >= (int)(0.6 * std::min<int>((int)T[i].lv.size(), snap)) && T[i].mean() > 20.f) idx.push_back((int)i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return T[(size_t)a].mean() > T[(size_t)b].mean(); }); if (idx.size() > 6) idx.resize(6);
        signals_ = (int)idx.size();
        const double tol = kTolBins * tr_.binHz();
        std::vector<Find> found; std::vector<char> used(T.size(), 0);
        for (int a : idx) {
            const Track& A = T[(size_t)a];
            for (int kind = 0; kind < 3; kind++) {
                const double fp = kind == 0 ? -A.hz : wrap((kind + 1) * A.hz);
                if (std::fabs(fp - A.hz) < 5 * tr_.binHz() || std::fabs(fp) < 5 * tr_.binHz()) continue;
                for (size_t q = 0; q < T.size(); q++) {
                    if ((int)q == a || std::fabs(T[q].hz - fp) > tol || T[q].lastSeen < snap - 3 || T[q].count() < 6) continue;
                    if (T[q].mean() > A.mean() - 3.f) continue;
                    const Track& Q = T[q]; const double w = kind == 0 ? 1.0 : kind + 1.0;
                    Find x; x.kind = kind; x.hz = Q.hz; x.fromHz = A.hz; x.below = A.mean() - Q.mean(); x.ratio = std::pow(10.0, -x.below / 20);
                    x.fit = fitLevels([&](int k) { return (float)(w * A.at(k)); }, [&](int k) { return Q.at(k); }, std::max(0, snap - 200), snap);
                    x.cls = judge(x.fit);
                    found.push_back(x); used[q] = 1;
                }
            }
        }
        // constant spurs at multiples of fs / 16 that nothing above explains
        for (size_t q = 0; q < T.size(); q++) {
            if (used[q] || T[q].lastSeen < snap - 3 || T[q].count() < (int)(0.8 * std::min<int>((int)T[q].lv.size(), snap)) || (int)T[q].lv.size() < 12) continue;
            const double step = fs_ / 16, k = std::round(T[q].hz / step);
            if (k == 0 || std::fabs(T[q].hz - k * step) > tol) continue;
            const Fit fl = fitLevels([&](int) { return 0.f; }, [&](int) { return 0.f; }, 0, 1); (void)fl;
            Find x; x.kind = 3; x.hz = T[q].hz; x.below = T[q].mean(); x.cls = 1; found.push_back(x);
        }
        // report: once a verdict has held for two evaluations
        for (auto& x : found) {
            for (auto& o : finds_) if (o.kind == x.kind && std::fabs(o.hz - x.hz) < 30.0 && std::fabs(o.fromHz - x.fromHz) < 30.0) { x.stable = o.cls == x.cls ? o.stable + 1 : 0; x.reported = o.reported; }
            if (x.stable >= 1 && x.reported != x.cls) { out->row(x.kind == 0 ? "Image" : x.kind == 3 ? "Spur" : "Harmonic", f(x.hz), describe(x), (float)(x.below)); x.reported = x.cls; }
        }
        if (dcDb_ > 6 && !dcReported_) { dcReported_ = true; out->row("DC", "0 Hz", fmt("DC spike %.0f dB over the noise", dcDb_), dcDb_); }
        if (dcDb_ <= 6) dcReported_ = false;
        finds_ = found;
        bool any = dcDb_ > 6; for (auto& x : finds_) any |= x.cls != 0;
        if (any) out->good(0, 0); else out->energy();
    }
    PeakTracker tr_; double fs_ = 1, centre_ = 0; int signals_ = 0; float dcDb_ = -99; bool have_ = false, dcReported_ = false; std::vector<Find> finds_;
};
}

std::unique_ptr<Decoder> makeSpur() { return std::make_unique<SpurDec>(); }

} // namespace dect2::x40
