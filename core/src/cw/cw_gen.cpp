// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW (Morse) test signal (see cw_gen.h).
#include "dect2/cw/cw_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kEdgeSec = 0.004;
constexpr double kRefSigma = 0.02;
} // namespace

std::vector<double> cwKeying(const std::string& text, double wpm, double fw, double dashRatio) {
    const double u = 1.2 / std::max(1.0, wpm);
    double cg = 3 * u, wg = 7 * u;
    if (fw > 0 && fw < wpm) {
        const double ta = (60 * wpm - 37.2 * fw) / (fw * wpm);   // ARRL: the delay spread over the gaps
        cg = 3 * ta / 19; wg = 7 * ta / 19;
    }
    std::vector<double> d;
    for (const std::string& tok : cw::tokenize(text)) {
        if (tok == " ") { if (!d.empty()) d.back() = wg; continue; }
        const std::string code = cw::encodeSymbol(tok);
        if (code.empty()) continue;
        for (char c : code) { d.push_back(c == '.' ? u : dashRatio * u); d.push_back(u); }
        d.back() = cg;
    }
    if (!d.empty()) d.back() = wg;
    return d;
}

struct CwGenerator::Impl {
    struct Sig {
        CwGenSignal s;
        double amp = 0, ph = 0;
        std::vector<double> dur;     // this sending, jittered
        size_t idx = 0;
        double left = 0;             // seconds left of the current element (or of the start delay / pause)
        bool on = false, waiting = true, done = false;
        double edge = 0;             // 0 .. 1 key position
        std::mt19937 rng;
    };
    CwGenConfig cfg;
    std::vector<Sig> sigs;
    genutil::NoiseSource noise;
    int64_t n = 0;
    explicit Impl(const CwGenConfig& c) : cfg(c), noise(c.seed) {
        const double sigma = c.noiseSigma > 0 ? c.noiseSigma : kRefSigma;
        uint32_t k = 0;
        for (const auto& s : c.signals) {
            Sig g;
            g.s = s;
            g.amp = std::sqrt(std::pow(10.0, s.snrDb / 10) * 2 * sigma * sigma * 500.0 / c.rate);
            g.left = s.startSec;
            g.rng.seed(c.seed * 7919u + (k++) * 104729u);
            sigs.push_back(std::move(g));
        }
    }
    void newSending(Sig& g) {
        g.dur = cwKeying(g.s.text, g.s.wpm, g.s.farnsworthWpm, g.s.dashRatio);
        if (g.s.jitter > 0) {
            dect2::normal_distribution<double> nd(0.0, g.s.jitter);
            for (double& v : g.dur) v *= std::max(0.4, std::min(1.6, 1 + nd(g.rng)));
        }
        g.idx = 0;
        g.on = !g.dur.empty();
        g.left = g.dur.empty() ? 1 : g.dur[0];
    }
};

CwGenerator::CwGenerator(const CwGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
CwGenerator::~CwGenerator() = default;
double CwGenerator::timeSec() const { return (double)p_->n / p_->cfg.rate; }

double CwGenerator::messageSec(const CwGenSignal& s) {
    double t = 0;
    for (double v : cwKeying(s.text, s.wpm, s.farnsworthWpm, s.dashRatio)) t += v;
    return t;
}

void CwGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double dt = 1.0 / m.cfg.rate;
    std::fill(out, out + n, cf32(0.f, 0.f));
    for (auto& g : m.sigs) {
        for (size_t i = 0; i < n; i++) {
            if (!g.done) {
                g.left -= dt;
                while (g.left <= 0 && !g.done) {
                    if (g.waiting) { g.waiting = false; const double carry = g.left; m.newSending(g); g.left += carry; continue; }
                    if (++g.idx >= g.dur.size()) {
                        g.on = false;
                        if (!g.s.repeat) { g.done = true; break; }
                        g.waiting = true; g.left += g.s.pauseSec; continue;
                    }
                    g.on = (g.idx % 2) == 0;
                    g.left += g.dur[g.idx];
                }
            }
            const bool key = g.on && !g.waiting && !g.done;
            g.edge += (key ? 1 : -1) * dt / kEdgeSec;
            g.edge = std::max(0.0, std::min(1.0, g.edge));
            if (g.edge <= 0) continue;
            const double t = (double)(m.n + (int64_t)i) * dt;
            double a = g.amp * (0.5 - 0.5 * std::cos(kPi * g.edge));
            if (g.s.qsbDepthDb > 0 && g.s.qsbPeriodSec > 0) a *= std::pow(10.0, -g.s.qsbDepthDb / 20 * (0.5 - 0.5 * std::cos(2 * kPi * t / g.s.qsbPeriodSec)));
            const double f = m.cfg.dialOffsetHz + m.cfg.cfoHz + g.s.offsetHz + g.s.driftHzPerMin * t / 60;
            g.ph = std::remainder(g.ph + 2 * kPi * f * dt, 2 * kPi);
            out[i] += cf32((float)(a * std::cos(g.ph)), (float)(a * std::sin(g.ph)));
        }
    }
    m.n += (int64_t)n;
    if (m.cfg.noiseSigma > 0) m.noise.add(out, n, (float)m.cfg.noiseSigma);
}

std::vector<CwGenSignal> cwTestSignals() {
    std::vector<CwGenSignal> v(4);
    v[0].offsetHz = -3000; v[0].wpm = 20; v[0].text = "CQ CQ DE LY2ABC LY2ABC <AR> K"; v[0].snrDb = 0;
    v[1].offsetHz = -1200; v[1].wpm = 28; v[1].text = "TEST DL1XYZ DL1XYZ TEST"; v[1].snrDb = -3; v[1].startSec = 1.1;
    v[2].offsetHz = 400; v[2].wpm = 16; v[2].farnsworthWpm = 10; v[2].text = "LY2ABC DE G4ABC UR 599 <BT> NAME JOHN <KN>"; v[2].snrDb = -2;
    v[2].jitter = 0.06; v[2].qsbDepthDb = 10; v[2].qsbPeriodSec = 7; v[2].startSec = 0.6;
    v[3].offsetHz = 2500; v[3].wpm = 35; v[3].text = "5NN TU OK1XX <SK>"; v[3].snrDb = -6; v[3].startSec = 2;
    return v;
}

namespace {
class CwSynth : public ModeSynth {
public:
    CwSynth(const SynthConfig& s, double rate) : rate_(rate) {
        CwGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -cwTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        const bool clean = s.snrDb >= 99;   // the transmitter's clean signal (TxSession): no noise, the keyed tones at the level of a typed message (40 dB)
        c.noiseSigma = clean ? 0 : 0.05;
        c.signals = cwTestSignals();
        if (s.modeOpt[0] == 1) c.signals.resize(1);
        for (auto& g : c.signals) g.snrDb += clean ? 40 : s.snrDb;
        gen_ = std::make_unique<CwGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<CwGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeCwSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CwSynth>(cfg, sampleRate);
}

} // namespace dect2
