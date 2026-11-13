// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar test signal: see globalstar_gen.h.
#include "dect2/globalstar/globalstar_gen.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/globalstar/globalstar_rx.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;

class GlobalstarSynth : public ModeSynth {
public:
    GlobalstarSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfg_(cfg), noise_(7) {
        scenario_ = cfg.modeOpt[0];
        timeScale_ = cfg.modeVal[0] > 0 ? cfg.modeVal[0] : 1.0;
        // the channels in view, the way the receiver counts them
        std::vector<int> view;
        for (int c = 0; c < kGsChannels; c++) {
            const double f = globalstarChannelHz(c) - kGsDefaultDialHz;
            if (std::fabs(f) + 0.74e6 <= 0.46 * rate) view.push_back(c);
        }
        if (view.empty()) view.push_back(6);
        int a = view[0];
        for (int c : view) if (std::fabs(globalstarChannelHz(c) - kGsDefaultDialHz) < std::fabs(globalstarChannelHz(a) - kGsDefaultDialHz)) a = c;
        if (cfg.modeOpt[1] >= 1 && cfg.modeOpt[1] <= kGsChannels) a = cfg.modeOpt[1] - 1;
        chA_ = a;
        chB_ = -1;
        for (int c : view) if (c != a && (chB_ < 0 || std::abs(c - a) < std::abs(chB_ - a))) chB_ = c;
        // a loop of band-limited noise: flat over +-0.5 MHz, a raised-cosine skirt to +-0.615 MHz
        const size_t M = (size_t)1 << 18;
        std::vector<cf32> sp(M, cf32());
        std::mt19937 rng(99);
        genutil::PortableNormal nd;
        for (size_t k = 0; k < M; k++) {
            const double f = (k < M / 2 ? (double)k : (double)k - (double)M) * rate / (double)M;
            const double af = std::fabs(f);
            double g = 0;
            if (af <= 0.5e6) g = 1;
            else if (af < 0.615e6) g = 0.5 + 0.5 * std::cos(kPi * (af - 0.5e6) / 0.115e6);
            if (g > 0) sp[k] = cf32((float)(g * nd(rng)), (float)(g * nd(rng)));
        }
        Fft fft((int)M);
        fft.inverse(sp.data());
        double pw = 0;
        for (const cf32& v : sp) pw += std::norm(v);
        const float norm = (float)(1.0 / std::sqrt(pw / (double)M));
        for (cf32& v : sp) v *= norm;
        base_ = std::move(sp);
        const double lin = std::pow(10.0, cfg.snrDb / 10.0);
        const double chanFrac = 1.23e6 / rate;                // noise power inside a channel / total
        double amp = 0.2;                                     // rms of one channel
        double nsig = cfg.snrDb < -100 ? 0 : std::sqrt(amp * amp / (chanFrac * lin) / 2);   // per real component
        if (nsig > 0.18) { amp *= 0.18 / nsig; nsig = 0.18; }
        amp_ = (float)amp; sigma_ = (float)nsig;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32());
        const size_t M = base_.size();
        size_t done = 0;
        while (done < n) {
            const size_t m = std::min<size_t>(n - done, 4096 - (size_t)(idx_ % 4096));   // chunks on a fixed 4096-sample grid: the same signal whatever the block size
            const double sec = ((double)(idx_ / 4096 * 4096) + 2048.0) / rate_ * timeScale_;   // the middle of the grid chunk
            addChannel(out + done, m, chA_, 0, scenario_ == 2 ? false : true, dopplerAt(sec));
            if (scenario_ == 0 && chB_ >= 0) addChannel(out + done, m, chB_, 1, std::fmod(sec, 0.8) < 0.3, dopplerAt(sec) * 0.4 - 4000.0);
            idx_ += m; done += m;
            (void)M;
        }
        if (sigma_ > 0.f) noise_.add(out, n, sigma_);
    }
private:
    double dopplerAt(double sec) const {
        if (scenario_ == 0) return globalstarGenDoppler(sec) + cfg_.cfoHz;
        return cfg_.cfoHz;
    }
    void addChannel(cf32* out, size_t m, int c, int slot, bool on, double dop) {
        Phase& P = ph_[slot];
        const double f = globalstarChannelHz(c) - kGsDefaultDialHz + dop;
        const double w = 2 * kPi * f / rate_;
        const cf32 step((float)std::cos(w), (float)std::sin(w));
        const size_t M = base_.size();
        const float target = on ? 1.f : 0.f;
        if (on || P.gain > 0.f) {   // a burst keys up and down over one chunk (a linear ramp), not in a step that splatters
            size_t pos = (size_t)((idx_ + (size_t)slot * 77777u + (size_t)c * 20011u) % M);
            for (size_t i = 0; i < m; i++) {
                out[i] += base_[pos] * P.rot * (amp_ * P.gain);
                P.gain = target > P.gain ? std::min(target, P.gain + 1.f / 4096) : std::max(target, P.gain - 1.f / 4096);
                P.rot *= step;
                if (++pos == M) pos = 0;
            }
        } else {
            for (size_t i = 0; i < m; i++) P.rot *= step;
        }
        P.rot /= std::abs(P.rot);
    }
    struct Phase { cf32 rot = cf32(1.f, 0.f); float gain = 0.f; };
    double rate_;
    SynthConfig cfg_;
    genutil::NoiseSource noise_;
    std::vector<cf32> base_;
    float amp_ = 0.2f, sigma_ = 0.f;
    int scenario_ = 0, chA_ = 6, chB_ = -1;
    double timeScale_ = 1.0;
    uint64_t idx_ = 0;
    Phase ph_[2];
};

} // namespace

double globalstarGenDoppler(double sec) {
    const double s = std::fmod(sec, kGsPassSec);
    return kGsPassDopplerHz * std::cos(kPi * s / kGsPassSec);
}

std::unique_ptr<ModeSynth> makeGlobalstarSynth(const SynthConfig& cfg, double sampleRate) {
    return std::make_unique<GlobalstarSynth>(cfg, sampleRate);
}

} // namespace dect2
