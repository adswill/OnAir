// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/astro/astro_gen.h"
#include "dect2/astro/astro_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <vector>

namespace dect2 {

namespace {

// noise shaped in blocks: white Gaussian noise, FFT, times the square root of the wanted spectrum, inverse FFT
class AstroSynth : public ModeSynth {
public:
    AstroSynth(const SynthConfig& c, double fs) : fs_(fs), fft_(kL) {
        gain_.resize(kL);
        const double fl = astro::freqForVelocity(kAstroLineKms, astro::kHiHz) - astro::kHiHz;
        const double sf = kAstroLineFwhmKms / astro::kC * astro::kHiHz / 2.3548;
        const double a = c.modeOpt[0] == 1 ? 0.0 : std::pow(10.0, kAstroLineDb / 10);
        for (int k = 0; k < kL; k++) {
            const int kk = k < kL / 2 ? k : k - kL;            // fft order: bin k is kk * fs / L
            const double f = kk * fs / kL, x = f / (fs / 2);
            const double base = 1 + kAstroSlope * x + kAstroCurve * x * x;
            const double line = a * std::exp(-0.5 * (f - fl) * (f - fl) / (sf * sf));
            gain_[(size_t)k] = (float)std::sqrt(base * (1 + line));
        }
        blk_.resize(kL);
        if (c.modeOpt[0] == 1) s_ ^= 0xD1B54A32D192ED03ull;   // off source: other noise than on source
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (pos_ >= kL) next();
            out[i] = blk_[(size_t)pos_++];
        }
    }

private:
    static constexpr int kL = 65536;
    double gauss() {
        if (have_) { have_ = false; return spare_; }
        double u, v;
        do { u = uni(); } while (u <= 1e-300);
        v = uni();
        const double r = std::sqrt(-2 * std::log(u));
        spare_ = r * std::sin(2 * M_PI * v); have_ = true;
        return r * std::cos(2 * M_PI * v);
    }
    double uni() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return (double)(s_ >> 11) * (1.0 / 9007199254740992.0); }
    void next() {
        const double sig = 0.2 / std::sqrt(2.0);   // rms 0.2
        for (int k = 0; k < kL; k++) blk_[(size_t)k] = cf32((float)(sig * gauss()), (float)(sig * gauss()));
        fft_.forward(blk_.data());
        for (int k = 0; k < kL; k++) blk_[(size_t)k] *= gain_[(size_t)k] / (float)kL;
        fft_.inverse(blk_.data());
        pos_ = 0;
    }
    double fs_;
    Fft fft_;
    std::vector<float> gain_;
    std::vector<cf32> blk_;
    int pos_ = kL;
    uint64_t s_ = 0x9E3779B97F4A7C15ull;
    bool have_ = false;
    double spare_ = 0;
};

} // namespace

std::unique_ptr<ModeSynth> makeAstroSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<AstroSynth>(cfg, sampleRate);
}

} // namespace dect2
