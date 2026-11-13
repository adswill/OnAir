// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SDR stream glitch detector: what a flaky USB link, a driver or a gain loop does to the samples, for debugging a radio. Four things are watched.
// 1. Dropped or repeated samples: the phase of the strongest steady tone (at least 20 dB over the noise per sample) is followed sample by sample against its own running frequency; a jump is the tone's frequency times the time of the
//    missing samples: for a tone at f Hz at rate fs and n samples lost, the phase steps by 2 pi f n / fs (modulo one turn): the number is only known to within whole turns (n = d phase / (2 pi f / fs) + k fs / f), so the estimate is the smallest
//    one, and a repeat (samples sent twice) steps the other way. Needs a tone; with noise only a drop cannot be seen.
// 2. Zero runs: 16 or more exactly zero samples in a row (a driver that hands out an empty buffer). 3. Repeated blocks: a block of 256 samples identical to the one before it (a buffer sent twice).
// 4. Level steps: the power in 1 ms blocks against the median of the last 50 ms; a step of 1 dB or more that stays for 5 ms (an AGC or gain change, a switched attenuator, a relay).
// Each event is a row with its time; totals are kept. A real signal that jumps in phase (a PSK transmitter) can look like a drop: the tone must be steady. Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr int kFft = 8192, kBlock = 256;

class GlitchDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; reset(); return true; }
    void reset() override {
        nIn_ = 0; buf_.clear(); haveTone_ = false; toneHz_ = 0; ph_ = 0; prevPh_ = 0; dph_ = 0; nDrop_ = 0; nRep_ = 0; nZero_ = 0; nBlkRep_ = 0; nStep_ = 0; zeroRun_ = 0; blk_.clear(); prevBlk_.clear(); haveBlk_ = false;
        pAcc_ = 0; nP_ = 0; pHist_.clear(); pending_ = false; tonePow_ = 0; lastRow_ = -1e9; have_ = false; tDecide_ = 0.5; wasZero_ = false; resid_.clear();
    }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) sample(x[i]);
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        const double el = (double)nIn_ / fs_;
        t.headline = fmt("%.0f s: %d drops / %d repeats of samples, %d zero runs, %d repeated blocks, %d level steps", el, nDrop_, nRep_, nZero_, nBlkRep_, nStep_);
        t.facts.push_back({"watched tone", haveTone_ ? fmt("%+.1f Hz (a steady tone for the sample-drop test)", toneHz_) : std::string("none (a drop cannot be seen without a steady tone 20 dB over the noise)")});
        t.facts.push_back({"dropped samples", fmt("%d events", nDrop_)});
        t.facts.push_back({"repeated samples", fmt("%d events", nRep_)});
        t.facts.push_back({"zero runs", fmt("%d events (16 or more zero samples in a row)", nZero_)});
        t.facts.push_back({"repeated blocks", fmt("%d events (a block of %d samples identical to the one before)", nBlkRep_, kBlock)});
        t.facts.push_back({"level steps", fmt("%d events (1 dB or more, lasting 5 ms or more)", nStep_)});
        t.facts.push_back({"time", fmt("%.1f s observed, %.3f Msps", el, fs_ / 1e6)});
    }
private:
    void sample(cf32 x) {
        have_ = true; const double t = (double)nIn_ / fs_; nIn_++;
        // zero runs
        if (x.real() == 0.f && x.imag() == 0.f) { zeroRun_++; if (zeroRun_ == 16 && !wasZero_) { wasZero_ = true; nZero_++; out->row("Zero run", fmt("%.4f s", t - 15.0 / fs_), "16 or more zero samples in a row", -120.f); } } else { if (wasZero_ && zeroRun_ >= 16) out->bad(); zeroRun_ = 0; wasZero_ = false; }
        // repeated blocks
        blk_.push_back(x); if ((int)blk_.size() == kBlock) { if (haveBlk_ && blk_ == prevBlk_ && !allZero(blk_)) { nBlkRep_++; out->row("Repeated block", fmt("%.4f s", t - (double)kBlock / fs_), fmt("%d samples identical to the block before", kBlock), -120.f); } prevBlk_ = blk_; haveBlk_ = true; blk_.clear(); }
        // level steps: 1 ms blocks against the median of the last 50
        pAcc_ += std::norm(x); nP_++;
        const int bn = std::max(8, (int)(0.001 * fs_));
        if (nP_ >= bn) { const double db = 10 * std::log10(std::max(pAcc_ / nP_, 1e-20)); pAcc_ = 0; nP_ = 0; levelBlock(db, t); }
        // the tone
        if (!haveTone_) { buf_.push_back(x); if ((int)buf_.size() == kFft) { findTone(); buf_.clear(); } return; }
        trackTone(x, t);
    }
    static bool allZero(const std::vector<cf32>& b) { for (auto& v : b) if (v != cf32(0, 0)) return false; return true; }
    void levelBlock(double db, double t) {
        pHist_.push_back(db); if (pHist_.size() > 50) pHist_.pop_front();
        if (pHist_.size() < 30) return;
        std::vector<double> v(pHist_.begin(), pHist_.end() - 1); std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end()); const double med = v[v.size() / 2];
        if (!pending_) { if (std::fabs(db - med) >= 1.0) { pending_ = true; pendT_ = t; pendBase_ = med; pendN_ = 0; pendSum_ = 0; } }
        else { pendN_++; pendSum_ += db; if (pendN_ >= 5) { const double m = pendSum_ / pendN_; if (std::fabs(m - pendBase_) >= 1.0 && std::fabs(db - pendBase_) >= 1.0) { nStep_++; out->row("Level step", fmt("%+.1f dB", m - pendBase_), fmt("at %.4f s: from %.1f to %.1f dB (power in 1 ms blocks)", pendT_, pendBase_, m), (float)m); pHist_.clear(); } pending_ = false; } }
    }
    void findTone() {
        std::vector<cf32> a((size_t)kFft); for (int i = 0; i < kFft; i++) a[i] = buf_[i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft)); fftx(a);
        std::vector<float> p((size_t)kFft); for (int k = 0; k < kFft; k++) p[k] = std::norm(a[k]);
        int kp = 0; for (int k = 0; k < kFft; k++) if (p[k] > p[kp]) kp = k;
        std::vector<float> s(p); std::nth_element(s.begin(), s.begin() + kFft / 2, s.end()); const double med = std::max((double)s[kFft / 2], 1e-30);
        if (p[kp] / (med / 0.693) < 100.0 * kFft * 0.667) return;       // the tone must be 20 dB over the noise per sample (the phase of every sample is judged)
        const double la = std::log(p[(kp + kFft - 1) % kFft] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % kFft] + 1e-30), den = la - 2 * lb + lc;
        double bin = kp + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0); if (bin > kFft / 2) bin -= kFft;
        toneHz_ = bin * fs_ / kFft; haveTone_ = true; ph_ = 0; dph_ = 2 * kPi * toneHz_ / fs_; started_ = false; nTrack_ = 0; resid_.clear();
        // the first samples of the window are replayed so that the tracker starts on them
        std::vector<cf32> b(buf_); for (size_t i = 0; i < b.size(); i++) trackTone(b[i], (double)(nIn_ - b.size() + i) / fs_);
    }
    void trackTone(cf32 x, double t) {
        // the phase of the sample against a reference oscillator at the tone frequency, low-passed over 64 samples; a drop is a step in this phase
        const cf32 ref((float)std::cos(ph_), (float)-std::sin(ph_)); ph_ += dph_; if (ph_ > kPi) ph_ -= 2 * kPi;
        const cf32 z = x * ref; acc_ = acc_ * 0.97f + z * 0.03f; nTrack_++;
        const double phase = std::arg(acc_);
        if (!started_) { if (nTrack_ > 1500) { started_ = true; lpPh_ = phase; lastPh_ = phase; sinceStep_ = 0; slope_ = 0; } return; }
        // slow follower for the tone's own frequency drift: the step is the difference of the fast phase to a slow average
        double d = phase - lpPh_; while (d > kPi) d -= 2 * kPi; while (d < -kPi) d += 2 * kPi;
        resid_.push_back(d); if (resid_.size() > 2048) resid_.pop_front();
        sinceStep_++;
        if (!pendingStep_) { lpPh_ += slope_ + 0.01 * d; slope_ += 2.5e-5 * d; }
        if (std::fabs(d) > 0.06 && sinceStep_ > 1000 && !pendingStep_) {
            // a step: the phase jumped within the last few samples; wait for it to settle, then read the size from the change of the slow phase
            stepDelay_ = 0; pendingStep_ = true; stepT_ = t; stepFrom_ = lpPh_; stepSlope_ = slope_;
        }
        if (pendingStep_) { if (++stepDelay_ >= 200) { pendingStep_ = false; sinceStep_ = 0; const double jump = wrapPi(phase - (stepFrom_ + stepSlope_ * (stepDelay_ + 1))); const double perSample = dph_; double n = jump / perSample; n = std::fabs(perSample) > 1e-9 ? n : 0.0;
                const int cnt = (int)std::lround(std::fabs(n)); const bool lost = n < 0;     // a lost sample advances the reference less than the tone: the measured phase steps backwards... sign checked in the test
                if (cnt >= 1 || std::fabs(jump) > 0.5) { if (n > 0) { nDrop_++; out->row("Dropped samples", fmt("%.4f s", stepT_), fmt("about %d sample(s) missing (phase step %+.2f rad of a tone at %+.1f Hz; known up to whole turns)", cnt, jump, toneHz_), 0.f); } else { nRep_++; out->row("Repeated samples", fmt("%.4f s", stepT_), fmt("about %d sample(s) repeated (phase step %+.2f rad of a tone at %+.1f Hz)", cnt, jump, toneHz_), 0.f); } (void)lost; lpPh_ = phase; slope_ = stepSlope_; out->bad(); } else { lpPh_ = phase; slope_ = stepSlope_; } } }
    }
    static double wrapPi(double d) { while (d > kPi) d -= 2 * kPi; while (d < -kPi) d += 2 * kPi; return d; }
    double fs_ = 1, toneHz_ = 0, ph_ = 0, prevPh_ = 0, dph_ = 0, pAcc_ = 0, pendT_ = 0, pendBase_ = 0, pendSum_ = 0, lpPh_ = 0, lastPh_ = 0, stepT_ = 0, stepFrom_ = 0, slope_ = 0, stepSlope_ = 0, lastRow_ = 0, tDecide_ = 0.5, tonePow_ = 0; long nIn_ = 0; int nDrop_ = 0, nRep_ = 0, nZero_ = 0, nBlkRep_ = 0, nStep_ = 0, zeroRun_ = 0, nP_ = 0, pendN_ = 0, nTrack_ = 0, sinceStep_ = 0, stepDelay_ = 0;
    bool haveTone_ = false, haveBlk_ = false, pending_ = false, have_ = false, wasZero_ = false, started_ = false, pendingStep_ = false; cf32 acc_{0, 0};
    std::vector<cf32> buf_, blk_, prevBlk_; std::deque<double> pHist_, resid_;
};
}

std::unique_ptr<Decoder> makeGlitch() { return std::make_unique<GlitchDec>(); }

} // namespace dect2::x40
