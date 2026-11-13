// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging test signal (see multitone_gen.h).
#include "dect2/multitone/multitone_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/multitone/multitone_rx.h"
#include "dect2/pager/pager_proto.h"
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kTabRate = 48000.0;
constexpr double kCycle = 10.0;
}

std::vector<uint32_t> multitoneTestWords(int count) {
    std::vector<uint32_t> w;
    w.push_back(0xB5A3C961u);   // invented header word: not Multitone's, and not BCH protected
    uint32_t x = 0x1F2E3D;
    for (int i = 1; i < count; i++) {
        x = x * 1103515245u + 12345u;
        w.push_back(pager::encodeWord((x >> 8) & 0x1FFFFF));
    }
    return w;
}

std::vector<uint16_t> multitoneFfskWords() { return {0x605A, 0x605A, 0x7B91, 0x310E, 0xC47F, 0xFFFF}; }

struct MultitoneGenerator::Impl {
    MultitoneGenConfig c;
    std::vector<float> aud;      // one cycle of audio (deviation as a fraction of the carrier's: Hz)
    std::vector<uint8_t> on;     // carrier on
    std::mt19937 rng;
    dect2::normal_distribution<float> nd{0.f, 1.f};
    uint64_t n = 0;
    double phase = 0, carPhase = 0;
    double noiseSigma = 0;
    static constexpr float kAmp = 0.5f;

    explicit Impl(const MultitoneGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        const size_t N = (size_t)(kCycle * kTabRate);
        aud.assign(N, 0.f);
        on.assign(N, 0);
        auto at = [&](double s) { return std::min(N, (size_t)(s * kTabRate)); };
        if (c.content == 0 || c.content == 2) {
            const double base = c.content == 0 ? 1.0 : 0.3;
            double t = base;
            struct T { double hz, dur; } seq[] = {{238.3, 0.5}, {474.6, 0.5}, {1000.0, 0.4}};
            const size_t a = at(base - 0.2);
            for (size_t i = a; i < at(base + 2.1) && i < N; i++) on[i] = 1;
            for (const T& s : seq) {
                for (size_t i = at(t); i < at(t + s.dur) && i < N; i++) aud[i] = (float)(c.devTone * std::sin(2 * kPi * s.hz * ((double)i / kTabRate)));
                t += s.dur + 0.1;
            }
        }
        if (c.content == 0 || c.content == 3 || c.content == 4) {   // (content 5: below)
            std::vector<uint8_t> bits;
            if (c.content == 4) bits = pager::pocsagBits({{1234567, 0, kPagerNumeric, "12345"}}, 288);
            else {
                for (int i = 0; i < 288; i++) bits.push_back(i & 1 ? 0 : 1);   // preamble
                for (uint32_t w : multitoneTestWords(c.words)) for (int k = 31; k >= 0; k--) bits.push_back((w >> k) & 1u);
            }
            const double start = c.content == 0 ? 4.5 : 0.5;
            const double bit = kTabRate / c.baud;
            const size_t i0 = at(start), i1 = std::min(N, i0 + (size_t)(bits.size() * bit) + (size_t)(0.1 * kTabRate));
            for (size_t i = i0; i < i1; i++) on[i] = 1;
            for (size_t i = i0; i < i1; i++) {
                const size_t k = (size_t)((double)(i - i0) / bit);
                const int b = k < bits.size() ? bits[k] : 0;
                aud[i] = (float)((((b ^ (c.invert ? 1 : 0)) ? 1 : -1)) * c.devData);
            }
            // pulse shaping: a one-pole low-pass at 1.6 baud, two passes
            const double a = 1 - std::exp(-2 * kPi * 1.6 * c.baud / kTabRate);
            for (int pass = 0; pass < 2; pass++) {
                float y = aud[i0];
                for (size_t i = i0; i < std::min(N, i1 + (size_t)(0.02 * kTabRate)); i++) { y += (float)a * (aud[i] - y); aud[i] = y; }
            }
        }
        if (c.content == 5) {
            // audio FFSK: a symbol lasts 1.7084 ms (585.3 baud): a 1 is one cycle of 585.3 Hz, a 0 half a cycle of 292.7 Hz; phase continuous
            std::vector<uint8_t> bits(24, 1);
            for (uint16_t w : multitoneFfskWords()) for (int k = 15; k >= 0; k--) bits.push_back((w >> k) & 1u);
            for (int k = 0; k < 16; k++) bits.push_back(1);
            const double tsym = 2.0 / 1170.6, start = 0.5;
            const size_t i0 = at(start), i1 = std::min(N, i0 + (size_t)((double)bits.size() * tsym * kTabRate) + (size_t)(0.05 * kTabRate));
            double ph = 0;
            for (size_t i = i0; i < i1; i++) {
                const size_t k = std::min(bits.size() - 1, (size_t)((double)(i - i0) / (tsym * kTabRate)));
                const double f = bits[k] ? 585.3 : 292.65;
                ph += 2 * kPi * f / kTabRate;
                aud[i] = (float)(c.devData * 0.28 * std::sin(ph));
                on[i] = 1;
            }
        }
        if (c.content == 1) for (auto& v : on) v = 0;
        // noise per sample so that carrier amplitude^2 / noise power in 20 kHz = snr
        const double snr = std::pow(10.0, c.snrDb / 10.0);
        noiseSigma = c.snrDb >= 150 ? 0.0 : std::sqrt(0.5 * (double)kAmp * kAmp / snr * (c.rate / 20000.0));   // 200 dB: a transmitter
    }

    void generate(cf32* out, size_t cnt) {
        const size_t N = aud.size();
        for (size_t i = 0; i < cnt; i++, n++) {
            const double t = (double)n / c.rate;
            const double tp = std::fmod(t, kCycle) * kTabRate;
            const size_t k = std::min(N - 1, (size_t)tp);
            const size_t k2 = std::min(N - 1, k + 1);
            const double fr = tp - (double)k;
            const double a = aud[k] * (1 - fr) + aud[k2] * fr;
            cf32 s(0, 0);
            if (on[k]) {
                phase += 2 * kPi * (a + c.carrierOffsetHz) / c.rate;
                if (phase > kPi) phase -= 2 * kPi; else if (phase < -kPi) phase += 2 * kPi;
                s = cf32(kAmp * (float)std::cos(phase), kAmp * (float)std::sin(phase));
            }
            out[i] = noiseSigma > 0 ? s + cf32{(float)noiseSigma * nd(rng), (float)noiseSigma * nd(rng)} : s;
        }
    }
};

MultitoneGenerator::MultitoneGenerator(const MultitoneGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
MultitoneGenerator::~MultitoneGenerator() = default;
void MultitoneGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
double MultitoneGenerator::cycleSec() const { return kCycle; }

namespace {
class MultitoneSynth : public ModeSynth {
public:
    MultitoneSynth(const SynthConfig& cfg, double fs) {
        MultitoneGenConfig c;
        c.rate = fs;
        c.snrDb = cfg.snrDb >= 150 ? cfg.snrDb : cfg.snrDb > 0 ? std::min(60.0, (double)cfg.snrDb) : 30;   // 200 dB (a transmitter): no noise at all, not -60 dB of it over the whole span
        c.content = cfg.modeOpt[0];
        c.carrierOffsetHz = cfg.cfoHz - multitoneTuning().tuneOffsetHz;   // the radio sits above the channel (the receiver mixes the offset out)
        c.baud = cfg.modeOpt[1] == 1 ? 512 : cfg.modeOpt[1] == 2 ? 2400 : 1200;
        gen_ = std::make_unique<MultitoneGenerator>(c);
        fs_ = fs;
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    std::unique_ptr<MultitoneGenerator> gen_;
    double fs_ = 2e6;
};
}

std::unique_ptr<ModeSynth> makeMultitoneSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<MultitoneSynth>(cfg, sampleRate);
}

} // namespace dect2
