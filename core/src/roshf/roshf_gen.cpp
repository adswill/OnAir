// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) test signal, see roshf_gen.h
#include "dect2/roshf/roshf_gen.h"
#include "dect2/roshf/roshf_rx.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <random>

namespace dect2 {

struct RoshfGenerator::Impl {
    RoshfGenConfig c;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    double phase = 0, symPos = 0, tone = 0, t = 0, sigma = 0;
    int bit = 0;

    explicit Impl(const RoshfGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        if (c.snrDb < 99 || c.kind == 1) {
            // SNR in 2.5 kHz: noise power in 2.5 kHz = tone power / snr; per component sigma over the whole rate
            const double snr = c.kind == 1 ? 1e-3 : std::pow(10.0, c.snrDb / 10.0);
            const double nPow2500 = (c.amp * c.amp) / snr;
            sigma = std::sqrt(nPow2500 * c.rate / 2500.0 / 2);
        }
        pick();
    }
    void pick() {
        if (c.kind == 2) { bit = (int)(rng() & 1); tone = (bit ? 0.5 : -0.5) * 170.0; return; }
        if (c.kind == 3) { tone = 0; return; }
        std::uniform_int_distribution<int> d(0, c.tones - 1);
        tone = (d(rng) - (c.tones - 1) / 2.0) * c.spacingHz;
    }
    void generate(cf32* out, size_t n) {
        const double baud = (c.kind == 2 ? 45.45 : c.baud) * (1 + c.clockPpm * 1e-6);
        const double sps = c.rate / baud;
        const double sgn = c.lsb ? -1.0 : 1.0;
        for (size_t i = 0; i < n; i++) {
            symPos += 1.0;
            if (symPos >= sps) { symPos -= sps; pick(); }
            double re = 0, im = 0;
            if (c.kind != 1) {
                const double f = c.dialOffsetHz + sgn * (c.audioCentreHz + tone) + c.cfoHz + c.driftHzPerSec * t;
                phase += 2 * math::kPi * f / c.rate;
                if (phase > math::kPi) phase -= 2 * math::kPi; else if (phase < -math::kPi) phase += 2 * math::kPi;
                re = c.amp * std::cos(phase); im = c.amp * std::sin(phase);
            }
            if (sigma > 0) { re += sigma * nd(rng); im += sigma * nd(rng); }
            out[i] = cf32((float)re, (float)im);
            t += 1.0 / c.rate;
        }
    }
};

RoshfGenerator::RoshfGenerator(const RoshfGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
RoshfGenerator::~RoshfGenerator() = default;
void RoshfGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class RoshfSynth : public ModeSynth {
public:
    RoshfSynth(const SynthConfig& s, double rate) : rate_(rate) {
        RoshfGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -roshfTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.snrDb = s.snrDb;
        c.baud = s.modeVal[0] > 0 ? s.modeVal[0] : 16;
        c.kind = s.modeOpt[0];
        gen_ = std::make_unique<RoshfGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<RoshfGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeRoshfSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<RoshfSynth>(cfg, sampleRate); }

} // namespace dect2
