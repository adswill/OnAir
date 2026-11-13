// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ / IFKP test signal (see kbd33_gen.h).
#include "dect2/fsq/kbd33_gen.h"
#include "dect2/fsq/kbd33.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

std::string fsqFrame(const std::string& call, const std::string& body, bool directed) {
    return " \n" + call + ":" + kbd33::crc8hex(call) + body + (directed ? "  \b  " : "\n ");
}

namespace {
using dect2::math::kPi;
constexpr double kFg = 16000;   // the internal rate of the sum of the signals

struct Sender {
    Kbd33GenSignal s;
    kbd33::Spec spec;
    std::vector<int> seq;
    double amp = 0, T = 0, env = 0;
    static constexpr double kRamp = 80;   // samples at kFg
    int64_t n = 0, m = -1;
    int tone = 0;
    bool active = false;
    double ph = 0;
    enum { Lead, Text, Gap } phase = Lead;
    int64_t left = 0;
    size_t idx = 0;
    std::vector<std::complex<float>> echo;
    size_t echoPos = 0, echoLen = 0;
    explicit Sender(const Kbd33GenSignal& g, double rate, double sigma) : s(g), spec(kbd33::specOf(g.ifkp, g.speed)) {
        T = spec.symSec * (1 + s.clockPpm * 1e-6);
        const double snr = std::pow(10.0, s.snrDb / 10);
        amp = s.amplitude > 0 ? s.amplitude : std::sqrt(snr * 2 * sigma * sigma * spec.bandHz() / rate);
        seq = kbd33::toneSequence(g.ifkp, g.text, g.startTone, 2);
        left = (int64_t)std::lround(s.startSec / T);
        if (s.echoMs > 0) { echoLen = (size_t)std::max(1.0, std::round(s.echoMs * 1e-3 * kFg)); echo.assign(echoLen, {0, 0}); }
    }
    void newSymbol() {
        m++;
        active = false;
        if (phase == Lead || phase == Gap) {
            if (left-- > 0) return;
            if (phase == Gap && !s.repeat) { left = 1LL << 40; return; }
            phase = Text; idx = 0;
        }
        if (idx < seq.size()) { tone = seq[idx++]; active = true; return; }
        phase = Gap; left = (int64_t)std::lround(s.pauseSec / T);
    }
    std::complex<float> sample() {
        const double t = (double)n / kFg;
        while ((double)(m + 1) * T <= t) newSymbol();
        const double f = s.centreHz + (tone - 16) * spec.spacingHz + s.driftHzPerMin * t / 60;
        ph += 2 * kPi * f / kFg;
        if (ph > 1e6) ph = std::fmod(ph, 2 * kPi);
        std::complex<float> out(0, 0);
        // a 5 ms raised-cosine key-up and key-down at the start and the end of the text, instead of a step that splatters
        env = active ? std::min(1.0, env + 1.0 / kRamp) : std::max(0.0, env - 1.0 / kRamp);
        if (env > 0) {
            double g = amp * (0.5 - 0.5 * std::cos(kPi * env));
            if (s.fadeDepth > 0) g *= 1 - s.fadeDepth * (0.5 - 0.5 * std::cos(2 * kPi * s.fadeHz * t));
            out = std::polar((float)g, (float)std::fmod(ph, 2 * kPi));
        }
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

struct Kbd33Generator::Impl {
    Kbd33GenConfig cfg;
    genutil::NoiseSource noise;
    std::vector<Sender> snd;
    std::complex<float> h[4];
    double frac = 0, step = 0, ph = 0;
    explicit Impl(const Kbd33GenConfig& c) : cfg(c), noise(c.seed) {
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

Kbd33Generator::Kbd33Generator(const Kbd33GenConfig& c) : p_(std::make_unique<Impl>(c)) {}
Kbd33Generator::~Kbd33Generator() = default;

void Kbd33Generator::generate(cf32* out, size_t n) {
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

} // namespace dect2
