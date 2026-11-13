// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst finder for a wide capture: every transmission shorter than a few seconds, wherever it is in the span: a 256-point spectrum every 128 samples (64 us at 4 Msps, 128 us at 2 Msps),
// bins 14 dB over the frame's median are hot, hot bins within one bin of each other form a group, groups are followed from frame to frame (centre within two bins) and a group that lasts
// at least 2 frames is a burst. Each finished burst is a row: frequency, length, bandwidth (width of the group 12 dB under its peak, the widest it got), level. Bursts at the same frequency (within 1.5 bins) are compared:
// four or more with start-to-start times that agree within 5 % are reported as repeating every T s (a telemetry sensor, a key fob, a beacon). Many bursts at once are followed
// separately (up to 16). Resolution: frequency and bandwidth to a bin (fs / 256), length to a frame; a burst shorter than a frame is missed. It lists and times; it does not decode. Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr int kFft = 256, kStep = 128, kMaxActive = 16, kMaxHist = 600;
constexpr double kHotDb = 14.0;

struct Act { double lo = 0, hi = 0, centre = 0; long start = 0, last = 0; int n = 0; double wMax = 0, dbSum = 0; int miss = 0; std::vector<float> cs, ws; };
struct Hist { double t = 0, hz = 0, dur = 0; };

class BurstDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; binHz_ = fs / kFft; dt_ = (double)kStep / fs;
        win_.resize(kFft); for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() override { ring_.assign(kFft, cf32(0, 0)); pos_ = 0; since_ = 0; fr_ = 0; act_.clear(); hist_.clear(); nAll_ = 0; have_ = false; repeats_.clear(); lastEval_ = 0; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) { ring_[pos_] = x[i]; pos_ = (pos_ + 1) % kFft; if (++since_ >= kStep) { since_ = 0; frame(); } }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "listening"; return; }
        const double el = (double)fr_ * dt_;
        t.headline = fmt("%d bursts in %.0f s%s", nAll_, el, repeats_.empty() ? "" : fmt(", %zu repeating source(s)", repeats_.size()).c_str());
        t.facts.push_back({"bursts", fmt("%d in %.0f s (%.2f per second)", nAll_, el, el > 0 ? nAll_ / el : 0.0)});
        for (size_t i = 0; i < repeats_.size() && i < 6; i++) t.facts.push_back({fmt("repeating %zu", i + 1), fmt("%s every %.3f s (%d bursts, spread %.1f %%)", f(repeats_[i].hz).c_str(), repeats_[i].period, repeats_[i].n, repeats_[i].spread * 100)});
        t.facts.push_back({"resolution", fmt("%.2f kHz bins, %.0f us frames", binHz_ / 1000, dt_ * 1e6)});
    }
private:
    struct Rep { double hz = 0, period = 0, spread = 0; int n = 0; bool logged = false; };
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.1f kHz", hz / 1000); }
    void frame() {
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = ring_[(pos_ + (size_t)i) % kFft] * win_[i];
        fftx(a);
        std::vector<float> p(kFft); for (int k = 0; k < kFft; k++) p[k] = std::norm(a[k]);
        std::vector<float> s(p); std::nth_element(s.begin(), s.begin() + kFft / 2, s.end()); const double med = std::max((double)s[kFft / 2], 1e-30);
        const long fr = ++fr_; have_ = true;
        const double thr = med * std::pow(10.0, kHotDb / 10);
        // groups of hot bins in frequency order (bin index -> signed bin number)
        struct G { int lo, hi; double pk; int w = 0; }; std::vector<G> gs;
        for (int jj = -kFft / 2; jj < kFft / 2; jj++) {
            const int k = (jj + kFft) % kFft; if (std::abs(jj) <= 1) continue;
            if (p[k] < thr) continue;
            if (!gs.empty() && jj - gs.back().hi <= 2) { gs.back().hi = jj; gs.back().pk = std::max(gs.back().pk, (double)p[k]); } else gs.push_back({jj, jj, p[k]});
        }
        for (auto& g : gs) { int w = 0; for (int jj = g.lo; jj <= g.hi; jj++) if (p[(jj + kFft) % kFft] >= 0.063 * g.pk) w++; g.w = w; }     // width at 12 dB under the group's peak
        std::vector<char> used(gs.size(), 0);
        for (auto& b : act_) {
            bool hit = false;
            for (size_t g = 0; g < gs.size(); g++) {
                if (used[g]) continue;
                const double c = 0.5 * (gs[g].lo + gs[g].hi) * binHz_;
                if (c >= b.lo - 2 * binHz_ && c <= b.hi + 2 * binHz_ ) {
                    used[g] = 1; hit = true; b.last = fr; b.n++; b.miss = 0; b.lo = std::min(b.lo, gs[g].lo * binHz_); b.hi = std::max(b.hi, gs[g].hi * binHz_);
                    b.centre = 0.5 * (b.lo + b.hi); b.wMax = std::max(b.wMax, gs[g].w * binHz_); b.cs.push_back((float)(0.5 * (gs[g].lo + gs[g].hi) * binHz_)); b.ws.push_back((float)(gs[g].w * binHz_)); b.dbSum += 10 * std::log10(gs[g].pk / med); break;
                }
            }
            if (!hit) b.miss++;
        }
        for (size_t g = 0; g < gs.size(); g++) if (!used[g] && (int)act_.size() < kMaxActive) {
            Act b; b.lo = gs[g].lo * binHz_; b.hi = gs[g].hi * binHz_; b.centre = 0.5 * (b.lo + b.hi); b.start = b.last = fr; b.n = 1; b.wMax = gs[g].w * binHz_; b.cs.push_back((float)(0.5 * (gs[g].lo + gs[g].hi) * binHz_)); b.ws.push_back((float)(gs[g].w * binHz_)); b.dbSum = 10 * std::log10(gs[g].pk / med); act_.push_back(b);
        }
        for (size_t i = 0; i < act_.size();) { if (act_[i].miss > 2) { finish(act_[i]); act_.erase(act_.begin() + (long)i); } else i++; }
        // a burst that goes on for 3 s is not a burst: let it go (it is a carrier; the occupancy monitor is the tool for those)
        for (size_t i = 0; i < act_.size();) { if ((fr - act_[i].start) * dt_ > 3.0) act_.erase(act_.begin() + (long)i); else i++; }
        if (hist_.size() >= 4 && fr_ - lastEval_ > (long)(0.5 / dt_)) { lastEval_ = fr_; evaluate(); }
    }
    static double medianOf(std::vector<float> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
    void finish(const Act& a) {
        if (a.n < 2) return;
        Act b = a; b.centre = medianOf(a.cs); b.wMax = medianOf(a.ws);      // the edges of a burst splatter: the middle frames say what it is
        const double dur = std::max(dt_, (b.n - 0.5) * dt_);            // frames that saw it, minus the part of the first and last frame it only touched
        const double t0 = b.start * dt_;
        nAll_++;
        hist_.push_back({t0, b.centre, dur}); if (hist_.size() > (size_t)kMaxHist) hist_.pop_front();
        out->good((float)(b.dbSum / b.n), 0);
        out->row("Burst", f(b.centre), fmt("at %.3f s, length %.2f ms, %s (%.1f kHz wide), %.0f dB over the noise", t0, dur * 1000, b.wMax <= 3.5 * binHz_ ? "narrow" : "wide", std::max(b.wMax, binHz_) / 1000, b.dbSum / b.n), (float)(b.dbSum / b.n));
    }
    void evaluate() {
        std::vector<Rep> reps; std::vector<char> used(hist_.size(), 0);
        for (size_t i = 0; i < hist_.size(); i++) {
            if (used[i]) continue;
            std::vector<double> ts; ts.push_back(hist_[i].t); used[i] = 1;
            for (size_t j = i + 1; j < hist_.size(); j++) if (!used[j] && std::fabs(hist_[j].hz - hist_[i].hz) < 1.5 * binHz_) { ts.push_back(hist_[j].t); used[j] = 1; }
            if (ts.size() < 4) continue;
            std::sort(ts.begin(), ts.end()); std::vector<double> iv; for (size_t k = 1; k < ts.size(); k++) iv.push_back(ts[k] - ts[k - 1]);
            std::vector<double> s(iv); std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end()); const double m = s[s.size() / 2];
            int good = 0; for (double v : iv) if (std::fabs(v - m) < 0.05 * m) good++;
            if (m > 0.002 && good >= (int)(0.7 * iv.size()) && good >= 3) { Rep r; r.hz = hist_[i].hz; r.period = m; r.n = (int)ts.size(); double sd = 0; int c = 0; for (double v : iv) if (std::fabs(v - m) < 0.05 * m) { sd += (v - m) * (v - m); c++; } r.spread = std::sqrt(sd / std::max(c, 1)) / m; reps.push_back(r); }
        }
        for (auto& r : reps) {
            bool known = false; for (auto& o : repeats_) if (std::fabs(o.hz - r.hz) < 1.5 * binHz_) { known = true; r.logged = o.logged; if (!o.logged) { } }
            if (!r.logged) { r.logged = true; out->row("Repeating", f(r.hz), fmt("every %.3f s (%d bursts, spread %.1f %%)", r.period, r.n, r.spread * 100), 0.f); }
            (void)known;
        }
        repeats_ = reps;
    }
    double fs_ = 1, binHz_ = 1, dt_ = 1, centre_ = 0; size_t pos_ = 0; int since_ = 0, nAll_ = 0; long fr_ = 0, lastEval_ = 0; bool have_ = false;
    std::vector<float> win_; std::vector<cf32> ring_; std::vector<Act> act_; std::deque<Hist> hist_; std::vector<Rep> repeats_;
};
}

std::unique_ptr<Decoder> makeBurst() { return std::make_unique<BurstDec>(); }

} // namespace dect2::x40
