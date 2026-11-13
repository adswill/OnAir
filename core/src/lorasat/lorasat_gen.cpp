// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa satellite test signal (see lorasat_gen.h).
#include "dect2/lorasat/lorasat_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/lorasat/lorasat_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace dect2 {

namespace {
constexpr double kC = 299792458.0;
}

double LoraSatPass::dopplerHz(double t) const {
    const double a = closestKm * 1e3 / speedMS, d = t - tcaSec;
    return carrierMhz * 1e6 * (speedMS / kC) * (-d) / std::sqrt(d * d + a * a);
}
double LoraSatPass::rateHzS(double t) const {
    const double a = closestKm * 1e3 / speedMS, d = t - tcaSec, u = d * d + a * a;
    return -carrierMhz * 1e6 * (speedMS / kC) * a * a / (u * std::sqrt(u));
}
double LoraSatPass::stretchPpm(double t) const { return -1e6 * dopplerHz(t) / (carrierMhz * 1e6); }

std::vector<LoraSatGenFrame> loraSatBeacons(int sf, double bwHz, int sync, double periodSec, double passSec, int payloadBytes) {
    std::vector<LoraSatGenFrame> v;
    int n = 0;
    for (double t = 0.5; t + 3 < passSec; t += periodSec, n++) {
        LoraSatGenFrame f;
        f.p.sf = sf; f.p.bwHz = bwHz; f.p.cr = 8; f.p.crc = true; f.p.preamble = 8; f.p.syncWord = (uint8_t)sync;
        f.p.ldro = lora::autoLdro(sf, bwHz);
        char b[96];
        snprintf(b, sizeof b, "SAT-BCN #%04d bat=%.2fV t=%+d", n, 3.60 + 0.01 * (n % 20), 18 + (n % 7));
        f.payload.assign(b, b + std::min<size_t>(strlen(b), (size_t)payloadBytes));
        while ((int)f.payload.size() < payloadBytes) f.payload.push_back((uint8_t)('A' + f.payload.size() % 26));
        f.startSec = t; f.passSec = t;
        v.push_back(f);
    }
    return v;
}

struct LoraSatGenerator::Impl {
    LoraSatGenConfig c;
    double noiseRms = 0;
    genutil::NoiseSource noise;
    std::mt19937 rng;
    int64_t pos = 0;
    size_t next = 0;
    double cycle = 0;
    std::vector<lora::TxFrame> active;

    explicit Impl(const LoraSatGenConfig& cfg) : c(cfg), noise(cfg.seed * 7919u + 3u), rng(cfg.seed * 977u + 5u) {}

    void schedule(double until) {
        while (!c.frames.empty()) {
            if (next >= c.frames.size()) {
                if (!c.loop) return;
                next = 0; cycle += c.loopSec;
            }
            const LoraSatGenFrame& g = c.frames[next];
            const double start = cycle + c.leadSec + g.startSec;
            if (start > until) return;
            next++;
            lora::TxFrame f;
            f.p = g.p;
            f.data = lora::encode(g.p, g.payload.data(), g.payload.size());
            f.startSec = start;
            const double dop = c.dopplerOn ? c.pass.dopplerHz(g.passSec) : 0.0;
            f.freqRateHzS = c.dopplerOn && c.rateOn ? c.pass.rateHzS(g.passSec) : 0.0;
            f.sroPpm = c.sroPpm + (c.dopplerOn ? c.pass.stretchPpm(g.passSec) : 0.0);
            f.freqHz = c.dialOffsetHz + c.cfoHz + dop;
            f.amp = (float)(c.amp * std::pow(10.0, g.snrOffDb / 20));
            f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng);
            active.push_back(std::move(f));
        }
    }
};

LoraSatGenerator::LoraSatGenerator(const LoraSatGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    // noise: SNR in the bandwidth of the first frame (or 125 kHz)
    const double bw = c.frames.empty() ? 125e3 : c.frames[0].p.bwHz;
    if (c.snrDb < 99) p_->noiseRms = c.amp / std::sqrt(std::pow(10.0, c.snrDb / 10) * bw / c.rate);
}
LoraSatGenerator::~LoraSatGenerator() = default;

void LoraSatGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double t0 = (double)m.pos / m.c.rate, t1 = (double)(m.pos + (int64_t)n) / m.c.rate;
    m.schedule(t1 + 1);
    for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
    for (const auto& f : m.active) f.render(out, n, t0, m.c.rate);
    if (m.noiseRms > 0) m.noise.add(out, n, (float)(m.noiseRms / std::sqrt(2.0)));
    m.active.erase(std::remove_if(m.active.begin(), m.active.end(), [&](const lora::TxFrame& f) { return f.endSec() < t1; }), m.active.end());
    m.pos += (int64_t)n;
}

namespace {
class LoraSatSynth : public ModeSynth {
public:
    LoraSatSynth(const SynthConfig& s, double rate) : rate_(rate) {
        LoraSatGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -loraSatTuning().tuneOffsetHz;
        c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.sroPpm = s.sroPpm;
        if (s.modeOpt[0] == 1) {
            c.pass.carrierMhz = 868.1;
            c.frames = loraSatBeacons(10, 125e3, 0x34, 8, 600);
        } else {
            c.pass.carrierMhz = 436.8;
            c.frames = loraSatBeacons(11, 125e3, 0x12, 6, 600);
        }
        c.pass.tcaSec = 300;
        gen_ = std::make_unique<LoraSatGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<LoraSatGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeLoraSatSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<LoraSatSynth>(cfg, sampleRate);
}

} // namespace dect2
