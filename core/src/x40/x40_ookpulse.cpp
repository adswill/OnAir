// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OOK / ASK pulse analyser (key fobs, remote switches, sensors, 300 - 920 MHz): the same job as the "analyse" mode of rtl_433, from the envelope of the signal in the 90 kHz around the tuned frequency:
// the envelope is sliced with an adaptive threshold (40 % between the noise floor and the recent peak, with hysteresis) into pulses and gaps (4 us resolution at the default 250 ksps); a frame is a run of pulses
// whose gaps stay under 5 ms (and under 12 times the median pulse width); a frame needs 8 pulses. The pulse widths, gap widths and pulse + gap periods are clustered (1-D, a new cluster where
// neighbouring sorted values differ by 1.5 times or more). The modulation is then named: PWM (two pulse widths, one bit period: the bit is the pulse width: short = 0, long = 1; the common PT2262 / EV1527 pattern is
// 1:3 within one period), PPM (one pulse width, two gaps: the bit is the gap: short = 0, long = 1), Manchester (pulses and gaps of one or two half-bit times: decoded as 10 = 1, 01 = 0 from the first rising edge; the
// polarity and the start of the first bit are assumptions), or "PCM / NRZ-like" (widths are whole multiples of one time T: the runs are listed with T). Per frame a row: modulation, number of bits, bits in hex and binary, timings in us;
// repeats of the same frame in a train are counted. It names the pattern, it does not know the product: short = 0 and long = 1 is a convention, the real protocol may invert it and has its own preamble, address and checksum.
// Receive only.
#include "x40_dsp.h"
#include <deque>
#include <map>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 200000, kCut = 90000, kFrameGapSec = 0.005, kSplit = 1.5;
constexpr int kMinPulses = 8;

struct Pulse { double t0 = 0, w = 0; };

class OokDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); reset(); return true;
    }
    void reset() override { dec_.reset(); n_ = 0; env_ = 0; nf_ = 0; pk_ = 0; high_ = false; pulses_.clear(); lastEdge_ = 0; t_ = 0; frames_ = 0; have_ = false; recent_.clear(); skip_ = (size_t)(0.1 * rate_); lastBits_.clear(); repeats_ = 0; res_ = Res(); lastFrameEnd_ = -1e9; sinceNf_ = 0; sm_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            if (skip_) { skip_--; return; }
            t_ += 1.0 / rate_; n_++;
            const float e = std::abs(a); sm_ += (e - sm_) * 0.4f;
            sample(sm_);
        });
    }
    void fill(X40Telemetry& t) const override {
        t.ookBursts.assign(scatter_.begin(), scatter_.end());
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = frames_ ? fmt("%d frames seen, none with a recognised pattern yet", frames_) : "no OOK frame yet"; return; }
        t.headline = fmt("%s, %d bits: %s, repeated %d time(s)", res_.mod.c_str(), res_.nbits, res_.hex.c_str(), repeats_);
        t.facts.push_back({"modulation", res_.mod});
        t.facts.push_back({"bits", fmt("%d: %s (hex %s)", res_.nbits, res_.bin.c_str(), res_.hex.c_str())});
        t.facts.push_back({"timing", res_.timing});
        t.facts.push_back({"frame", fmt("%d pulses, %.1f ms long", res_.pulses, res_.durMs)});
        t.facts.push_back({"repeats of this frame", fmt("%d in the last train", repeats_)});
        t.facts.push_back({"frames seen", fmt("%d", frames_)});
    }
private:
    struct Res { bool ok = false; std::string mod, bin, hex, timing; int nbits = 0, pulses = 0; double durMs = 0; };
    void sample(float e) {
        recent_.push_back(e); if (recent_.size() > (size_t)(0.5 * rate_)) recent_.pop_front();
        pk_ = std::max((double)e, pk_ * (1.0 - 4.0 / rate_));                          // the peak decays in about 0.25 s
        if (++sinceNf_ >= (int)(0.01 * rate_)) { sinceNf_ = 0; std::vector<float> v; v.reserve(recent_.size() / 8 + 1); for (size_t i = 0; i < recent_.size(); i += 8) v.push_back(recent_[i]); if (v.size() > 50) { std::nth_element(v.begin(), v.begin() + (long)(v.size() / 10), v.end()); nf_ = v[v.size() / 10]; } }
        have_ = true;
        const double span = pk_ - nf_;
        const bool sig = span > 6.0 * std::max(nf_, 1e-9) && span > 0;
        const double thrHi = nf_ + 0.45 * span, thrLo = nf_ + 0.35 * span;
        const double now = t_;
        if (!high_ && sig && e > thrHi) { high_ = true; const double gap = now - lastEdge_; if (!pulses_.empty() && gap > frameTimeout()) finishFrame(gap); curStart_ = now; if (pulses_.empty()) frameStart_ = now; }
        else if (high_ && (e < thrLo || !sig)) { high_ = false; lastEdge_ = now; const double w = now - curStart_; if (w > 8.0 / rate_ * 2) pulses_.push_back({curStart_, w}); }
        if (!high_ && !pulses_.empty() && now - lastEdge_ > frameTimeout()) finishFrame(now - lastEdge_);
    }
    double frameTimeout() const { if (pulses_.size() < 4) return kFrameGapSec; std::vector<double> w; for (auto& p : pulses_) w.push_back(p.w); std::nth_element(w.begin(), w.begin() + (long)(w.size() / 2), w.end()); return std::min(kFrameGapSec, std::max(0.0012, 12 * w[w.size() / 2])); }
    // clusters of 1-D values: sorted, a new cluster where the ratio to the previous exceeds kSplit
    static std::vector<std::pair<double, int>> clusters(std::vector<double> v) {
        std::vector<std::pair<double, int>> c; if (v.empty()) return c; std::sort(v.begin(), v.end());
        double sum = v[0]; int n = 1; double last = v[0];
        for (size_t i = 1; i < v.size(); i++) { if (v[i] > last * kSplit) { c.push_back({sum / n, n}); sum = 0; n = 0; } sum += v[i]; n++; last = v[i]; }
        c.push_back({sum / n, n}); return c;
    }
    static std::string hexOf(const std::string& bin) { std::string h; for (size_t i = 0; i < bin.size(); i += 4) { int v = 0; for (size_t j = 0; j < 4; j++) v = v * 2 + (i + j < bin.size() ? bin[i + j] - '0' : 0); h += "0123456789ABCDEF"[v]; } return h; }
    void finishFrame(double trail) {
        std::vector<Pulse> P; P.swap(pulses_); frames_++; lastFrameEndT_ = lastEdge_;
        if (P.size() >= 4) {
            std::vector<std::pair<float, float>> sb;
            for (size_t i = 0; i < P.size(); i++) sb.push_back({(float)(P[i].w * 1e6), (float)((i + 1 < P.size() ? P[i + 1].t0 - (P[i].t0 + P[i].w) : trail) * 1e6)});
            scatter_.push_back(std::move(sb)); if (scatter_.size() > 64) scatter_.pop_front();
        }
        if ((int)P.size() < kMinPulses) return;
        // drop a long sync gap / first pulse: the gaps between pulses
        std::vector<double> w, g; for (size_t i = 0; i < P.size(); i++) { w.push_back(P[i].w); if (i + 1 < P.size()) g.push_back(P[i + 1].t0 - (P[i].t0 + P[i].w)); }
        const double medG = [&] { auto s = g; std::nth_element(s.begin(), s.begin() + (long)(s.size() / 2), s.end()); return s[s.size() / 2]; }();
        // the sync gap(s): gaps over 6 times the median gap are not data
        std::vector<double> gd; for (double v : g) if (v < 6 * medG) gd.push_back(v);
        const auto cw = clusters(w), cg = clusters(gd);
        std::vector<double> per; for (size_t i = 0; i + 1 < P.size(); i++) { const double gp = g[i]; if (gp < 6 * medG) per.push_back(P[i].w + gp); }
        const auto cp = clusters(per);
        // real timings sit in tight clusters; noise gives widths all over the place
        auto tight = [](std::vector<double> v, const std::vector<std::pair<double, int>>& c) { std::sort(v.begin(), v.end()); size_t i = 0; for (auto& cl : c) { if (i >= v.size()) break; const double lo = v[i]; const size_t e = std::min(v.size(), i + (size_t)cl.second); const double hi = v[e - 1]; if (hi > 1.4 * lo) return false; i = e; } return true; };
        if (!tight(w, cw) || !tight(gd, cg)) return;
        const bool periodsTight = !per.empty() && *std::max_element(per.begin(), per.end()) < 1.25 * *std::min_element(per.begin(), per.end());
        Res r; r.pulses = (int)P.size(); r.durMs = (P.back().t0 + P.back().w - P[0].t0) * 1000;
        auto us = [](double v) { return v * 1e6; };
        std::string bin;
        if (cw.size() == 2 && periodsTight && cw[1].first / cw[0].first >= 1.8) {
            r.mod = "PWM (pulse width, fixed bit period)";
            const bool syncLast = trail >= 12.0 * P.back().w && P.back().w < 0.5 * (cw[0].first + cw[1].first);     // a short pulse followed by a very long gap is the sync pulse of the next frame (PT2262 style)
            for (size_t i = 0; i < P.size(); i++) { if (i + 1 < P.size() && g[i] >= 6 * medG) continue; if (syncLast && i + 1 == P.size()) break; bin += P[i].w > 0.5 * (cw[0].first + cw[1].first) ? '1' : '0'; }
            r.timing = fmt("short pulse %.0f us, long pulse %.0f us, bit period %.0f us", us(cw[0].first), us(cw[1].first), us(cp[0].first));
        } else if (cw.size() == 1 && cg.size() == 2 && cg[1].first / cg[0].first >= 1.8) {
            r.mod = "PPM (pulse position, the gap is the bit)";
            for (size_t i = 0; i + 1 < P.size(); i++) { if (g[i] >= 6 * medG) continue; bin += g[i] > 0.5 * (cg[0].first + cg[1].first) ? '1' : '0'; }
            r.timing = fmt("pulse %.0f us, short gap %.0f us, long gap %.0f us", us(cw[0].first), us(cg[0].first), us(cg[1].first));
        } else if (cw.size() <= 2 && cg.size() <= 2 && !cw.empty() && !cg.empty() && ((cw.size() == 1 || std::fabs(cw[1].first / cw[0].first - 2.0) < 0.35) && (cg.size() == 1 || std::fabs(cg[1].first / cg[0].first - 2.0) < 0.35)) && (cw.size() == 2 || cg.size() == 2) && std::fabs(cw[0].first / cg[0].first - 1.0) < 0.35) {
            const double T = 0.5 * (cw[0].first + cg[0].first);
            r.mod = "Manchester (half-bit " + fmt("%.0f", us(T)) + " us)";
            std::string half; for (size_t i = 0; i < P.size(); i++) { half.append((size_t)std::lround(P[i].w / T), '1'); if (i + 1 < P.size()) { if (g[i] >= 6 * medG) break; half.append((size_t)std::lround(g[i] / T), '0'); } }
            for (size_t i = 0; i + 1 < half.size(); i += 2) { if (half[i] == half[i + 1]) break; bin += half[i] == '1' ? '1' : '0'; }
            r.timing = fmt("half-bit %.0f us, bit rate %.0f bit/s", us(T), 1.0 / (2 * T));
        } else {
            // PCM / NRZ-like: all widths whole multiples of the smallest
            std::vector<double> all = w; for (double v : gd) all.push_back(v); const double T = [&] { auto s = all; std::sort(s.begin(), s.end()); return s[s.size() / 10]; }();
            double err = 0; for (double v : all) { const double q = v / T; err += std::fabs(q - std::round(q)); } err /= (double)all.size();
            if (T < 20e-6 || err > 0.22) return;
            r.mod = fmt("PCM / NRZ-like (unit %.0f us)", us(T));
            for (size_t i = 0; i < P.size(); i++) { bin.append((size_t)std::max(1L, std::lround(P[i].w / T)), '1'); if (i + 1 < P.size()) { if (g[i] >= 6 * medG) break; bin.append((size_t)std::max(1L, std::lround(g[i] / T)), '0'); } }
            r.timing = fmt("unit %.0f us (%.0f baud if one unit is a bit)", us(T), 1.0 / T);
        }
        if (bin.size() < 6) return;
        r.nbits = (int)bin.size(); r.bin = bin.size() > 96 ? bin.substr(0, 96) + "..." : bin; r.hex = hexOf(bin.substr(0, 96)); r.ok = true;
        if (bin == lastBits_ && t_ - lastFrameEnd_ < 0.5) repeats_++; else repeats_ = 1;
        lastBits_ = bin; lastFrameEnd_ = t_; res_ = r;
        out->good(0, 0);
        out->row("Frame", fmt("%s %d bits", r.mod.substr(0, r.mod.find(' ')).c_str(), r.nbits), fmt("%s (hex %s); %s; %d pulses in %.1f ms", r.bin.c_str(), r.hex.c_str(), r.timing.c_str(), r.pulses, r.durMs), (float)(pk_));
    }
    Decimator dec_; double rate_ = 1, t_ = 0, env_ = 0, nf_ = 0, pk_ = 0, curStart_ = 0, frameStart_ = 0, lastEdge_ = 0, lastFrameEnd_ = -1e9, lastFrameEndT_ = 0; long n_ = 0; int frames_ = 0, repeats_ = 0, sinceNf_ = 0; size_t skip_ = 0; float sm_ = 0;
    bool high_ = false, have_ = false; std::deque<float> recent_; std::deque<std::vector<std::pair<float, float>>> scatter_; std::vector<Pulse> pulses_; std::string lastBits_; Res res_;
};
}

std::unique_ptr<Decoder> makeOokPulse() { return std::make_unique<OokDec>(); }

} // namespace dect2::x40
