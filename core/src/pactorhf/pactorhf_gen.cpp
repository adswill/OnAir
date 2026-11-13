// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PACTOR-II / -III / -IV test signals (see pactorhf_gen.h).
#include "dect2/pactorhf/pactorhf_gen.h"
#include "dect2/pactorhf/pactorhf_dsp.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
}

std::vector<int> pactor3Carriers(int level) {
    // The SCS table marks which of the 18 carriers (0 .. 17) a level uses; the text of the description gives the counts: 2, 6, 14, 14, 16, 18.
    // Level 1 sits in the middle, the others are centred around it (the crossed carriers are not recoverable from the text of the table, so the
    // sets here are the centred ones; the receiver does not depend on which of them it is, only on the 120 Hz grid).
    int count = 2;
    switch (level) { case 1: count = 2; break; case 2: count = 6; break; case 3: case 4: count = 14; break; case 5: count = 16; break; default: count = 18; break; }
    std::vector<int> v;
    const int first = (18 - count) / 2;
    for (int i = 0; i < count; i++) v.push_back(first + i);
    return v;
}

struct PactorHfGenerator::Impl {
    PactorHfGenConfig cfg;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    std::vector<double> carriers;
    std::vector<std::vector<uint8_t>> symbols;     // PACTOR-II / III: phase step indices
    std::vector<std::vector<double>> cum;          // their cumulative phase
    std::vector<uint8_t> sync;                     // the fixed pattern
    std::vector<cd> iv;                            // PACTOR-IV symbols
    std::vector<double> rrc;
    size_t n = 0;
    double sigma = 0;
    double fph[4] = {0, 1.3, 2.9, 4.1}, ffr[4] = {0, 0, 0, 0};
    // The slowly changing amplitude of every carrier is computed at kCtl samples per second and interpolated per output sample
    static constexpr double kCtl = 8000.0;
    static constexpr long kBlock = 2000, kMargin = 16;
    long blockStart = -1000000;
    std::vector<std::vector<cd>> amps;             // [carrier][m - blockStart + kMargin]
    std::vector<cd> rot, step, delayRot;

    explicit Impl(const PactorHfGenConfig& c) : cfg(c), rng(c.seed) {
        if (c.variant == 2) carriers = {c.audioCentreHz - 100, c.audioCentreHz + 100};
        else if (c.variant == 3) for (int i : pactor3Carriers(c.level)) carriers.push_back(480.0 + 120.0 * i);
        else carriers = {1500.0};
        sync.resize((size_t)c.syncSyms);
        std::mt19937 sr(12345);
        const size_t order = (size_t)std::max(2, c.order);
        for (auto& s : sync) s = (uint8_t)(sr() % (unsigned)order);
        if (c.variant != 4) {
            const size_t total = 100 * 400;   // 400 s of symbols, then they wrap
            symbols.assign(carriers.size(), std::vector<uint8_t>(total));
            cum.assign(carriers.size(), std::vector<double>(total + 2));
            const double stp = 2 * kPi / (double)order;
            for (size_t k = 0; k < carriers.size(); k++) {
                double p = 0;
                for (size_t i = 0; i < total; i++) {
                    const size_t pos = i % (size_t)c.frameSyms;
                    symbols[k][i] = pos < (size_t)c.syncSyms ? (uint8_t)((sync[pos] + k) % order) : (uint8_t)(rng() % order);
                    p += (double)symbols[k][i] * stp;
                    cum[k][i] = p;
                }
                cum[k][total] = p; cum[k][total + 1] = p;
            }
        } else {
            const size_t total = 1800 * 120;
            iv.resize(total);
            const double a4[4] = {-3, -1, 1, 3};
            for (size_t i = 0; i < total; i++) {
                const size_t pos = i % (size_t)c.frameSyms;
                const unsigned r = pos < (size_t)c.syncSyms ? sync[pos] : (unsigned)(rng() % order);
                if (c.order <= 8) iv[i] = std::polar(1.0, 2 * kPi * (double)r / (double)order + (c.order == 2 ? 0 : kPi / (double)order));
                else iv[i] = cd(a4[(r >> 2) & 3] / std::sqrt(10.0), a4[r & 3] / std::sqrt(10.0));
            }
            const int span = 8; const double alpha = 0.35;
            rrc.assign((size_t)(2 * span * 16 + 1), 0);
            for (size_t i = 0; i < rrc.size(); i++) {
                const double t = ((double)i - span * 16.0) / 16.0;
                double v;
                if (std::abs(t) < 1e-9) v = 1 - alpha + 4 * alpha / kPi;
                else if (std::abs(std::abs(4 * alpha * t) - 1) < 1e-6) v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
                else v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - std::pow(4 * alpha * t, 2)));
                rrc[i] = v;
            }
        }
        if (c.snrDb < 99) {
            const double pSig = c.variant == 4 ? c.amp * c.amp * 0.2 : (double)carriers.size() * c.amp * c.amp * 0.75;
            const double snr = std::pow(10.0, c.snrDb / 10);
            sigma = std::sqrt(pSig / snr * c.rate / 2500.0 / 2.0);   // per component (I and Q)
        }
        for (int i = 0; i < 4; i++) ffr[i] = c.fadeHz * (0.6 + 0.35 * i) * 0.5;
        const size_t K = carriers.size();
        amps.assign(K, std::vector<cd>((size_t)(kBlock + 2 * kMargin + 2)));
        rot.assign(K, cd(1, 0)); step.resize(K); delayRot.resize(K);
        for (size_t k = 0; k < K; k++) {
            const double f = (carriers[k] + c.cfoHz) * (1.0 + c.clockPpm * 1e-6) + c.dialOffsetHz;
            step[k] = std::polar(1.0, 2 * kPi * f / c.rate);
            delayRot[k] = std::polar(1.0, -2 * kPi * (carriers[k] + c.cfoHz) * 0.0015);
        }
    }

    // the baseband amplitude of PACTOR-II / III carrier k at time t: pulses, phase accumulated from the symbol steps
    cd carrierAt(size_t k, double t) {
        const double x = t * 100.0;
        const long i = (long)std::floor(x);
        const double f = x - (double)i;   // 0 .. 1 inside symbol i (its centre at 0.5, its boundaries at 0 and 1)
        // PACTOR-II: raised-cosine pulses (cross-fade over the whole symbol). PACTOR-III: flat top with a cross-fade of +-h around the boundaries
        // (the carriers are only 120 Hz apart; the receiver nulls the neighbours with a rectangular window of 1 / 120 s)
        const double h = cfg.variant == 3 ? 0.18 : 0.5;
        auto fade = [&](double d) { return d <= -h ? 0.0 : d >= h ? 1.0 : std::pow(std::sin(kPi * (d + h) / (4 * h)), 2); };
        const cd c0 = std::polar(1.0, cp(k, i));
        if (f < h) { const double w = fade(f); return std::polar(1.0, cp(k, i - 1)) * (1 - w) + c0 * w; }
        if (f > 1 - h) { const double w = fade(f - 1); return c0 * (1 - w) + std::polar(1.0, cp(k, i + 1)) * w; }
        return c0;
    }
    double cp(size_t k, long j) const {
        const size_t N = cum[k].size() - 2;
        if (j < 0) j = 0;
        return cum[k][(size_t)j % N];
    }
    cd pulseIV(double tSym) const {
        const int span = 8;
        const long i0 = (long)std::floor(tSym);
        cd acc(0, 0);
        for (long j = i0 - span + 1; j <= i0 + span; j++) {
            const double x = ((tSym - (double)j) + span) * 16.0;
            if (x < 0 || x >= (double)rrc.size() - 1) continue;
            const size_t xi = (size_t)x; const double fr = x - (double)xi;
            const double h = rrc[xi] * (1 - fr) + rrc[xi + 1] * fr;
            acc += iv[(size_t)(((j % (long)iv.size()) + (long)iv.size()) % (long)iv.size())] * h;
        }
        return acc;
    }
    cd fade(double t) const {
        if (cfg.fadeHz <= 0) return cd(1, 0);
        cd g(0, 0);
        for (int i = 0; i < 4; i++) g += std::polar(0.5, 2 * kPi * ffr[i] * t * (i % 2 ? 1 : -1) + fph[i]);
        return g;
    }
    void fillBlock(long bs) {
        blockStart = bs;
        for (size_t k = 0; k < carriers.size(); k++)
            for (long m = -kMargin; m <= kBlock + kMargin; m++) {
                const double t = (double)(bs + m) / kCtl;
                cd b = cfg.variant == 4 ? pulseIV(t * 1800.0) * 0.45 : carrierAt(k, t);
                amps[k][(size_t)(m + kMargin)] = b * fade(t) * cfg.amp;
            }
    }

    cd next() {
        const double t = (double)n / cfg.rate * (1.0 + cfg.clockPpm * 1e-6);
        n++;
        const double x = t * kCtl;
        long m = (long)std::floor(x);
        const double fr = x - (double)m;
        if (m < blockStart || m >= blockStart + kBlock) fillBlock((m / kBlock) * kBlock);
        const size_t mi = (size_t)(m - blockStart + kMargin);
        cd s(0, 0);
        const long dly = (long)std::lround(0.0015 * kCtl);
        for (size_t k = 0; k < carriers.size(); k++) {
            const cd a = amps[k][mi] * (1 - fr) + amps[k][mi + 1] * fr;
            cd v = a;
            if (cfg.twoPath) { const cd d = amps[k][(size_t)((long)mi - dly)] * (1 - fr) + amps[k][(size_t)((long)mi - dly + 1)] * fr; v += 0.5 * d * delayRot[k]; }
            s += v * rot[k];
            rot[k] *= step[k];
            if ((n & 1023) == 0) rot[k] /= std::abs(rot[k]);
        }
        if (sigma > 0) s += cd(sigma * nd(rng), sigma * nd(rng));
        return s;
    }
};

PactorHfGenerator::PactorHfGenerator(const PactorHfGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
PactorHfGenerator::~PactorHfGenerator() = default;
const std::vector<std::vector<uint8_t>>& PactorHfGenerator::sent() const { return p_->symbols; }
const std::vector<double>& PactorHfGenerator::carrierHz() const { return p_->carriers; }
void PactorHfGenerator::generate(cf32* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const cd s = p_->next();
        out[i] = p_->cfg.analytic ? cf32((float)s.real(), (float)s.imag()) : cf32((float)s.real(), 0.f);
    }
}

namespace {
class HfSynth : public ModeSynth {
public:
    HfSynth(int variant, const SynthConfig& s, double rate) : rate_(rate) {
        PactorHfGenConfig c;
        c.variant = variant; c.rate = rate; c.analytic = true;
        c.dialOffsetHz = -20000;   // the radio sits 20 kHz above the dial (tuneOffsetHz)
        c.cfoHz = s.cfoHz; c.clockPpm = s.sroPpm; c.snrDb = s.snrDb;
        if (variant == 3) c.level = s.modeOpt[0] >= 1 && s.modeOpt[0] <= 6 ? s.modeOpt[0] : 3;
        if (variant == 4) c.order = s.modeOpt[0] == 2 || s.modeOpt[0] == 8 || s.modeOpt[0] == 16 ? s.modeOpt[0] : 4;
        c.twoPath = s.modeOpt[1] == 1;
        c.amp = variant == 4 ? 0.3 : 0.25 / std::sqrt((double)(variant == 3 ? pactor3Carriers(c.level).size() : 2));
        gen_ = std::make_unique<PactorHfGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<PactorHfGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makePactorHfSynth(int variant, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HfSynth>(variant, cfg, sampleRate);
}

} // namespace dect2
