// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite beacon Doppler curve logger (a steady carrier within +-14 kHz of the tuned frequency: a cubesat or weather-satellite beacon, 100 MHz - 3 GHz). Every window (about
// 0.7 s) the carrier is found and its frequency logged; every 10 s a row. From the readings of a pass the curve is fitted with the flyby model
//     f(t) = fc - A (t - t0) / sqrt((t - t0)^2 + B^2)
// (a straight, constant-speed track past the observer): fc is the carrier with no Doppler (the transmitter's frequency plus this radio's error), t0 the time of closest approach
// (TCA), A the Doppler the satellite approaches at far away and A / B the steepest slope, at TCA. With the tuned frequency known, A gives the speed (c A / f) and B x speed a
// closest distance. The model is a straight line: a real orbit curves, so the speed and distance are good to some ten percent, TCA and the steepest slope are the robust
// numbers; the shape is poorer at low elevation. A pass ends when the carrier has been gone 20 s; the fit is made then, and every 30 s during a pass. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 40000, kCut = 15000, kSearchHz = 14000, kGapEndSec = 20, kFitEverySec = 30;
constexpr double kC = 299792458.0;

struct Rd { double t, hz, db; };
struct Curve { bool ok = false; double fc = 0, A = 0, t0 = 0, B = 0, rms = 0; };

// for given t0 and B, the linear least squares of fc and A; returns the sum of squares
static double solve(const std::vector<Rd>& r, double t0, double B, double& fc, double& A) {
    double sg = 0, sgg = 0, sy = 0, sgy = 0; const size_t n = r.size();
    for (auto& d : r) { const double u = d.t - t0, g = -u / std::sqrt(u * u + B * B); sg += g; sgg += g * g; sy += d.hz; sgy += g * d.hz; }
    const double den = n * sgg - sg * sg; if (std::fabs(den) < 1e-12) { fc = sy / n; A = 0; return 1e30; }
    A = (n * sgy - sg * sy) / den; fc = (sy - A * sg) / n;
    double e = 0; for (auto& d : r) { const double u = d.t - t0, g = -u / std::sqrt(u * u + B * B); const double x = d.hz - fc - A * g; e += x * x; }
    return e;
}
static Curve fitCurve(const std::vector<Rd>& r) {
    Curve c; if (r.size() < 12) return c;
    const double ta = r.front().t, tb = r.back().t;
    double best = 1e30, bt0 = 0, bB = 10, bfc = 0, bA = 0;
    for (double t0 = ta - 0.3 * (tb - ta); t0 <= tb + 0.3 * (tb - ta); t0 += std::max(0.5, (tb - ta) / 200))
        for (double B = 2; B <= 600; B *= 1.12) { double fc, A; const double e = solve(r, t0, B, fc, A); if (e < best) { best = e; bt0 = t0; bB = B; bfc = fc; bA = A; } }
    // refine around the best
    double st = std::max(0.5, (tb - ta) / 200), sb = bB * 0.12;
    for (int it = 0; it < 4; it++) {
        for (double t0 = bt0 - 2 * st; t0 <= bt0 + 2 * st; t0 += st / 2) for (double B = std::max(1.0, bB - 2 * sb); B <= bB + 2 * sb; B += sb / 2) { double fc, A; const double e = solve(r, t0, B, fc, A); if (e < best) { best = e; bt0 = t0; bB = B; bfc = fc; bA = A; } }
        st /= 2; sb /= 2;
    }
    c.ok = true; c.fc = bfc; c.A = bA; c.t0 = bt0; c.B = bB; c.rms = std::sqrt(best / r.size());
    return c;
}

class DopplerLogDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); n_ = 1; while (n_ * 2 <= rate_) n_ *= 2; winSec_ = n_ / rate_;
        win_.resize(n_); for (int i = 0; i < n_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n_));
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); skip_ = (size_t)(0.2 * rate_); t_ = 0; rd_.clear(); lastSeen_ = -1e9; lastFit_ = -1e9; passNo_ = 0; inPass_ = false; curve_ = Curve(); lastRow_ = -1e9; have_ = false; level_ = 0; curHz_ = 0; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            if (skip_) { skip_--; return; }
            buf_.push_back(a);
            if ((int)buf_.size() >= n_) { analyse(); buf_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!inPass_) { t.headline = passNo_ ? fmt("no carrier; %d pass(es) logged, last TCA %.0f s", passNo_, lastT0_) : "no beacon carrier in +-14 kHz of the tuned frequency"; if (!passNo_) return; }
        else {
            const size_t m = rd_.size(); double rate = 0;
            if (m >= 6) rate = (rd_[m - 1].hz - rd_[m - 6].hz) / (rd_[m - 1].t - rd_[m - 6].t);
            t.headline = fmt("carrier %+.0f Hz, changing %+.1f Hz/s, %zu readings in this pass", curHz_, rate, m);
            t.facts.push_back({"Doppler now", fmt("%+.0f Hz (%+.1f Hz/s)", curHz_, rate)});
            t.facts.push_back({"level", fmt("%.1f dB over the noise in the window", level_)});
            t.facts.push_back({"readings", fmt("%zu, one every %.2f s, first at %.0f s", m, winSec_, rd_.front().t)});
        }
        if (curve_.ok) {
            t.facts.push_back({"time of closest approach", fmt("%.1f s", curve_.t0)});
            t.facts.push_back({"carrier without Doppler", fmt("%+.1f Hz from the tuned frequency", curve_.fc)});
            t.facts.push_back({"largest Doppler (far away)", fmt("%.0f Hz", std::fabs(curve_.A))});
            t.facts.push_back({"steepest slope (at closest approach)", fmt("%.1f Hz/s", std::fabs(curve_.A / curve_.B))});
            t.facts.push_back({"fit error", fmt("%.1f Hz RMS", curve_.rms)});
            if (centre_ > 0) { const double v = kC * std::fabs(curve_.A) / (centre_ + curve_.fc) / 1000; t.facts.push_back({"speed and closest distance (straight-line model)", fmt("%.2f km/s, %.0f km", v, v * curve_.B)}); }
        }
    }
private:
    void analyse() {
        have_ = true;
        std::vector<cf32> a((size_t)n_);
        for (int i = 0; i < n_; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        const double bh = rate_ / n_;
        std::vector<float> p(n_); for (int k = 0; k < n_; k++) p[k] = std::norm(a[k]);
        const int hw = (int)(kSearchHz / bh);
        int kp = 0; float pk = 0;
        for (int d = -hw; d <= hw; d++) { const int k = (d + n_) % n_; if (p[k] > pk) { pk = p[k]; kp = k; } }
        std::vector<float> nb; for (int d = -(int)(kCut / bh); d <= (int)(kCut / bh); d += 3) { const int k = (d + n_) % n_; if (std::abs(((k - kp + n_ / 2) % n_ + n_) % n_ - n_ / 2) > 40) nb.push_back(p[k]); }
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end()); const double noise = std::max((double)nb[nb.size() / 2], 1e-30) / 0.693;
        // the carrier moves during the window and smears over a few bins: use the energy of the peak and its 7 neighbours
        double pw = 0; for (int d = -3; d <= 3; d++) pw += p[(kp + d + n_) % n_];
        const double db = 10 * std::log10(pw / (noise * 1.5));
        const bool present = pk > 40.0 * noise;
        if (!present) {
            if (inPass_ && t_ - lastSeen_ > kGapEndSec) endPass();
            if (inPass_) out->energy(); return;
        }
        const double la = std::log(p[(kp + n_ - 1) % n_] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % n_] + 1e-30), den = la - 2 * lb + lc;
        double fb = (kp + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0)) * bh; if (fb > rate_ / 2) fb -= rate_;
        if (!inPass_) { inPass_ = true; rd_.clear(); curve_ = Curve(); lastFit_ = t_; out->row("Pass", "start", fmt("carrier found at %+.0f Hz, %.1f dB over the noise", fb, db), (float)db); }
        lastSeen_ = t_; curHz_ = fb; level_ = (float)db;
        rd_.push_back({t_ - 0.5 * winSec_, fb, db});
        out->good((float)db, fb);
        if (t_ - lastRow_ >= 10.0) {
            lastRow_ = t_; const size_t m = rd_.size(); const double rate = m >= 6 ? (rd_[m - 1].hz - rd_[m - 6].hz) / (rd_[m - 1].t - rd_[m - 6].t) : 0;
            out->row("Doppler", fmt("%+.0f Hz", fb), fmt("t %.0f s, %+.1f Hz/s, %.1f dB over the noise", t_, rate, db), (float)db);
        }
        if (t_ - lastFit_ >= kFitEverySec && rd_.size() >= 40) { lastFit_ = t_; curve_ = fitCurve(rd_); }
    }
    void endPass() {
        inPass_ = false;
        // trim the readings of the gap
        if (rd_.size() >= 20) {
            curve_ = fitCurve(rd_);
            if (curve_.ok) {
                passNo_++; lastT0_ = curve_.t0;
                const double v = centre_ > 0 ? kC * std::fabs(curve_.A) / (centre_ + curve_.fc) / 1000 : 0;
                out->row("Pass", fmt("TCA %.0f s", curve_.t0), fmt("%zu readings; no-Doppler carrier %+.1f Hz; Doppler span +-%.0f Hz; steepest %.1f Hz/s at TCA; fit %.1f Hz RMS%s", rd_.size(), curve_.fc, std::fabs(curve_.A), std::fabs(curve_.A / curve_.B), curve_.rms,
                         centre_ > 0 ? fmt("; speed %.2f km/s, closest distance %.0f km (straight-line model)", v, v * curve_.B).c_str() : ""), (float)rd_.back().db);
            }
        } else out->row("Pass", "short", fmt("carrier seen for %zu readings only, too few for a curve", rd_.size()), 0.f);
    }
    Decimator dec_; double rate_ = 1, t_ = 0, centre_ = 0, winSec_ = 1, lastSeen_ = -1e9, lastFit_ = -1e9, lastT0_ = 0, lastRow_ = -1e9, curHz_ = 0; int n_ = 1, passNo_ = 0; size_t skip_ = 0; float level_ = 0;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<Rd> rd_; bool inPass_ = false, have_ = false; Curve curve_;
};
}

std::unique_ptr<Decoder> makeDopplerLog() { return std::make_unique<DopplerLogDec>(); }

} // namespace dect2::x40
