// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter test signal (see mscatter_gen.h).
#include "dect2/mscatter/mscatter_gen.h"
#include "dect2/mscatter/mscatter_dsp.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

struct MscatterGenerator::Impl {
    MscatterGenConfig c;
    genutil::NoiseSource noise;
    float sigma = 0;
    double noiseBin = 0;               // noise power in one bin of the detector
    double carrierPh = 0;
    int64_t n = 0;
    uint32_t rng;
    struct Echo { int64_t start, end; double amp, lp, tau, d0, drift, ph; int64_t lenS; };
    std::vector<Echo> active;
    std::vector<MsTruePing> all;
    double nextSec = 0, prevEndSec = 0, nextBurst = 0;
    struct Burst { int64_t a, b; };
    std::vector<Burst> bursts;

    explicit Impl(const MscatterGenConfig& cfg) : c(cfg), noise(cfg.seed), rng(cfg.seed * 2654435761u + 12345u) {
        if (c.snrDb < 99) {
            const double v = c.amp * c.amp * c.rate / (100.0 * std::pow(10.0, c.snrDb / 10));
            sigma = (float)std::sqrt(v / 2);
            noiseBin = v * ms::cicNoiseGain(ms::decimation(c.rate)) * 1.5 / ms::kFft;
        } else noiseBin = 1e-14;
        nextSec = c.firstSec;
        nextBurst = c.firstSec + 7.0;
        if (c.pingsOnlyAt) for (const auto& p : c.fixed) place(p);
    }
    double uni() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) / 16777216.0; }

    void place(const MsTruePing& p) {
        Echo e;
        const double pk = std::max(p.peakSnrDb, 11.0);
        const double decayDb = std::max(pk - 10.0, 1.0);
        const double lp = p.overdense ? 0.6 * p.durSec : 0.0;
        e.tau = (p.durSec - lp) / (decayDb * 0.2303);
        e.lp = lp;
        e.amp = std::sqrt(noiseBin * std::pow(10.0, pk / 10));
        e.d0 = p.dopplerHz; e.drift = 0; e.ph = 0;
        e.start = (int64_t)(p.startSec * c.rate);
        const double lenSec = lp + e.tau * 8.06 + 0.01;
        e.end = e.start + (int64_t)(lenSec * c.rate);
        e.lenS = e.end - e.start;
        active.push_back(e);
        all.push_back(p);
    }

    void schedule() {
        MsTruePing p;
        p.startSec = nextSec;
        p.overdense = uni() < 0.06;
        p.durSec = p.overdense ? 2.0 + 3.0 * uni() : 0.25 + 0.9 * uni();
        p.peakSnrDb = 16 + 18 * uni();
        p.dopplerHz = (uni() < 0.5 ? -1 : 1) * (30 + 220 * uni());
        place(p);
        active.back().drift = (uni() - 0.5) * 12;
        prevEndSec = (double)active.back().end / c.rate;
        nextSec = prevEndSec + 0.8 + (c.meanGapSec > 1 ? -c.meanGapSec * 0.7 * std::log(1.0 - uni() * 0.999) : 0.3);
    }
};

MscatterGenerator::MscatterGenerator(const MscatterGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
MscatterGenerator::~MscatterGenerator() = default;
const std::vector<MsTruePing>& MscatterGenerator::scheduled() const { return p_->all; }

void MscatterGenerator::generate(cf32* out, size_t cnt) {
    Impl& m = *p_;
    const double w0 = 2 * math::kPi * m.c.carrierHz / m.c.rate;
    if (!m.c.pingsOnlyAt && m.c.meanGapSec > 0)
        while (m.nextSec * m.c.rate < (double)(m.n + (int64_t)cnt) + m.c.rate) m.schedule();
    if (m.c.interference)
        while (m.nextBurst * m.c.rate < (double)(m.n + (int64_t)cnt) + m.c.rate) {
            m.bursts.push_back({(int64_t)(m.nextBurst * m.c.rate), (int64_t)((m.nextBurst + 0.04) * m.c.rate)});
            m.nextBurst += 20.0;
        }
    for (size_t k = 0; k < cnt; k++) {
        const int64_t i = m.n + (int64_t)k;
        m.carrierPh += w0;
        if (m.carrierPh > math::kPi) m.carrierPh -= 2 * math::kPi;
        const double ca = m.c.carrierOn ? m.c.amp : 0.0;
        double re = ca * std::cos(m.carrierPh), im = ca * std::sin(m.carrierPh);
        for (auto it = m.active.begin(); it != m.active.end();) {
            auto& e = *it;
            if (i >= e.end) { it = m.active.erase(it); continue; }
            if (i >= e.start) {
                const double t = (double)(i - e.start) / m.c.rate;
                double env;
                if (t < 0.01) env = t / 0.01;
                else if (t < e.lp) env = 1.0;
                else env = std::exp(-(t - e.lp) / (2 * e.tau));
                e.ph += 2 * math::kPi * (m.c.carrierHz + e.d0 + e.drift * t) / m.c.rate;
                if (e.ph > math::kPi) e.ph -= 2 * math::kPi; else if (e.ph < -math::kPi) e.ph += 2 * math::kPi;
                re += e.amp * env * std::cos(e.ph); im += e.amp * env * std::sin(e.ph);
            }
            ++it;
        }
        out[k] = cf32((float)re, (float)im);
    }
    if (m.sigma > 0) m.noise.add(out, cnt, m.sigma);
    for (auto it = m.bursts.begin(); it != m.bursts.end();) {
        if (it->b <= m.n) { it = m.bursts.erase(it); continue; }
        const int64_t a = std::max<int64_t>(it->a, m.n), b = std::min<int64_t>(it->b, m.n + (int64_t)cnt);
        if (b > a && m.sigma > 0) m.noise.add(out + (a - m.n), (size_t)(b - a), m.sigma * 18.f);
        ++it;
    }
    m.n += (int64_t)cnt;
}

namespace {
class MscatterSynth : public ModeSynth {
public:
    MscatterSynth(const SynthConfig& s, double rate) : rate_(rate) {
        MscatterGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.carrierHz = s.cfoHz != 0 ? s.cfoHz : 420.0;
        c.meanGapSec = s.modeOpt[0] == 1 ? 50.0 : (s.modeOpt[0] == 2 ? 0.0 : 3.0);
        c.interference = s.modeOpt[1] == 1;
        gen_ = std::make_unique<MscatterGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<MscatterGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeMscatterSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<MscatterSynth>(cfg, sampleRate);
}

} // namespace dect2
