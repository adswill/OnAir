// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals of the satellite detector modes. Nothing is transmitted; the samples only exist in memory or a file.
#include "dect2/satmisc/satdetect_live.h"
#include <cmath>
#include <random>
#include <vector>

namespace dect2 {
using namespace satmisc;

namespace {
class SatDetectSynth : public ModeSynth {
public:
    SatDetectSynth(const SatSystem& s, const SynthConfig& c, double rate) : s_(s), rate_(rate), g_(77), n_(0.f, 1.f), u_(0.f, 1.f) {
        on_ = c.modeOpt[0] != 1;
        // noise power 1 in the whole capture; snrDb is the signal over the noise in 1 kHz for the narrow methods, in the whole capture for the framed ones
        const double snr = std::pow(10.0, c.snrDb / 10);
        amp_ = (s.method == SdMethod::FrameGap || s.method == SdMethod::RepeatSeq) ? std::sqrt(snr) : std::sqrt(snr * 1e3 / rate);
        noiseAmp_ = 0.05f;
        if (c.cfoHz != 0) base_ = c.cfoHz;
        scale_ = noiseAmp_ / std::sqrt(2.f);
        frame_ = rate / (s.frameHz > 0 ? s.frameHz : 1);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            cf32 x(0, 0);
            if (on_) {
                if (s_.method == SdMethod::Carrier) x = carrier();
                else if (s_.method == SdMethod::Channel) x = channel();
                else if (s_.method == SdMethod::FrameGap) x = ofdmGapSample();
                else x = repeatSample();
            }
            t_ += 1 / rate_;
            out[i] = x * noiseAmp_ + cf32(n_(g_), n_(g_)) * scale_;
        }
    }
private:
    cf32 carrier() {
        // a satellite-like S curve: +-3 kHz over about 10 minutes
        const double f = base_ + 3000 * std::tanh((300 - t_) / 120);
        ph_ += 2 * M_PI * f / rate_;
        if (ph_ > M_PI) ph_ -= 2 * M_PI;
        return cf32((float)(amp_ * std::cos(ph_)), (float)(amp_ * std::sin(ph_)));
    }
    cf32 channel() {
        const double ch = s_.channelHz;
        const double f[3] = {3 * ch + base_, -7 * ch + base_, 12 * ch + base_};
        const double period[3] = {4.0, 7.0, 3.0}, dur[3] = {1.5, 2.0, 1.0};
        cf32 x(0, 0);
        for (int k = 0; k < 3; k++) {
            ph3_[k] += 2 * M_PI * (f[k] + 800 * std::sin(2 * M_PI * 600 * t_)) / rate_;
            if (std::fmod(t_ + k, period[k]) < dur[k]) x += cf32((float)(amp_ * std::cos(ph3_[k])), (float)(amp_ * std::sin(ph3_[k])));
        }
        return x;
    }
    // A slice of an OFDM signal, 302 symbols and a guard per frame: random QPSK on the subcarriers inside the capture, and nothing in the guard (public Starlink numbers).
    cf32 ofdmGapSample() {
        if (fr_ < 0) newFrame();
        const double pos = frameT_;
        frameT_ += 1;
        if (frameT_ >= frame_) { frameT_ -= frame_; }
        if (pos >= frame_ * (302.0 * 1056.0 / 320000.0)) return cf32(0, 0);
        if (symPos_ >= sym_.size()) makeSymbol();
        return sym_[symPos_++] * (float)amp_;
    }
    void newFrame() { fr_ = 0; makeSymbol(); }
    void makeSymbol() {
        const size_t N = (size_t)std::round(1056.0 / 240e6 * rate_);
        sym_.assign(N, cf32(0, 0));
        const int K = 32;
        for (int k = 0; k < K; k++) {
            const double re = u_(g_) < 0.5 ? 1 : -1, im = u_(g_) < 0.5 ? 1 : -1;
            const double w = 2 * M_PI * (k - K / 2) / (double)N;
            for (size_t i = 0; i < N; i++) { const double p = w * (double)i; sym_[i] += cf32((float)(re * std::cos(p) - im * std::sin(p)), (float)(re * std::sin(p) + im * std::cos(p))); }
        }
        const float sc = 1.f / std::sqrt((float)K * 2);
        for (auto& v : sym_) v *= sc;
        symPos_ = 0;
    }
    // Random QPSK with a 40 us sequence repeated every period
    cf32 repeatSample() {
        const size_t period = (size_t)std::lround(rate_ / s_.frameHz), slen = (size_t)(40e-6 * rate_);
        if (seq_.empty()) { for (size_t i = 0; i < slen; i++) seq_.push_back(qpsk()); }
        const size_t p = pos_++ % period;
        return (p < slen ? seq_[p] : qpsk()) * (float)amp_;
    }
    cf32 qpsk() { return cf32(u_(g_) < 0.5f ? 0.707f : -0.707f, u_(g_) < 0.5f ? 0.707f : -0.707f); }
    SatSystem s_;
    double rate_, amp_ = 0, base_ = 20000, t_ = 0, ph_ = 0, ph3_[3] = {0, 0, 0}, frame_ = 0, frameT_ = 0;
    float noiseAmp_ = 0.05f, scale_ = 0;
    bool on_ = true;
    int fr_ = -1;
    size_t symPos_ = 0, pos_ = 0;
    std::vector<cf32> sym_, seq_;
    std::mt19937 g_;
    std::normal_distribution<float> n_;
    std::uniform_real_distribution<float> u_;
};
} // namespace

std::unique_ptr<ModeSynth> makeSatDetectSynth(int stdMode, const SynthConfig& cfg, double sampleRate) {
    const SatSystem* s = satSystem(stdMode);
    if (!s || sampleRate <= 0) return nullptr;
    return std::make_unique<SatDetectSynth>(*s, cfg, sampleRate);
}

} // namespace dect2
