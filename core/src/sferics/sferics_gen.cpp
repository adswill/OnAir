// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the Sferics mode (sferics_gen.h): noise, damped-oscillation sferics, an optional mains impulse train.
#include "dect2/sferics/sferics_gen.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

namespace {
// one damped oscillation of peak amplitude amp starting at sample i0
void addSferic(std::vector<cf32>& out, size_t i0, double rate, double amp, double tauUs, double oscFrac, double phase) {
    const double tau = tauUs * 1e-6 * rate;
    const size_t len = (size_t)(tau * 7) + 2;
    for (size_t k = 0; k < len && i0 + k < out.size(); k++) {
        const double e = std::exp(-(double)k / tau) * std::min(1.0, (double)(k + 1) / 2.0);
        const double ph = 2 * math::kPi * oscFrac * (double)k + phase;
        out[i0 + k] += cf32((float)(amp * e * std::cos(ph)), (float)(amp * e * std::sin(ph)));
    }
}
}

std::vector<cf32> sfericsRender(const SfericsScene& s, double rate) {
    const size_t n = (size_t)std::llround(s.seconds * rate);
    std::vector<cf32> out(n, cf32(0, 0));
    std::mt19937 rng(s.seed);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::uniform_real_distribution<double> ud(0.0, 2 * math::kPi);
    const double sigma = s.noiseRms / std::sqrt(2.0);
    // amplitude whose smoothed power sits snrDb over the noise: peak power = amp^2, noise power = noiseRms^2
    auto ampFor = [&](double snrDb) { return std::min(0.9, s.noiseRms * std::pow(10.0, snrDb / 20.0)); };
    for (size_t i = 0; i < s.strikeT.size(); i++) {
        const size_t i0 = (size_t)std::llround(s.strikeT[i] * rate);
        if (i0 < n) addSferic(out, i0, rate, ampFor(s.strikeSnrDb[i]), s.tauUs, s.oscFrac, ud(rng));
    }
    if (s.mainsRate > 0)
        for (double t = s.mainsPhase; t < s.seconds; t += 1.0 / s.mainsRate) {
            const size_t i0 = (size_t)std::llround(t * rate);
            if (i0 < n) addSferic(out, i0, rate, ampFor(s.mainsSnrDb), 30.0, 0.2, ud(rng));
        }
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        cf32 v = out[i] + cf32(nd(rng) * (float)sigma, nd(rng) * (float)sigma);
        if (s.cfoHz != 0) {
            ph += 2 * math::kPi * s.cfoHz / rate;
            if (ph > math::kPi) ph -= 2 * math::kPi;
            v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        out[i] = v;
    }
    return out;
}

namespace {
class SfericsSynth : public ModeSynth {
public:
    SfericsSynth(const SynthConfig& c, double rate) : rate_(rate), cfg_(c), rng_(11) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (pos_ >= cur_.size()) next();
            out[i] = cur_[pos_++];
        }
    }
private:
    void next() {
        SfericsScene s;
        s.seconds = 1.0;
        s.seed = (uint32_t)(sec_ * 7919 + 3);
        s.cfoHz = cfg_.cfoHz;
        const double snr = cfg_.snrDb >= 99 ? 30 : std::max(6.0, cfg_.snrDb);
        const int mode = cfg_.modeOpt[0];
        if (mode == 0 || mode == 2) {
            const double rate = cfg_.modeVal[0] > 0 ? cfg_.modeVal[0] : 1.5;   // strikes per second
            std::exponential_distribution<double> ed(rate / 1.7);               // flashes; about 1.7 strokes each
            std::uniform_int_distribution<int> nstroke(1, 3);
            std::uniform_real_distribution<double> ud(0.0, 1.0);
            for (double t = ed(rng_); t < 0.9; t += ed(rng_)) {
                double ts = t;
                for (int k = nstroke(rng_); k > 0; k--, ts += 0.04 + 0.04 * ud(rng_))
                    if (ts < 0.95) { s.strikeT.push_back(ts); s.strikeSnrDb.push_back(snr - 8 * ud(rng_)); }
            }
        }
        if (mode == 1 || mode == 2) {
            s.mainsRate = cfg_.modeOpt[1] == 1 ? 120.0 : 100.0;
            s.mainsPhase = 0.0037;
            s.mainsSnrDb = 20;
        }
        sec_++;
        cur_ = sfericsRender(s, rate_);
        pos_ = 0;
    }
    double rate_;
    SynthConfig cfg_;
    std::mt19937 rng_;
    std::vector<cf32> cur_;
    size_t pos_ = 0;
    uint64_t sec_ = 0;
};
}

std::unique_ptr<ModeSynth> makeSfericsSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<SfericsSynth>(cfg, sampleRate); }

}  // namespace dect2
