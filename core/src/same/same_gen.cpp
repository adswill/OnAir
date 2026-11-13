// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME test signal: see same_gen.h.
#include "dect2/same/same_gen.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/same/same_rx.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kWork = 96000.0;
constexpr double kBaud = 520.0 + 5.0 / 6.0;
constexpr double kMark = 2083.0 + 1.0 / 3.0, kSpace = 1562.5;
constexpr double kDevAfsk = 2400.0, kDevTone = 3500.0, kChanBw = 25000.0;

struct Mod {
    std::vector<cf32>* out;
    double theta = 0, ph = 0;
    void silence(double sec) { out->insert(out->end(), (size_t)std::lround(sec * kWork), cf32(0.f, 0.f)); }
    void push(double devHz) {
        theta += 2 * kPi * devHz / kWork;
        if (theta > kPi) theta -= 2 * kPi;
        out->push_back(cf32((float)std::cos(theta), (float)std::sin(theta)));
    }
    // continuous-phase AFSK, bits at the exact baud rate
    void bits(const std::vector<uint8_t>& b) {
        double acc = 0;   // fractional samples carried between bits
        for (uint8_t v : b) {
            acc += kWork / kBaud;
            const int n = (int)acc;
            acc -= n;
            for (int i = 0; i < n; i++) {
                ph += 2 * kPi * (v ? kMark : kSpace) / kWork;
                if (ph > 2 * kPi) ph -= 2 * kPi;
                push(kDevAfsk * std::sin(ph));
            }
        }
    }
    void tone(double sec) {
        double p = 0;
        const size_t n = (size_t)std::lround(sec * kWork);
        for (size_t i = 0; i < n; i++) {
            p += 2 * kPi * 1050.0 / kWork;
            if (p > 2 * kPi) p -= 2 * kPi;
            push(kDevTone * std::sin(p));
        }
    }
};

std::vector<uint8_t> burstBits(const std::string& text, int flip, std::mt19937& rng) {
    std::string s(16, (char)0xAB);
    s += text;
    std::vector<uint8_t> b;
    for (uint8_t c : s) for (int i = 0; i < 8; i++) b.push_back((c >> i) & 1);   // least significant bit first
    for (int k = 0; k < flip; k++) {
        const size_t pos = 16 * 8 + rng() % (text.size() * 8);
        b[pos] ^= 1;
    }
    return b;
}

} // namespace

std::string sameTestTornado() { return "ZCZC-WXR-TOR-048201-348113-029047+0030-1301530-KHGX/NWS-"; }
std::string sameTestWeekly() { return "ZCZC-WXR-RWT-029047+0015-1301545-KEAX/NWS-"; }

struct SameGenerator::Impl {
    SameGenConfig cfg;
    std::vector<cf32> bb;
    double pos = 0;
    genutil::NoiseSource noise{7};
    float sigma = 0, scale = 1;
    cf32 step, rot = cf32(1.f, 0.f);

    explicit Impl(const SameGenConfig& c) : cfg(c), noise(c.seed) {
        std::mt19937 rng(c.seed);
        Mod m;
        m.out = &bb;
        if (!c.noiseOnly) {
            for (const SameGenMessage& g : c.messages) {
                m.silence(g.leadSec);
                for (int r = 0; r < g.repeats; r++) {
                    if (r == g.dropBurst) { m.silence(g.gapSec + 0.5); continue; }
                    m.bits(burstBits(g.text, r == g.corruptBurst ? g.corruptBits : 0, rng));
                    m.silence(g.gapSec);
                }
                if (g.toneSec > 0) { m.tone(g.toneSec); m.silence(0.5); }
                if (g.sendEom) for (int r = 0; r < 3; r++) { m.bits(burstBits("NNNN", 0, rng)); m.silence(g.gapSec); }
            }
            m.silence(1.0);
        } else bb.assign((size_t)(kWork * 4), cf32(0.f, 0.f));
        if (bb.empty()) bb.assign(1, cf32(0.f, 0.f));
        const double pn = c.rate / kChanBw / std::pow(10.0, c.snrDb / 10.0);
        sigma = c.snrDb < -100 || c.snrDb >= 150 ? 0.f : (float)std::sqrt(pn / 2);   // 200 dB: a transmitter, no noise and the clean level below (not full scale)
        scale = sigma > 0.15f ? 0.15f / sigma : 1.f;
        if (sigma == 0.f) scale = 0.4f;
        const double w = 2 * kPi * (c.cfoHz - c.tuneOffsetHz) / c.rate;
        step = cf32((float)std::cos(w), (float)std::sin(w));
    }
};

SameGenerator::SameGenerator(const SameGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
SameGenerator::~SameGenerator() = default;
double SameGenerator::cycleSec() const { return (double)p_->bb.size() / kWork; }

void SameGenerator::generate(cf32* out, size_t n) {
    Impl& s = *p_;
    const std::vector<cf32>& bb = s.bb;
    const double adv = kWork / s.cfg.rate;
    const size_t len = bb.size();
    for (size_t i = 0; i < n; i++) {
        const size_t i0 = (size_t)s.pos;
        const float fr = (float)(s.pos - (double)i0);
        const bool end = i0 + 1 >= len && !s.cfg.loop;
        const cf32 a = i0 < len ? bb[i0 % len] : cf32(0.f, 0.f), b = end ? cf32(0.f, 0.f) : bb[(i0 + 1) % len];
        out[i] = (a + (b - a) * fr) * s.rot * s.scale;
        s.rot *= s.step;
        s.pos += adv;
        if (s.pos >= (double)len) s.pos = s.cfg.loop ? s.pos - (double)len : (double)len;
    }
    s.rot /= std::abs(s.rot);
    if (s.sigma > 0.f) s.noise.add(out, n, s.sigma * s.scale);
}

namespace {

class SameSynth : public ModeSynth {
public:
    SameSynth(const SameGenConfig& c) : gen_(c), rate_(c.rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    SameGenerator gen_;
    double rate_;
};

} // namespace

std::unique_ptr<ModeSynth> makeSameSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < sameTuning().minSampleRate - 1) return nullptr;
    SameGenConfig c;
    c.rate = sampleRate;
    c.snrDb = cfg.snrDb;
    c.cfoHz = cfg.cfoHz;
    c.tuneOffsetHz = sameTuning().tuneOffsetHz;
    c.noiseOnly = cfg.modeOpt[0] == 1;
    SameGenMessage a;
    a.text = sameTestTornado();
    a.toneSec = 1.5;
    c.messages.push_back(a);
    if (cfg.modeOpt[0] == 0) {
        SameGenMessage b;
        b.text = sameTestWeekly();
        b.leadSec = 1.0;
        c.messages.push_back(b);
    }
    return std::make_unique<SameSynth>(c);
}

} // namespace dect2
