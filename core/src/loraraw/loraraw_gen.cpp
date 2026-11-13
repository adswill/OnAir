// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) test signal (see loraraw_gen.h).
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/loraraw/loraraw_rx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace dect2 {

namespace {
LoraRawGenFrame mk(int sf, double bw, int cr, bool crc, const char* text, int sync, double snrOff = 0, double cfo = 0, int ldro = -1) {
    LoraRawGenFrame f;
    f.p.sf = sf; f.p.bwHz = bw; f.p.cr = cr; f.p.crc = crc; f.p.syncWord = (uint8_t)sync; f.p.preamble = 8;
    f.p.ldro = ldro < 0 ? lora::autoLdro(sf, bw) : ldro == 1;
    f.payload.assign(text, text + std::strlen(text));
    f.snrOffDb = snrOff; f.cfoHz = cfo;
    return f;
}
} // namespace

std::vector<LoraRawGenFrame> loraRawTestSchedule(int sync) {
    std::vector<LoraRawGenFrame> v;
    v.push_back(mk(7, 125e3, 5, true, "OnAir LoRa raw SF7 125k", sync, 0, 1500));
    v.push_back(mk(8, 125e3, 6, true, "OnAir LoRa raw SF8 125k cr4/6", sync, 0, -2200));
    v.push_back(mk(9, 250e3, 5, true, "OnAir raw SF9 250k", sync, 3, 3100));
    v.push_back(mk(10, 500e3, 8, false, "OnAir raw SF10 500k cr4/8 no crc", sync, 6, -800));
    v.push_back(mk(7, 500e3, 7, true, "SF7 500k cr4/7", sync, 6, 400));
    v.push_back(mk(11, 125e3, 5, true, "{\"t\":21.5,\"h\":48}", sync, 0, 1200));
    v.push_back(mk(12, 125e3, 5, true, "Hello from SF12", sync, 0, -900));
    v.push_back(mk(9, 125e3, 5, true, "node=7 bat=3.71V", sync, 0, 2500));
    v.push_back(mk(8, 250e3, 6, true, "\x01\x02\x03 binary \xff\x10\x7f frame", sync, 3, -3000));
    return v;
}

std::vector<LoraRawGenFrame> loraRawImplicitSchedule(int sync) {
    std::vector<LoraRawGenFrame> v;
    const char* texts[] = {"implicit #001", "implicit #002", "implicit #003"};   // 13 bytes each ...
    for (const char* t : texts) {
        LoraRawGenFrame f = mk(8, 250e3, 6, true, t, sync, 3, 1800);
        f.payload.resize(12);                                                    // ... cut to the agreed 12
        f.p.implicitHdr = true; f.p.implicitLen = 12;
        v.push_back(f);
    }
    return v;
}

struct LoraRawGenerator::Impl {
    LoraRawGenConfig c;
    double noiseRms = 0;           // complex rms per sample
    genutil::NoiseSource noise;
    std::mt19937 rng;
    int64_t pos = 0;
    size_t next = 0;
    double nextStart = 0;
    std::vector<lora::TxFrame> active;
    double lastEnd = 0;

    explicit Impl(const LoraRawGenConfig& cfg) : c(cfg), noise(cfg.seed * 7919u + 3u), rng(cfg.seed * 977u + 5u) {
        nextStart = c.leadSec;
        if (c.snrDb < 99) noiseRms = c.amp / std::sqrt(std::pow(10.0, c.snrDb / 10) * 125e3 / c.rate);
    }

    void schedule(double until) {
        while (!c.frames.empty() && nextStart <= until) {
            if (next >= c.frames.size()) {
                if (!c.loop) return;
                next = 0;
            }
            const LoraRawGenFrame& g = c.frames[next++];
            lora::TxFrame f;
            f.p = g.p;
            f.data = lora::encode(g.p, g.payload.data(), g.payload.size());
            f.startSec = nextStart;
            f.sroPpm = c.sroPpm;
            f.amp = (float)(c.amp * std::pow(10.0, g.snrOffDb / 20));
            if (c.snrDb < 99 && g.p.bwHz != 125e3) f.amp = (float)(c.amp * std::pow(10.0, g.snrOffDb / 20));   // same noise: a wider band is a lower SNR
            f.freqHz = c.dialOffsetHz + c.cfoHz + g.cfoHz;
            f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng);
            nextStart = f.endSec() + g.gapSec;
            lastEnd = std::max(lastEnd, f.endSec());
            active.push_back(std::move(f));
        }
    }
};

LoraRawGenerator::LoraRawGenerator(const LoraRawGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
LoraRawGenerator::~LoraRawGenerator() = default;
double LoraRawGenerator::endSec() const { return p_->lastEnd; }

void LoraRawGenerator::generate(cf32* out, size_t n) {
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
class LoraRawSynth : public ModeSynth {
public:
    LoraRawSynth(const SynthConfig& s, double rate) : rate_(rate) {
        LoraRawGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -loraRawTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.sroPpm = s.sroPpm;
        const int sync = s.modeOpt[1] > 0 ? s.modeOpt[1] : 0x12;
        c.frames = s.modeOpt[0] == 1 ? loraRawImplicitSchedule(sync) : loraRawTestSchedule(sync);
        gen_ = std::make_unique<LoraRawGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<LoraRawGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeLoraRawSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<LoraRawSynth>(cfg, sampleRate);
}

} // namespace dect2
