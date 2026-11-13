// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// W1A ELT 121.5 MHz: detector of the swept-tone AM signal of an emergency locator transmitter, and its test generator (see w1a_dsp.h).
// Public description (UK CAA ELT specification text, ICAO Annex 10 Vol III): AM carrier, modulation factor at least 0.85, the modulating tone sweeps
// DOWNWARDS over at least 700 Hz inside 1600 - 300 Hz at 2 - 4 sweeps per second, duty cycle 33 - 66 %. Detection only: no beacon data is carried.
#include "w1a_priv.h"

namespace dect2::w1a {

namespace {

constexpr int kWin = 240;               // 20 ms analysis window
constexpr int kHop = 120;               // 10 ms
constexpr double kF0 = 250, kStep = 25; // tone search grid
constexpr int kNb = 65;                 // 250 .. 1850 Hz

class EltDec : public Decoder {
public:
    EltDec() {
        for (int i = 0; i < kWin; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kWin));
        for (int b = 0; b < kNb; b++) {
            const double w = 2 * kPi * (kF0 + kStep * b) / kRate;
            for (int i = 0; i < kWin; i++) { c_[(size_t)b][(size_t)i] = (float)std::cos(w * i) * win_[(size_t)i]; s_[(size_t)b][(size_t)i] = (float)std::sin(w * i) * win_[(size_t)i]; }
        }
        reset();
    }
    void reset() override {
        fill_ = 0; dc_ = 0; t_ = 0; sweeps_ = 0; starts_.clear(); run_ = false; gap_ = 0; lastStrong_ = -10; confirmed_ = false;
        rate_ = 0; hi_ = lo_ = 0; depth_ = 0; ratio_ = 0; accA_ = 0; accN_ = 0; state_ = 0;
        for (float& v : ring_) v = 0;
    }
    void push(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const float env = std::isfinite(x[i].real()) && std::isfinite(x[i].imag()) ? std::abs(x[i]) : 0.f;
            dc_ += 0.0005f * (env - dc_);
            ring_[(size_t)fill_++] = env - dc_;
            accA_ += (double)(env - dc_) * (env - dc_); accN_++;
            if (fill_ == kWin) {
                frame();
                std::copy(ring_ + kHop, ring_ + kWin, ring_);
                fill_ = kWin - kHop;
            }
        }
    }
    Status status() const override {
        Status s;
        s.good = sweeps_; s.state = state_; s.snrDb = ratio_;
        s.cfoHz = rate_;
        char m[160];
        if (confirmed_) snprintf(m, sizeof m, "ELT tone: %.1f sweeps/s, %.0f to %.0f Hz, AM %.0f %%", rate_, hi_, lo_, depth_ * 100);
        else if (sweeps_ > 0) snprintf(m, sizeof m, "swept tone seen (%llu), not confirmed", (unsigned long long)sweeps_);
        else snprintf(m, sizeof m, "searching");
        s.mode = m;
        char a[48], b[48], c[48];
        snprintf(a, sizeof a, "%.2f Hz", rate_); snprintf(b, sizeof b, "%.0f to %.0f Hz", hi_, lo_); snprintf(c, sizeof c, "%.0f %%", depth_ * 100);
        s.params = {{"signal", "AM 121.5 MHz, tone swept downwards (detection only)"}, {"sweep rate", a}, {"sweep range", b}, {"modulation", c},
                    {"sweeps counted", std::to_string(sweeps_)}};
        return s;
    }
private:
    void frame() {
        t_ += kHop / kRate;
        const double tNow = t_;
        // tone search
        double pk = 0, tot = 0;
        int ib = 0;
        double p[kNb];
        for (int b = 0; b < kNb; b++) {
            double re = 0, im = 0;
            for (int i = 0; i < kWin; i++) { re += (double)ring_[(size_t)i] * c_[(size_t)b][(size_t)i]; im += (double)ring_[(size_t)i] * s_[(size_t)b][(size_t)i]; }
            p[b] = re * re + im * im;
            tot += p[b];
            if (p[b] > pk) { pk = p[b]; ib = b; }
        }
        double f = kF0 + kStep * ib;
        if (ib > 0 && ib < kNb - 1) {   // parabolic interpolation on the log power
            const double a = std::log(p[ib - 1] + 1e-30), b = std::log(p[ib] + 1e-30), c = std::log(p[ib + 1] + 1e-30);
            const double den = a - 2 * b + c;
            if (den < -1e-9) f += kStep * 0.5 * (a - c) / den;
        }
        // amplitude of the tone relative to the carrier: the Hann-windowed sum of a sine of amplitude m is m * 120 / 2 ... in the DFT sense: |X| = m * sum(w) / 2
        const double amp = 2 * std::sqrt(pk) / 120.0;
        const double carrier = std::max((double)dc_, 1e-6);
        const double mean = tot / kNb;
        const bool strong = pk > 12 * mean && amp / carrier > 0.25 && f > 250 && f < 1850;
        ratio_ = (float)(10 * std::log10(pk / (mean + 1e-30) + 1e-9));
        if (strong) {
            lastStrong_ = tNow;
            gap_ = 0;
            depthAcc_ = 0.9 * depthAcc_ + 0.1 * amp / carrier; if (depthAcc_ == 0) depthAcc_ = amp / carrier;
            if (!run_) startRun(tNow, f);
            else if (f > lastF_ + 250) { endRun(); startRun(tNow, f); }
            else if (f <= lastF_ + 120) { lastF_ = std::min(lastF_, f); minF_ = std::min(minF_, f); lastT_ = tNow; }
        } else if (run_ && ++gap_ > 4) {
            endRun();
        }
        if (confirmed_ && tNow - lastSweepT_ > 2.0) {
            confirmed_ = false; state_ = 0; rate_ = 0;
            char m[96]; snprintf(m, sizeof m, "ELT tone ended after %llu sweeps", (unsigned long long)sweeps_);
            event(m);
        }
    }
    void startRun(double t, double f) { run_ = true; startT_ = lastT_ = t; startF_ = lastF_ = minF_ = f; gap_ = 0; }
    void endRun() {
        run_ = false;
        const double range = startF_ - minF_;
        if (startF_ >= 900 && range >= 600 && lastT_ - startT_ >= 0.08) {
            sweeps_++;
            lastSweepT_ = lastT_;
            if (!starts_.empty() && startT_ - starts_.back() > 1.5) starts_.clear();
            starts_.push_back(startT_);
            if (starts_.size() > 9) starts_.erase(starts_.begin());
            hi_ = 0.7 * (hi_ > 0 ? hi_ : startF_) + 0.3 * startF_;
            lo_ = 0.7 * (lo_ > 0 ? lo_ : minF_) + 0.3 * minF_;
            depth_ = std::min(1.0, depthAcc_);
            state_ = 1;
            std::vector<double> per;
            for (size_t i = 1; i < starts_.size(); i++) per.push_back(starts_[i] - starts_[i - 1]);
            if (per.size() >= 3) {
                std::vector<double> sp = per;
                std::sort(sp.begin(), sp.end());
                const double med = sp[sp.size() / 2];
                int ok = 0;
                for (double q : per) if (std::fabs(q - med) < 0.2 * med) ok++;
                if (med >= 0.2 && med <= 0.6 && ok >= 3) {
                    rate_ = 1.0 / med;
                    state_ = 2;
                    if (!confirmed_) {
                        confirmed_ = true;
                        char m[128]; snprintf(m, sizeof m, "ELT swept tone found: %.1f sweeps/s, %.0f to %.0f Hz", rate_, hi_, lo_);
                        event(m);
                    }
                }
            }
        }
    }
    float win_[kWin] = {}, c_[kNb][kWin] = {}, s_[kNb][kWin] = {}, ring_[kWin] = {};
    int fill_ = 0, gap_ = 0, state_ = 0;
    float dc_ = 0, ratio_ = 0;
    double t_ = 0, startT_ = 0, lastT_ = 0, startF_ = 0, lastF_ = 0, minF_ = 0, lastStrong_ = -10, lastSweepT_ = 0;
    double rate_ = 0, hi_ = 0, lo_ = 0, depth_ = 0, depthAcc_ = 0, accA_ = 0;
    uint64_t sweeps_ = 0, accN_ = 0;
    bool run_ = false, confirmed_ = false;
    std::vector<double> starts_;
};

}  // namespace

std::unique_ptr<Decoder> makeEltDecoder() { return std::make_unique<EltDec>(); }

std::vector<cf32> genElt(const GenOpts& o) {
    const size_t N = (size_t)(o.seconds * kRate);
    std::vector<cf32> z(N);
    const double T = 1.0 / o.sweepHz, duty = std::max(0.1, std::min(1.0, o.dutyPct / 100.0));
    double ph = 0;
    for (size_t i = 0; i < N; i++) {
        const double t = (double)i / kRate;
        double m = 0;
        if (t > o.leadSec) {
            const double u = std::fmod(t - o.leadSec, T) / T;
            if (u < duty) {
                const double f = o.highHz - (o.highHz - o.lowHz) * (u / duty);
                ph += 2 * kPi * f / kRate;
                if (ph > 2 * kPi) ph -= 2 * kPi;
                m = o.depth * std::sin(ph);
            }
        }
        z[i] = cf32((float)(0.5 * (1.0 + m)), 0.f);
    }
    return z;
}

}  // namespace dect2::w1a
