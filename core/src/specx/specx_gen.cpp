// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specx/specx_gen.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

namespace {

class SpecxSynth : public ModeSynth {
public:
    SpecxSynth(const SynthConfig& c, double fs) : fs_(fs), noise_(7) {
        // root-raised-cosine, 16 samples a symbol, 12 symbols long
        const int sps = (int)kSpecxSymDiv, span = 12, L = sps * span + 1;
        const double ro = 0.35;
        h_.resize((size_t)L);
        double e = 0;
        for (int i = 0; i < L; i++) {
            const double t = (i - (L - 1) / 2.0) / sps;
            double v;
            if (std::fabs(t) < 1e-9) v = 1 - ro + 4 * ro / M_PI;
            else if (std::fabs(std::fabs(4 * ro * t) - 1) < 1e-9)
                v = ro / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * ro)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * ro)));
            else v = (std::sin(M_PI * t * (1 - ro)) + 4 * ro * t * std::cos(M_PI * t * (1 + ro))) / (M_PI * t * (1 - std::pow(4 * ro * t, 2)));
            h_[(size_t)i] = v; e += v * v;
        }
        for (auto& v : h_) v /= std::sqrt(e / sps);   // unit power at the output for unit-power symbols
        sym_.assign((size_t)span + 1, std::complex<double>(0, 0));
        amp_ = 0.1;
        // complex noise power = carrier power / SNR * (fs / Rs)
        const double pn = amp_ * amp_ / std::pow(10.0, c.snrDb / 10) * kSpecxSymDiv;
        sigma_ = (float)std::sqrt(pn / 2);
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        const int sps = (int)kSpecxSymDiv, span = (int)sym_.size() - 1;
        // the two carriers as phasors turned by a fixed step, anchored at the start of every 4096 samples from the exact phase
        // (cos and sin of a growing time per sample were half the cost)
        const double fq = kSpecxQpskOffset, fc = kSpecxCwOffset;
        const std::complex<double> sq = std::polar(1.0, 2 * M_PI * fq), sc = std::polar(1.0, 2 * M_PI * fc);
        double qr = 0, qi = 0, cr = 0, ci = 0;
        for (size_t i = 0; i < n; i++, k_++) {
            if ((i & 4095) == 0) {
                const double t = (double)k_;
                const double aq = 2 * M_PI * std::fmod(fq * t, 1.0), ac = 2 * M_PI * std::fmod(fc * t, 1.0);
                qr = std::cos(aq); qi = std::sin(aq); cr = std::cos(ac); ci = std::sin(ac);
            }
            if (k_ % sps == 0) {
                lfsr_ = lfsr_ * 1103515245u + 12345u;
                const int b = (int)(lfsr_ >> 16) & 3;
                sym_.erase(sym_.begin());
                sym_.push_back(std::complex<double>(b & 1 ? 0.7071 : -0.7071, b & 2 ? 0.7071 : -0.7071));
            }
            const int ph = (int)(k_ % sps);
            double vr = 0, vi = 0;
            for (int j = 0; j <= span; j++) {
                const int idx = (span - j) * sps + ph;
                if (idx < (int)h_.size()) { vr += sym_[(size_t)j].real() * h_[(size_t)idx]; vi += sym_[(size_t)j].imag() * h_[(size_t)idx]; }
            }
            vr *= amp_; vi *= amp_;
            out[i] = cf32((float)(vr * qr - vi * qi + 0.03 * cr), (float)(vr * qi + vi * qr + 0.03 * ci));
            double t = qr * sq.real() - qi * sq.imag(); qi = qr * sq.imag() + qi * sq.real(); qr = t;
            t = cr * sc.real() - ci * sc.imag(); ci = cr * sc.imag() + ci * sc.real(); cr = t;
        }
        if (sigma_ > 1e-6f) noise_.add(out, n, sigma_);   // a transmitter (200 dB) has no noise worth making
    }

private:
    double fs_, amp_ = 0.1;
    float sigma_ = 0.01f;
    genutil::NoiseSource noise_;
    std::vector<double> h_;
    std::vector<std::complex<double>> sym_;
    uint64_t k_ = 0;
    uint32_t lfsr_ = 1;
};

} // namespace

std::unique_ptr<ModeSynth> makeSpecxSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<SpecxSynth>(cfg, sampleRate);
}

} // namespace dect2
