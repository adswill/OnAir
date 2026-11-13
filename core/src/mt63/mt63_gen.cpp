// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 test signal (see mt63_gen.h).
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_proto.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFg = 16000;   // the internal rate of the sum of the signals

struct Sender {
    Mt63GenSignal s;
    mt63::Encoder enc;
    double amp = 0, T = 0, R = 0, spacing = 0;
    int64_t n = 0, m = -1;
    std::array<std::array<int8_t, mt63::kCarriers>, 4> sg;
    std::array<int8_t, mt63::kCarriers> cur;
    std::complex<float> c[mt63::kCarriers], st[mt63::kCarriers];
    double comPh = 0;
    std::vector<std::complex<float>> echo;
    size_t echoPos = 0, echoLen = 0;
    // text supply
    enum { Lead, Text, Gap } phase = Lead;
    int64_t left = 0;
    size_t idx = 0;
    explicit Sender(const Mt63GenSignal& g, double rate, double sigma) : s(g), enc(g.longInterleave, g.seed) {
        R = mt63::internalRate(s.bandwidth);
        T = mt63::symbolSec(s.bandwidth) * (1 + s.clockPpm * 1e-6);
        spacing = mt63::carrierSpacingHz(s.bandwidth);
        double e = 0;
        for (int i = 0; i < mt63::kFft; i++) { const double p = mt63::pulse(i); e += p * p; }
        e /= mt63::kSymSep;
        const double snr = std::pow(10.0, s.snrDb / 10);
        amp = std::sqrt(snr * 2 * sigma * sigma * s.bandwidth / rate / (mt63::kCarriers * e));
        cur.fill(1);
        for (auto& a : sg) a.fill(1);
        for (int i = 0; i < mt63::kCarriers; i++) {
            c[i] = std::complex<float>(1, 0);
            st[i] = std::polar(1.f, (float)(2 * kPi * (i - 32) * spacing / kFg));
        }
        left = (int64_t)std::lround(s.startSec / T);
        if (s.echoMs > 0) { echoLen = (size_t)std::max(1.0, std::round(s.echoMs * 1e-3 * kFg)); echo.assign(echoLen, {0, 0}); }
    }
    int nextCode() {
        if (phase == Lead || phase == Gap) {
            if (left-- > 0) return 0;
            if (phase == Gap && !s.repeat) { left = 1 << 30; return 0; }
            phase = Text; idx = 0;
        }
        if (idx < s.text.size()) return (unsigned char)s.text[idx++] & 127;
        phase = Gap; left = (int64_t)std::lround(s.pauseSec / T);
        return 0;
    }
    void newSymbol() {
        m++;
        uint8_t bits[mt63::kCarriers];
        enc.process(nextCode(), bits);
        auto& slot = sg[(size_t)(m & 3)];
        for (int i = 0; i < mt63::kCarriers; i++) cur[(size_t)i] = slot[(size_t)i] = (int8_t)(cur[(size_t)i] * (bits[i] ? 1 : -1));
    }
    std::complex<float> sample() {
        const double t = (double)n / kFg;
        while ((double)(m + 1) * T <= t) newSymbol();
        std::complex<float> out(0, 0);
        const double span = mt63::kFft / R;
        for (int64_t a = m; a > m - 4 && a >= 0; a--) {
            const double u = t - (double)a * T;
            if (u < 0 || u >= span) continue;
            const double w = mt63::pulse(u * R);
            float re = 0, im = 0;
            const auto& g = sg[(size_t)(a & 3)];
            for (int i = 0; i < mt63::kCarriers; i++) { re += g[(size_t)i] * c[i].real(); im += g[(size_t)i] * c[i].imag(); }
            out += std::complex<float>(re, im) * (float)w;
        }
        for (int i = 0; i < mt63::kCarriers; i++) c[i] *= st[i];
        if ((n & 4095) == 4095) for (int i = 0; i < mt63::kCarriers; i++) c[i] /= std::abs(c[i]);
        comPh += 2 * kPi * (s.centreHz + s.driftHzPerMin * t / 60) / kFg;
        if (comPh > 1e6) comPh = std::remainder(comPh, 2 * kPi);
        double g = amp;
        if (s.fadeDepth > 0) g *= 1 - s.fadeDepth * (0.5 - 0.5 * std::cos(2 * kPi * s.fadeHz * t));
        out *= (float)g * std::polar(1.f, (float)std::remainder(comPh, 2 * kPi));
        if (echoLen) {
            const std::complex<float> d = echo[echoPos];
            echo[echoPos] = out;
            echoPos = (echoPos + 1) % echoLen;
            out += d * (float)std::pow(10.0, s.echoDb / 20) * std::polar(1.f, 1.3f);
        }
        n++;
        return out;
    }
};
} // namespace

struct Mt63Generator::Impl {
    Mt63GenConfig cfg;
    genutil::NoiseSource noise;
    std::vector<Sender> snd;
    std::complex<float> h[4];
    double frac = 0, step = 0, ph = 0, nowT = 0;
    explicit Impl(const Mt63GenConfig& c) : cfg(c), noise(c.seed) {
        const double sigma = c.noiseSigma > 0 ? c.noiseSigma : 0.02;
        for (const auto& g : c.signals) snd.emplace_back(g, c.rate, sigma);
        step = kFg / c.rate;
        for (int i = 0; i < 4; i++) h[i] = next16();
    }
    std::complex<float> next16() {
        std::complex<float> v(0, 0);
        for (Sender& s : snd) v += s.sample();
        return v;
    }
};

Mt63Generator::Mt63Generator(const Mt63GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Mt63Generator::~Mt63Generator() = default;

void Mt63Generator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double w = 2 * kPi * (m.cfg.dialOffsetHz + m.cfg.cfoHz) / m.cfg.rate;
    for (size_t i = 0; i < n; i++) {
        const float f = (float)m.frac;
        const std::complex<float> p0 = m.h[0], p1 = m.h[1], p2 = m.h[2], p3 = m.h[3];
        const std::complex<float> v = 0.5f * (2.f * p1 + (p2 - p0) * f + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * f * f + (3.f * (p1 - p2) + p3 - p0) * f * f * f);
        m.ph += w;
        if (m.ph > kPi) m.ph -= 2 * kPi; else if (m.ph < -kPi) m.ph += 2 * kPi;
        out[i] = v * std::polar(1.f, (float)m.ph);
        m.frac += m.step;
        while (m.frac >= 1) { m.frac -= 1; m.h[0] = m.h[1]; m.h[1] = m.h[2]; m.h[2] = m.h[3]; m.h[3] = m.next16(); }
    }
    if (m.cfg.noiseSigma > 0) m.noise.add(out, n, (float)m.cfg.noiseSigma);
}

std::vector<Mt63GenSignal> mt63TestSignals() {
    std::vector<Mt63GenSignal> v(1);
    v[0].bandwidth = 1000; v[0].longInterleave = false; v[0].centreHz = 1500; v[0].snrDb = 8;
    v[0].text = "CQ CQ CQ DE LY2ABC LY2ABC KO24 MT63 TEST PSE K\n";
    return v;
}

namespace {
class Mt63Synth : public ModeSynth {
public:
    Mt63Synth(const SynthConfig& s, double rate) : rate_(rate) {
        Mt63GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -mt63Tuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.noiseSigma = 0.05;
        c.signals = mt63TestSignals();
        if (s.modeOpt[0] == 1) c.signals.resize(1);
        if (s.snrDb >= 150) {
            // a transmitter (cleanSynth asks for 200 dB): no noise and the strongest signal at an rms of 0.15 (MT63 is 64 carriers: about 10 dB
            // peak-to-average, so the peaks stay under the output limiter's knee). Adding 200 dB made it 10^10 too strong.
            c.noiseSigma = 0;
            double top = -1e9;
            for (const auto& g : c.signals) top = std::max(top, g.snrDb + 10 * std::log10(g.bandwidth));
            const double off = 20 * std::log10(0.15) - 10 * std::log10(2 * 0.02 * 0.02 / rate) - top;   // power = 2 sigma^2 bandwidth / rate 10^(snr/10)
            for (auto& g : c.signals) g.snrDb += off;
        } else
            for (auto& g : c.signals) g.snrDb += s.snrDb;
        gen_ = std::make_unique<Mt63Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<Mt63Generator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeMt63Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<Mt63Synth>(cfg, sampleRate);
}

} // namespace dect2
