// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo test signal (see cquam_gen.h).
#include "dect2/cquam/cquam_gen.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;

class CquamSynth : public ModeSynth {
public:
    CquamSynth(const SynthConfig& s, double rate) : rate_(rate), noise_(11) {
        kind_ = s.modeOpt[0] >= 0 && s.modeOpt[0] <= 2 ? s.modeOpt[0] : 0;
        leftOff_ = s.modeOpt[1] == 1;
        carHz_ = -cquamTuning().tuneOffsetHz + s.modeVal[0] + s.cfoHz;
        fadeDepth_ = std::min(0.95, std::max(0.0, s.modeVal[1]));
        fadeHz_ = s.modeVal[2] > 0 ? s.modeVal[2] : 0.5;
        toneL_ = s.modeVal[3] > 0 ? s.modeVal[3] : 440.0;
        const double n10 = s.snrDb >= 99 ? 0 : kCquamSynthAmp * kCquamSynthAmp / std::pow(10.0, s.snrDb / 10);
        sigma_ = (float)std::sqrt(n10 * rate / 10000.0 / 2);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double dt = 1.0 / rate_, a = kCquamSynthAmp;
        const double step = 2 * kPi * carHz_ * dt;
        for (size_t i = 0; i < n; i++) {
            std::complex<double> z(0, 0);
            if (kind_ != 2) {
                const double L = leftOff_ ? 0.0 : 0.35 * std::sin(2 * kPi * toneL_ * t_);
                const double R = 0.35 * std::sin(2 * kPi * toneL_ * 1.5 * t_ + 0.7);
                const double m = L + R;
                // plain AM: the mono mix on the envelope; C-QUAM: the quadrature part L - R plus the pilot
                const double q = kind_ == 0 ? (L - R) + kCquamPilotDepth * std::sin(2 * kPi * 25.0 * t_) : 0.0;
                const double fade = fadeDepth_ > 0 ? 1.0 - fadeDepth_ * (0.5 - 0.5 * std::cos(2 * kPi * fadeHz_ * t_)) : 1.0;
                z = std::complex<double>(1 + m, q) * (a * fade);
            }
            const std::complex<double> c(std::cos(ph_), std::sin(ph_));
            const std::complex<double> y = z * c;
            out[i] = cf32((float)y.real(), (float)y.imag());
            ph_ += step;
            if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
            t_ += dt;
            if (t_ >= 1000.0) t_ -= 1000.0;   // the tones and the pilot are whole numbers of Hz: a jump by 1000 s is seamless for them
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }
private:
    double rate_, carHz_ = 0, t_ = 0, ph_ = 0, fadeDepth_ = 0, fadeHz_ = 0.5, toneL_ = 440;
    int kind_ = 0;
    bool leftOff_ = false;
    float sigma_ = 0;
    genutil::NoiseSource noise_;
};

} // namespace

std::unique_ptr<ModeSynth> makeCquamSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CquamSynth>(cfg, sampleRate);
}

} // namespace dect2
