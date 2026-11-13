// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Intermodulation finder for a wide capture (the radio's whole span): finds the strong signals, then looks for peaks at the frequencies where two of them would produce
// intermodulation (2 f1 - f2, 2 f2 - f1, f2 - f1) and tests whether such a peak behaves like a product. A product follows its parents by a law: a third-order product moves 2 dB for
// every dB of f1 and 1 dB for every dB of f2, so when the signals fade and swell the product has to move with them (slope 1 against that law); a real transmitter on the same
// frequency does not care. The slope is read from 0.25 s snapshots over up to a minute, and only when the inputs changed by at least 1 dB (standard deviation); with steady inputs the
// finder says it cannot tell, and the way to settle it is to switch on the radio's attenuator and see whether the peak falls three times faster than the signals. It says
// "follows the intermod law", it does not say whether the product arises in the radio or in a transmitter: a product made in the air or in a rusty joint follows the same law. Receive only.
#include "x40_tracks.h"

namespace dect2::x40 {

namespace {
constexpr double kSnapSec = 0.25, kThrDb = 10.0, kTolBins = 2.5;
constexpr int kMinSnaps = 16, kEvery = 8;

struct Prod { double hz = 0, fa = 0, fb = 0; float lvA = 0; int a = -1, b = -1, kind = 0; int cls = 3; Fit fit; float below = 0; int stable = 0, reported = -1; };   // kind: 0 2fa-fb, 1 2fb-fa, 2 fb-fa

class IntermodDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; if (!tr_.configure(fs, kSnapSec, kThrDb)) return false; tr_.onSnapshot = [this]() { snapshot(); }; reset(); return true; }
    void reset() override { tr_.reset(); prods_.clear(); inputs_.clear(); evals_ = 0; have_ = false; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { tr_.feed(x, n); }
    void fill(X40Telemetry& t) const override {
        if (tr_.snapshots() < kMinSnaps) { t.headline = "collecting spectra"; return; }
        int nIm = 0, nInd = 0, nUn = 0;
        for (auto& p : prods_) { if (p.cls == 1) nIm++; else if (p.cls == 0) nInd++; else nUn++; }
        t.headline = inputs_.empty() ? "no strong signals" : prods_.empty() ? fmt("%zu strong signals, nothing at an intermodulation frequency", inputs_.size()) : fmt("%zu strong signals, %zu peaks at intermodulation frequencies: %d follow the law, %d are independent, %d cannot be told", inputs_.size(), prods_.size(), nIm, nInd, nUn);
        for (size_t i = 0; i < inputs_.size() && i < 4; i++) t.facts.push_back({fmt("signal %zu", i + 1), fmt("%s, %.0f dB over the noise", f(inputs_[i].first).c_str(), inputs_[i].second)});
        for (size_t i = 0; i < prods_.size() && i < 6; i++) {
            const Prod& p = prods_[i];
            t.facts.push_back({fmt("peak %zu", i + 1), fmt("%s: %s", f(p.hz).c_str(), verdict(p).c_str())});
        }
        t.facts.push_back({"snapshots", fmt("%d of %.2f s", tr_.snapshots(), kSnapSec)});
    }
private:
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.1f kHz", hz / 1000); }
    std::string formula(const Prod& p) const {
        const double fa = p.fa, fb = p.fb;
        return p.kind == 0 ? fmt("2 x %s - %s", f(fa).c_str(), f(fb).c_str()) : p.kind == 1 ? fmt("2 x %s - %s", f(fb).c_str(), f(fa).c_str()) : fmt("%s - %s", f(fb).c_str(), f(fa).c_str());
    }
    std::string verdict(const Prod& p) const {
        const char* c = p.cls == 1 ? "follows the intermod law" : p.cls == 0 ? "independent of the signals (a transmitter of its own)" : p.cls == 2 ? "unclear" : "the signals stayed steady, cannot tell";
        return fmt("%s, %.0f dB below the weaker signal, %s%s", formula(p).c_str(), p.below, c, p.cls == 3 ? "" : fmt(" (slope %.2f, correlation %.2f)", p.fit.slope, p.fit.corr).c_str());
    }
    void snapshot() {
        const int s = tr_.snapshots();
        if (s < kMinSnaps || (s - kMinSnaps) % kEvery != 0) return;
        have_ = true; evaluate(s);
    }
    void evaluate(int snap) {
        auto& T = tr_.tracks();
        // the inputs: the strongest tracks that were there most of the time
        std::vector<int> idx; for (size_t i = 0; i < T.size(); i++) if (T[i].count() >= (int)(0.6 * std::min<int>((int)T[i].lv.size(), snap)) && T[i].lastSeen >= snap - 3 && T[i].mean() > 20.f) idx.push_back((int)i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return T[(size_t)a].mean() > T[(size_t)b].mean(); }); if (idx.size() > 8) idx.resize(8);
        inputs_.clear(); for (int i : idx) inputs_.push_back({T[(size_t)i].hz, T[(size_t)i].mean()});
        const double tol = kTolBins * tr_.binHz(), lim = fs_ / 2 * 0.98;
        std::vector<Prod> found;
        for (size_t i = 0; i < idx.size(); i++) for (size_t j = 0; j < idx.size(); j++) {
            if (i == j) continue;
            const int a = idx[i], b = idx[j]; const double fa = T[(size_t)a].hz, fb = T[(size_t)b].hz; if (fa >= fb) continue;
            for (int kind = 0; kind < 3; kind++) {
                const double fp = kind == 0 ? 2 * fa - fb : kind == 1 ? 2 * fb - fa : fb - fa;
                if (std::fabs(fp) > lim || std::fabs(fp) < 5 * tr_.binHz()) continue;
                for (size_t q = 0; q < T.size(); q++) {
                    if ((int)q == a || (int)q == b || T[q].lastSeen < snap - 3 || T[q].count() < 6) continue;
                    if (std::fabs(T[q].hz - fp) > tol) continue;
                    const float weaker = std::min(T[(size_t)a].mean(), T[(size_t)b].mean());
                    if (T[q].mean() > weaker - 3.f) continue;                       // a product is weaker than its parents
                    Prod p; p.hz = T[q].hz; p.a = a; p.b = b; p.kind = kind; p.fa = fa; p.fb = fb; p.lvA = weaker; p.below = weaker - T[q].mean();
                    const double wa = kind == 0 ? 2 : kind == 1 ? 1 : 1, wb = kind == 0 ? 1 : kind == 1 ? 2 : 1;
                    const Track& A = T[(size_t)a]; const Track& B = T[(size_t)b]; const Track& Q = T[q];
                    p.fit = fitLevels([&](int k) { return (float)(wa * A.at(k) + wb * B.at(k)); }, [&](int k) { return Q.at(k); }, std::max(0, snap - 200), snap);
                    p.cls = judge(p.fit);
                    // the same peak may match several pairs: keep the pair whose parents are strongest
                    bool dup = false; for (auto& o : found) if (std::fabs(o.hz - p.hz) < 30.0) { dup = true; if (p.below > o.below) o = p; }
                    if (!dup) found.push_back(p);
                }
            }
        }
        std::sort(found.begin(), found.end(), [](const Prod& x, const Prod& y) { return x.below < y.below; });
        // log a verdict once it has held for two evaluations
        for (auto& p : found) {
            for (auto& o : prods_) if (std::fabs(o.hz - p.hz) < 30.0) { p.stable = o.cls == p.cls ? o.stable + 1 : 0; p.reported = o.reported; }
            if (p.stable >= 1 && p.reported != p.cls) {
                out->row("Product", f(p.hz), verdict(p), p.lvA);
                p.reported = p.cls;
            }
        }
        prods_ = found;
        if (!prods_.empty()) { bool any = false; for (auto& p : prods_) any |= p.cls == 1; if (any) out->good(0, 0); else out->energy(); } else if (!inputs_.empty()) out->energy();
    }
    PeakTracker tr_; double fs_ = 1, centre_ = 0; int evals_ = 0; bool have_ = false; std::vector<Prod> prods_; std::vector<std::pair<double, float>> inputs_;
};
}

std::unique_ptr<Decoder> makeIntermod() { return std::make_unique<IntermodDec>(); }

} // namespace dect2::x40
