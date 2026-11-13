// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers test signal (inmsat_gen.h).
#include "dect2/inmsat/inmsat_gen.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_cgen.h"
#include "dect2/inmc/inmc_gen.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

namespace {
class InmsatSynth : public ModeSynth {
public:
    InmsatSynth(const SynthConfig& cfg, double rate) : fs_(rate) {
        const double ebn0 = cfg.modeVal[0] != 0 ? cfg.modeVal[0] : 17.0;
        SynthConfig a = cfg;
        a.mode = 20; a.modeOpt[0] = 7; a.modeVal[0] = ebn0;
        aero1_ = std::make_unique<AeroSynth>(a, rate);
        SynthConfig b = a;
        b.modeOpt[0] = 4; b.modeOpt[1] = 3;
        aero2_ = std::make_unique<AeroSynth>(b, rate);
        SynthConfig c = cfg;
        c.mode = 19; c.modeOpt[3] = 1; c.modeVal[0] = ebn0;
        inmc_ = makeInmcSynth(c, rate);
        shift_[0] = 0; shift_[1] = -0.23 * rate; shift_[2] = 0.15 * rate;
        wideOff_ = 0.30 * rate;
        if (cfg.modeOpt[2] == 1) {      // a real Aero C channel (voice frames, aero_cgen.h) in place of the stand-in
            AeroCGenConfig g;
            g.fs = rate; g.carrierHz = wideOff_; g.seed = cfg.modeOpt[1] ? (uint32_t)cfg.modeOpt[1] : 1;
            g.amplitude = aero1_->noiseSigma() * std::sqrt(2.0 * 8400.0 * std::pow(10.0, ebn0 / 10) / rate);
            cgen_ = std::make_unique<AeroCGenerator>(g);
        }
        const double bw = 3000;
        pole_ = 1.0 - 2.0 * M_PI * bw / rate * 0.8;
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        b1_.resize(n); b2_.resize(n); b3_.resize(n);
        if (cgen_) { b4_.resize(n); cgen_->generate(b4_.data(), n); }
        aero1_->generate(b1_.data(), n);
        aero2_->generate(b2_.data(), n);
        inmc_->generate(b3_.data(), n);
        const double w2 = 2 * M_PI * shift_[1] / fs_, w3 = 2 * M_PI * shift_[2] / fs_, ww = 2 * M_PI * wideOff_ / fs_;
        for (size_t i = 0; i < n; i++) {
            const cf32 r2((float)std::cos(ph2_), (float)std::sin(ph2_)), r3((float)std::cos(ph3_), (float)std::sin(ph3_));
            ph2_ += w2; ph3_ += w3; phw_ += ww;
            if (ph2_ > 2 * M_PI) ph2_ -= 2 * M_PI; if (ph2_ < -2 * M_PI) ph2_ += 2 * M_PI;
            if (ph3_ > 2 * M_PI) ph3_ -= 2 * M_PI; if (ph3_ < -2 * M_PI) ph3_ += 2 * M_PI;
            if (phw_ > 2 * M_PI) phw_ -= 2 * M_PI;
            // the wide carrier: two-pole filtered noise
            const cf32 wn(gauss(), gauss());
            y1_ = (float)pole_ * y1_ + wn; y2_ = (float)pole_ * y2_ + y1_;
            const cf32 wide = y2_ * (float)(0.05 * (1 - pole_) * (1 - pole_)) * cf32((float)std::cos(phw_), (float)std::sin(phw_));
            cf32 v = 0.55f * (b1_[i] + b2_[i] * r2 + b3_[i] * r3) + (cgen_ ? 0.55f * b4_[i] : wide * 0.2f);
            v = cf32(std::max(-0.9f, std::min(0.9f, v.real())), std::max(-0.9f, std::min(0.9f, v.imag())));
            out[i] = v;
        }
    }
private:
    float gauss() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; const float a = (float)((s_ >> 11) & 0xFFFFFF) / 16777216.f - 0.5f; s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; const float b = (float)((s_ >> 11) & 0xFFFFFF) / 16777216.f - 0.5f; return (a + b) * 2.449f; }
    double fs_, shift_[3] = {}, wideOff_ = 0, pole_ = 0.99, ph2_ = 0, ph3_ = 0, phw_ = 0;
    uint64_t s_ = 88172645463325252ull;
    cf32 y1_{}, y2_{};
    std::unique_ptr<AeroSynth> aero1_, aero2_;
    std::unique_ptr<ModeSynth> inmc_;
    std::vector<cf32> b1_, b2_, b3_, b4_;
    std::unique_ptr<AeroCGenerator> cgen_;
};
}

std::unique_ptr<ModeSynth> makeInmsatSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<InmsatSynth>(cfg, sampleRate); }

} // namespace dect2
