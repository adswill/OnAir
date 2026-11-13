// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General receiver test signal (see general_gen.h).
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;

class GeneralSynth : public ModeSynth {
public:
    GeneralSynth(const SynthConfig& s, double rate) : rate_(rate), noise_(7) {
        demod_ = s.modeOpt[0] >= 0 && s.modeOpt[0] < GenDemodCount ? s.modeOpt[0] : GenNfm;
        quiet_ = s.modeOpt[1] == 1;
        carHz_ = -generalTuning().tuneOffsetHz + s.modeVal[0] + s.cfoHz;
        toneHz_ = s.modeVal[1] > 0 ? s.modeVal[1] : 1000.0;
        frac_ = s.modeVal[2] > 0 ? s.modeVal[2] : 0.5;
        const double a = kGeneralSynthAmp;
        // noise: amp^2 / snr in 12.5 kHz, spread over the whole rate (complex variance 2 sigma^2)
        const double n125 = s.snrDb >= 99 ? 0 : a * a / std::pow(10.0, s.snrDb / 10);
        sigma_ = (float)std::sqrt(n125 * rate / 12500.0 / 2);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double dt = 1.0 / rate_, a = kGeneralSynthAmp;
        for (size_t i = 0; i < n; i++) {
            const double tone = std::sin(2 * kPi * toneHz_ * t_);
            double ph = 2 * kPi * carHz_ * t_, amp = a;
            switch (demod_) {
            case GenNfm: case GenWfm: {
                const double dev = frac_ * (demod_ == GenNfm ? 5000.0 : 75000.0);
                // the phase of the tone: integral of dev * sin = -dev / f * cos
                ph += -dev / toneHz_ * std::cos(2 * kPi * toneHz_ * t_);
                break;
            }
            case GenAm: amp = a * (1 + frac_ * tone); break;
            case GenUsb: ph += 2 * kPi * toneHz_ * t_; break;
            case GenLsb: ph -= 2 * kPi * toneHz_ * t_; break;
            case GenDsb: amp = a * tone; break;   // both sidebands, no carrier
            default: break;   // CW, CW-L, RAW: a steady carrier
            }
            out[i] = quiet_ ? cf32(0, 0) : cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
            t_ += dt;
            if (t_ >= 1000.0) t_ -= 1000.0;   // whole seconds: every frequency used is a whole number of Hz... close enough for a test
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }
private:
    double rate_, carHz_ = 0, toneHz_ = 1000, frac_ = 0.5, t_ = 0;
    int demod_ = 0;
    bool quiet_ = false;
    float sigma_ = 0;
    genutil::NoiseSource noise_;
};

} // namespace

std::unique_ptr<ModeSynth> makeGeneralSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<GeneralSynth>(cfg, sampleRate);
}

} // namespace dect2
