// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES test signal: see vdes_gen.h.
#include "dect2/vdes/vdes_gen.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/vdes/vdes_tel.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kAmp = 0.3;
constexpr int kTab = 64, kSpan = 8;     // RRC table: points per symbol, symbols each side

double rrc(double t, double b) {
    if (std::fabs(t) < 1e-9) return 1.0 - b + 4.0 * b / kPi;
    if (std::fabs(std::fabs(t) - 1.0 / (4.0 * b)) < 1e-9)
        return b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
    const double d = kPi * t * (1.0 - 16.0 * b * b * t * t);
    return (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / d;
}

const std::vector<float>& rrcTable() {
    static const std::vector<float> t = [] {
        std::vector<float> v((size_t)(2 * kSpan * kTab + 2));
        for (size_t i = 0; i < v.size(); i++) v[i] = (float)rrc(((double)i - kSpan * kTab) / kTab, kVdeRolloff);
        return v;
    }();
    return t;
}

std::vector<cf32> vdeSymbols(int mod, size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<cf32> a(n);
    double ph = 0;
    for (size_t k = 0; k < n; k++) {
        if (mod == 1) {                                   // pi/4-QPSK: phase steps of +-pi/4 and +-3pi/4
            ph += (2 * (int)(rng() & 3) + 1) * kPi / 4;
            a[k] = cf32((float)std::cos(ph), (float)std::sin(ph));
        } else if (mod == 2) {                            // 8PSK
            const double p = (rng() & 7) * kPi / 4;
            a[k] = cf32((float)std::cos(p), (float)std::sin(p));
        } else {                                          // 16QAM
            const float s = 1.f / std::sqrt(10.f);
            a[k] = cf32(((int)(rng() & 3) * 2 - 3) * s, ((int)(rng() & 3) * 2 - 3) * s);
        }
    }
    return a;
}

void addVde(std::vector<cf32>& out, const VdeBurstSpec& s, const VdesRenderConfig& c) {
    const double R = kVdeSymRate25 * s.nSub * (1.0 + c.sroPpm * 1e-6);
    const size_t nSym = std::max<size_t>(8, (size_t)std::lround(s.durSec * R));
    const std::vector<cf32> a = vdeSymbols(s.mod, nSym, s.seed);
    const std::vector<float>& tab = rrcTable();
    const double amp = kAmp * s.level * std::sqrt((double)s.nSub);
    const double subc = (s.firstSub + (s.nSub - 1) / 2.0 - 1.5) * 25000.0;
    const double f = kVdeBandHz[s.band ? 1 : 0] - c.dialHz + subc + s.cfoHz + c.cfoHz;
    const double w = 2 * kPi * f / c.rate;
    const size_t i0 = (size_t)std::max(0.0, std::floor((s.startSec - kSpan / R) * c.rate));
    const size_t i1 = std::min(out.size(), (size_t)std::ceil((s.startSec + ((double)nSym + kSpan) / R) * c.rate));
    for (size_t i = i0; i < i1; i++) {
        const double u = ((double)i / c.rate - s.startSec) * R;          // in symbols
        const long k0 = (long)std::floor(u);
        cf32 v(0, 0);
        for (long k = k0 - kSpan + 1; k <= k0 + kSpan; k++) {
            if (k < 0 || k >= (long)nSym) continue;
            const double pos = (u - (double)k + kSpan) * kTab;
            const size_t ip = (size_t)pos;
            const float fr = (float)(pos - (double)ip);
            if (ip + 1 >= tab.size()) continue;
            v += a[(size_t)k] * (tab[ip] + (tab[ip + 1] - tab[ip]) * fr);
        }
        const double ph = w * (double)i;
        out[i] += v * (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph));
    }
}

class VdesSynth : public ModeSynth {
public:
    VdesSynth(const SynthConfig& cfg, double rate) : rate_(rate) {
        std::mt19937 rng(5);
        std::vector<AisBurstSpec> asmB;
        std::vector<VdeBurstSpec> vde;
        auto asmAt = [&](char ch, double t) {
            AisBurstSpec b;
            b.channel = ch; b.startSec = t;
            b.payload.assign(96 + rng() % 120, 0);
            for (auto& v : b.payload) v = (uint8_t)(rng() & 1);
            b.payload[0] = b.payload[1] = b.payload[2] = 0;   // not an AIS message type
            asmB.push_back(b);
        };
        auto vdeAt = [&](int first, int n, int mod, double t, double dur) {
            VdeBurstSpec b;
            b.band = 1; b.firstSub = first; b.nSub = n; b.mod = mod; b.startSec = t; b.durSec = dur; b.seed = 100 + (uint32_t)vde.size();
            vde.push_back(b);
        };
        const int sel = cfg.modeOpt[0];
        if (sel != 2) { asmAt('A', 0.10); asmAt('B', 0.60); asmAt('A', 1.20); asmAt('B', 1.70); asmAt('A', 2.30); asmAt('B', 2.70); }
        if (sel != 1) {
            vdeAt(0, 1, 1, 0.30, 0.025); vdeAt(3, 1, 2, 0.75, 0.025); vdeAt(1, 2, 2, 0.90, 0.030); vdeAt(0, 4, 3, 1.40, 0.030);
            vdeAt(2, 1, 3, 1.90, 0.025); vdeAt(2, 2, 1, 2.10, 0.030); vdeAt(0, 4, 1, 2.50, 0.030);
        }
        VdesRenderConfig rc;
        rc.rate = rate; rc.durationSec = kCycle; rc.snrDb = cfg.snrDb; rc.cfoHz = cfg.cfoHz; rc.sroPpm = cfg.sroPpm;
        cyc_ = renderVdes(asmB, vde, rc);
        if (cfg.snrDb >= 150) {
            // a transmitter (cleanSynth: 200 dB): the four-channel 16-QAM bursts (0.6 rms through the root-raised cosine) peaked at 1.23 and
            // were clipped by the output limiter. The whole cycle is scaled down so that its peak is 0.8, the AIS / VDE levels kept relative.
            float pk = 0.f;
            for (const auto& v : cyc_) pk = std::max(pk, std::abs(v));
            if (pk > 0.8f) for (auto& v : cyc_) v *= 0.8f / pk;
        }
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            out[i] = cyc_[pos_];
            if (++pos_ == cyc_.size()) pos_ = 0;
        }
    }
private:
    static constexpr double kCycle = 3.0;
    double rate_;
    std::vector<cf32> cyc_;
    size_t pos_ = 0;
};

} // namespace

std::vector<cf32> renderVdes(const std::vector<AisBurstSpec>& asmBursts, const std::vector<VdeBurstSpec>& vdeBursts, const VdesRenderConfig& c) {
    std::vector<cf32> out;
    if (asmBursts.empty()) out.assign((size_t)(c.durationSec * c.rate), cf32());
    else {
        AisRenderConfig ac;
        ac.rate = c.rate; ac.durationSec = c.durationSec; ac.snrDb = -200; ac.cfoHz = c.cfoHz; ac.sroPpm = c.sroPpm;
        ac.tuneOffsetHz = -((kVdesAsm1Hz - c.dialHz) + 25000.0);   // ASM 1 / ASM 2 sit at -25 / +25 kHz of the tuned frequency, minus this
        out = renderAisBursts(asmBursts, ac);
    }
    for (const auto& b : vdeBursts) addVde(out, b, c);
    if (c.snrDb > -100) {
        const double pn = kAmp * kAmp / std::pow(10.0, c.snrDb / 10.0) * (c.rate / 25000.0);
        float sigma = (float)std::sqrt(pn / 2);
        float scale = 1.f;
        if (sigma > 0.15f) scale = 0.15f / sigma;
        genutil::NoiseSource ns(c.seed);
        ns.add(out.data(), out.size(), sigma);
        if (scale != 1.f) for (auto& v : out) v *= scale;
    }
    return out;
}

std::unique_ptr<ModeSynth> makeVdesSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<VdesSynth>(cfg, sampleRate); }

} // namespace dect2
