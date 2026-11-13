// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise test signal, see ebicab_gen.h
#include "dect2/ebicab/ebicab_gen.h"
#include "dect2/ebicab/ebicab_proto.h"
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <random>

namespace dect2 {

std::vector<uint8_t> ebicabTestTelegram(bool lng, int nidBg, int nPig) {
    ebicab::Header h;
    h.qUpdown = 1; h.mVersion = 33; h.qMedia = 0; h.nPig = nPig; h.nTotal = 2; h.mDup = 0; h.mMcount = 255; h.nidC = 466; h.nidBg = nidBg; h.qLink = 1; h.firstPacket = 255;
    std::vector<uint8_t> u = ebicab::userBitsFromHeader(h, lng ? 830 : 210);
    // some payload bits after the end-of-information packet, varied by the balise number
    std::mt19937 g((uint32_t)(nidBg * 31 + nPig));
    for (size_t i = 58; i < u.size(); i++) u[i] = (uint8_t)(g() & 1);
    for (uint16_t b = 1; b < 4096; b++) {
        std::vector<uint8_t> t = ebicab::encode(u, lng, b);
        if (!t.empty()) return t;
    }
    return {};
}

struct EbicabGenerator::Impl {
    EbicabGenConfig c;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    double phase = 0, t = 0, sigma = 0;
    // schedule
    size_t telIdx = 0;
    bool inBurst = false;
    double leftSec = 0;           // time left in the current burst / gap
    std::vector<uint8_t> cur;     // telegram being repeated
    size_t bitPos = 0;
    double bitFrac = 0;
    double curFreq = 0;
    int curBit = 0;
    double jitter = 0;

    explicit Impl(const EbicabGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        if (c.ebn0Db < 99) {
            const double n0 = c.amp * c.amp / (ebicab::kBitRate * std::pow(10.0, c.ebn0Db / 10.0));
            sigma = std::sqrt(n0 * c.rate / 2);
        }
        leftSec = 0.002;   // starts with a gap
    }
    void nextPhase() {
        inBurst = !inBurst;
        if (inBurst) {
            leftSec = c.burstSec;
            if (c.randomBits || c.telegrams.empty()) { cur.assign(400, 0); for (auto& b : cur) b = (uint8_t)(rng() & 1); }
            else { cur = c.telegrams[telIdx]; telIdx = (telIdx + 1) % c.telegrams.size(); }
            bitPos = rng() % cur.size();   // the transmission need not start at the beginning of a telegram
            bitFrac = 0;
        } else {
            leftSec = c.gapSec;
        }
    }
    void generate(cf32* out, size_t n) {
        const double bitDur = 1.0 / (ebicab::kBitRate * (1 + c.clockPpm * 1e-6));
        for (size_t i = 0; i < n; i++) {
            const double dt = 1.0 / c.rate;
            leftSec -= dt;
            if (leftSec <= 0) nextPhase();
            double re = 0, im = 0;
            if (inBurst) {
                bitFrac += dt;
                if (bitFrac >= bitDur + jitter) {
                    bitFrac -= bitDur + jitter;
                    bitPos = (bitPos + 1) % cur.size();
                    jitter = c.jitterNs > 0 ? nd(rng) * c.jitterNs * 1e-9 : 0;
                }
                curBit = cur[bitPos] ^ (c.invert ? 1 : 0);
                curFreq = (curBit ? 1 : -1) * 0.5 * (ebicab::kFreqOneHz - ebicab::kFreqZeroHz);
                const double f = c.dialOffsetHz + c.cfoHz + c.driftHzPerSec * t + curFreq;
                phase += 2 * math::kPi * f / c.rate;
                if (phase > math::kPi) phase -= 2 * math::kPi; else if (phase < -math::kPi) phase += 2 * math::kPi;
                re = c.amp * std::cos(phase); im = c.amp * std::sin(phase);
            }
            if (sigma > 0) { re += sigma * nd(rng); im += sigma * nd(rng); }
            out[i] = cf32((float)re, (float)im);
            t += dt;
        }
    }
};

EbicabGenerator::EbicabGenerator(const EbicabGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
EbicabGenerator::~EbicabGenerator() = default;
void EbicabGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

namespace {
class EbicabSynth : public ModeSynth {
public:
    EbicabSynth(const SynthConfig& s, double rate) : rate_(rate) {
        EbicabGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -ebicabTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.ebn0Db = s.snrDb;
        const int k = s.modeOpt[0];
        c.randomBits = k == 3;
        if (k == 0 || k == 2) { c.telegrams.push_back(ebicabTestTelegram(true, 1201, 0)); c.telegrams.push_back(ebicabTestTelegram(true, 1201, 1)); }
        if (k == 1 || k == 2) { c.telegrams.push_back(ebicabTestTelegram(false, 1202, 0)); }
        gen_ = std::make_unique<EbicabGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<EbicabGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeEbicabSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<EbicabSynth>(cfg, sampleRate); }

} // namespace dect2
