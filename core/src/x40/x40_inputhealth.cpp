// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SDR input health meter: what the samples that reach the program look like before any filtering, once a second, for whatever the radio is receiving (best on a band with no single huge signal):
// the level (RMS and peak in dB re full scale of +-1.0 per component), the crest factor, clipping (samples at full scale: parts per million of the samples, and whether the peak sits on a flat top),
// the DC offset (mean of I and of Q, in % of full scale and in dB under the RMS level) and the IQ imbalance read blind from the second-order statistics of the samples: the gain mismatch
// 10 log(P_Q / P_I), the phase error asin(E[IQ] / sqrt(P_I P_Q)), and the image rejection they would cause (about -10 log((e^2 + p^2) / 4) with e the gain error and p the phase error in radians). The blind IQ reading assumes the
// signals are spread over both sides of the centre (a complex signal has no correlation between I and Q): a strong signal that is itself real (an AM carrier at the centre frequency) or only one side
// loaded make it read a mismatch that is not the radio's. The verdict is a rule of thumb (an 8-bit radio wants the RMS level around -30 to -15 dBFS for a quiet band, 12-bit and up around -40 to -20).
// A reading of the input, not of the radio's performance: it does not know the radio's true full scale. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kBlockSec = 1.0;

class InputHealthDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; blockN_ = (size_t)(kBlockSec * fs); reset(); return true; }
    void reset() override { n_ = 0; sI_ = sQ_ = sII_ = sQQ_ = sIQ_ = 0; pk_ = 0; clip_ = 0; flat_ = 0; have_ = false; verdict_ = -1; res_ = Res(); t_ = 0; lastClipRow_ = -1e9; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const double a = x[i].real(), b = x[i].imag(); sI_ += a; sQ_ += b; sII_ += a * a; sQQ_ += b * b; sIQ_ += a * b;
            const double m = std::max(std::fabs(a), std::fabs(b)); if (m > pk_) pk_ = m;
            if (m >= 0.985) clip_++;
            if (++n_ >= blockN_) block();
        }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = fmt("RMS %.1f dBFS, peak %.1f dBFS, clipping %.0f ppm, DC %.2f %%, IQ image %s", res_.rmsDb, res_.peakDb, res_.clipPpm, res_.dcPct, res_.irr > 60 ? ">60 dB down" : fmt("%.0f dB down", res_.irr).c_str());
        t.facts.push_back({"level (RMS)", fmt("%.1f dB re full scale", res_.rmsDb)});
        t.facts.push_back({"peak", fmt("%.1f dB re full scale (crest factor %.1f dB)", res_.peakDb, res_.peakDb - res_.rmsDb)});
        t.facts.push_back({"clipping", fmt("%.0f ppm of the samples within 1.5 %% of full scale%s", res_.clipPpm, res_.clipPpm > 100 ? " : overload, lower the gain" : "")});
        t.facts.push_back({"DC offset", fmt("I %+.3f %%, Q %+.3f %% of full scale (%.1f dB under the RMS level)", res_.dcI * 100, res_.dcQ * 100, res_.dcRel)});
        t.facts.push_back({"IQ gain mismatch (Q against I)", fmt("%+.2f dB", res_.gainDb)});
        t.facts.push_back({"IQ phase error", fmt("%+.2f degrees", res_.phaseDeg)});
        t.facts.push_back({"image rejection (from the mismatch)", res_.irr > 80 ? std::string("above 80 dB") : fmt("%.0f dB", res_.irr)});
        t.facts.push_back({"verdict", verdictText(verdict_)});
    }
private:
    struct Res { double rmsDb = 0, peakDb = 0, clipPpm = 0, dcI = 0, dcQ = 0, dcPct = 0, dcRel = 0, gainDb = 0, phaseDeg = 0, irr = 99; };
    static std::string verdictText(int v) {
        switch (v) { case 0: return "level too low: the radio's own noise floor is not reached, raise the gain"; case 1: return "level good"; case 2: return "level high: little headroom for a strong signal, consider lowering the gain"; case 3: return "overload: samples at full scale, lower the gain"; default: return "-"; }
    }
    void block() {
        const double N = (double)n_;
        Res r; const double mI = sI_ / N, mQ = sQ_ / N, pI = sII_ / N - mI * mI, pQ = sQQ_ / N - mQ * mQ, cIQ = sIQ_ / N - mI * mQ;
        const double ptot = sII_ / N + sQQ_ / N;
        r.rmsDb = 10 * std::log10(std::max(ptot, 1e-20)); r.peakDb = 20 * std::log10(std::max(pk_, 1e-10));
        r.clipPpm = 1e6 * (double)clip_ / N; r.dcI = mI; r.dcQ = mQ; r.dcPct = 100 * std::hypot(mI, mQ);
        r.dcRel = 10 * std::log10(std::max(mI * mI + mQ * mQ, 1e-20) / std::max(ptot, 1e-20));
        r.gainDb = 10 * std::log10(std::max(pQ, 1e-20) / std::max(pI, 1e-20));      // Q against I
        const double rho = cIQ / std::sqrt(std::max(pI * pQ, 1e-30)); r.phaseDeg = std::asin(std::max(-1.0, std::min(1.0, rho))) * 180 / kPi;
        const double e = std::sqrt(pI / std::max(pQ, 1e-30)) - 1, p = std::asin(std::max(-1.0, std::min(1.0, rho)));
        r.irr = -10 * std::log10(std::max((e * e + p * p) / 4, 1e-12));
        res_ = r; have_ = true; t_ += kBlockSec;
        int v = r.clipPpm > 100 ? 3 : r.rmsDb > -10 ? 2 : r.rmsDb < -50 ? 0 : 1;
        if (v != verdict_) { verdict_ = v; out->row("Verdict", v == 0 ? "low" : v == 1 ? "good" : v == 2 ? "high" : "overload", verdictText(v), (float)r.rmsDb); }
        if (r.clipPpm > 100 && t_ - lastClipRow_ >= 10) { lastClipRow_ = t_; out->row("Clipping", fmt("%.0f ppm", r.clipPpm), fmt("peak %.1f dBFS, RMS %.1f dBFS", r.peakDb, r.rmsDb), (float)r.rmsDb); }
        if (v == 3) out->bad(); else out->good((float)(-r.rmsDb), 0);
        n_ = 0; sI_ = sQ_ = sII_ = sQQ_ = sIQ_ = 0; pk_ = 0; clip_ = 0;
    }
    double fs_ = 1, sI_ = 0, sQ_ = 0, sII_ = 0, sQQ_ = 0, sIQ_ = 0, pk_ = 0, t_ = 0, lastClipRow_ = -1e9; size_t blockN_ = 1, n_ = 0, clip_ = 0, flat_ = 0; bool have_ = false; int verdict_ = -1; Res res_;
};
}

std::unique_ptr<Decoder> makeInputHealth() { return std::make_unique<InputHealthDec>(); }

} // namespace dect2::x40
