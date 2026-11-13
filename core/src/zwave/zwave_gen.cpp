// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave test signal, see zwave_gen.h
#include "dect2/zwave/zwave_gen.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

using dect2::math::kPi;

std::vector<uint8_t> zwavePpduBits(const ZwGenFrame& f) {
    std::vector<uint8_t> bytes((size_t)(f.rate == 3 ? 24 : 10), 0x55);
    bytes.push_back(zwave::sofByte(f.rate));
    bytes.insert(bytes.end(), f.mpdu.begin(), f.mpdu.end());
    std::vector<uint8_t> bits;
    for (uint8_t b : bytes) for (int i = 7; i >= 0; i--) bits.push_back((b >> i) & 1);
    return bits;
}

struct ZwaveGenerator::Impl {
    ZwGenConfig c;
    genutil::NoiseSource noise;
    size_t idx = 0;
    std::vector<cf32> buf;
    size_t pos = 0;
    size_t gap = 0;
    double phase = 0;
    explicit Impl(const ZwGenConfig& cfg) : c(cfg), noise(cfg.seed) {}

    void render(const ZwGenFrame& f) {
        const int r = f.rate;
        std::vector<uint8_t> bits = zwavePpduBits(f), chips;
        if (r == 1) { for (uint8_t b : bits) { chips.push_back(b ? 1 : 0); chips.push_back(b ? 0 : 1); } }
        else chips = bits;
        chips.push_back(chips.back()); chips.push_back(chips.back());
        const double spc = c.rate / (zwave::chipRate(r) * (1 + c.clockPpm * 1e-6));
        const double dev = r == 3 ? 29e3 : 20e3;
        const size_t n = (size_t)(chips.size() * spc);
        buf.assign(n, cf32(0, 0));
        const double off = (f.mhz - c.centerMhz) * 1e6 + c.cfoHz;
        const float a = (float)(c.amp * f.ampScale);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)i / spc;
            double s;
            if (r == 3) {   // Gaussian FSK, BT 0.6
                const int k0 = (int)std::floor(t);
                s = 0;
                for (int k = k0 - 2; k <= k0 + 2; k++) if (k >= 0 && k < (int)chips.size()) s += (chips[(size_t)k] ? 1.0 : -1.0) * genutil::gfskPulse(t - k - 0.5, 0.6);
            } else s = chips[std::min((size_t)t, chips.size() - 1)] ? 1.0 : -1.0;
            phase += 2 * kPi * (off + dev * s) / c.rate;
            if (phase > kPi) phase -= 2 * kPi; else if (phase < -kPi) phase += 2 * kPi;
            buf[i] = cf32(a * (float)std::cos(phase), a * (float)std::sin(phase));
        }
        gap = (size_t)(f.gapSec * c.rate);
        pos = 0;
    }

    void generate(cf32* out, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (pos < buf.size()) {
                const size_t m = std::min(n - i, buf.size() - pos);
                std::copy(buf.begin() + (long)pos, buf.begin() + (long)(pos + m), out + i);
                pos += m; i += m;
            } else if (gap > 0) {
                const size_t m = std::min(n - i, gap);
                std::fill(out + i, out + i + m, cf32(0, 0));
                gap -= m; i += m;
            } else if (c.frames.empty() || (idx >= c.frames.size() && !c.loop)) {
                std::fill(out + i, out + n, cf32(0, 0));
                i = n;
            } else {
                if (idx >= c.frames.size()) idx = 0;
                render(c.frames[idx++]);
            }
        }
        if (c.snrDb < 99) {
            const double sig = c.amp * c.amp;
            const double sigma = std::sqrt(sig * c.rate / (1e5 * std::pow(10.0, c.snrDb / 10)) / 2);
            noise.add(out, n, (float)sigma);
        }
    }
};

ZwaveGenerator::ZwaveGenerator(const ZwGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
ZwaveGenerator::~ZwaveGenerator() = default;
void ZwaveGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

std::vector<ZwGenFrame> zwaveTestSchedule(const std::string& regionId) {
    const zwave::Region* reg = zwave::region(regionId);
    if (!reg) reg = &zwave::regions()[0];
    auto chan = [&](int rate) { for (const auto& ch : reg->chans) if (ch.rate == rate) return ch.mhz; return reg->chans[0].mhz; };
    uint32_t rng = 12345;
    auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); };
    const uint32_t home = 0xC0FFEE01u;
    std::vector<ZwGenFrame> v;
    auto add = [&](int rate, int src, int dst, std::vector<uint8_t> pl, int type = 1, bool ackReq = false, int seq = 0, std::vector<int> grp = {}) {
        zwave::Frame f;
        f.homeId = home; f.src = src; f.dst = dst; f.headerType = type; f.ackReq = ackReq; f.seq = seq; f.payload = std::move(pl); f.group = std::move(grp);
        ZwGenFrame g;
        g.rate = rate; g.mhz = chan(rate); g.mpdu = zwave::build(f, rate);
        v.push_back(g);
    };
    add(2, 1, 2, {0x25, 0x01, 0xFF}, 1, true, 1);
    add(2, 2, 1, {}, zwave::kHdrAck, false, 1);
    add(2, 1, 2, {0x25, 0x02}, 1, true, 2);
    add(2, 2, 1, {0x25, 0x03, 0xFF}, 1, false, 3);
    add(1, 1, 3, {0x26, 0x01, 0x32}, 1, true, 4);
    add(1, 3, 1, {}, zwave::kHdrAck, false, 4);
    add(2, 4, 1, {0x31, 0x05, 0x01, 0x22, 0x00, 0xDC}, 1, false, 5);
    add(1, 5, 1, {0x80, 0x03, 0x5A}, 1, false, 6);
    add(2, 5, 1, {0x84, 0x07}, 1, false, 7);
    add(2, 6, 255, {0x01, 0x01, 0xD3, 0x9C, 0x00, 0x04, 0x10, 0x01}, 1, false, 8);
    add(2, 1, 2, {0x00}, 1, true, 9);
    add(2, 1, 0, {0x25, 0x01, 0x00}, zwave::kHdrMulticast, false, 10, {2, 3, 5});
    {   std::vector<uint8_t> p = {0x98, 0x81}; for (int i = 0; i < 20; i++) p.push_back(rnd()); add(2, 1, 2, p, 1, true, 11); }
    {   std::vector<uint8_t> p = {0x9F, 0x03}; for (int i = 0; i < 16; i++) p.push_back(rnd()); add(2, 3, 1, p, 1, false, 12); }
    add(3, 1, 2, {0x20, 0x01, 0xFF}, 1, true, 13);
    add(3, 2, 1, {}, zwave::kHdrAck, false, 13);
    add(1, 2, 1, {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16}, 1, false, 14);
    for (auto& g : v) g.gapSec = 0.04;
    return v;
}

namespace {
struct ZwaveSynth : ModeSynth {
    double rate = 0;
    SynthConfig cfg;
    std::unique_ptr<ZwaveGenerator> gen;
    double sampleRate() const override { return rate; }
    bool build(const SynthConfig& sc) {
        cfg = sc;
        const auto& regs = zwave::regions();
        const int ri = sc.modeOpt[0] >= 0 && sc.modeOpt[0] < (int)regs.size() ? sc.modeOpt[0] : 0;
        ZwGenConfig g;
        g.rate = rate; g.centerMhz = regs[(size_t)ri].centreMhz; g.snrDb = sc.snrDb; g.cfoHz = sc.cfoHz;
        g.clockPpm = sc.sroPpm; g.frames = zwaveTestSchedule(regs[(size_t)ri].id);
        gen = std::make_unique<ZwaveGenerator>(g);
        return true;
    }
    bool configure(const SynthConfig& sc) override { return sc.modeOpt[0] == cfg.modeOpt[0] && sc.snrDb == cfg.snrDb && sc.cfoHz == cfg.cfoHz ? true : build(sc); }
    void generate(cf32* x, size_t n) override { gen->generate(x, n); }
};
}

std::unique_ptr<ModeSynth> makeZwaveSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < zwaveTuning().minSampleRate - 1) return nullptr;
    auto s = std::make_unique<ZwaveSynth>();
    s->rate = sampleRate;
    s->build(cfg);
    return s;
}

} // namespace dect2
