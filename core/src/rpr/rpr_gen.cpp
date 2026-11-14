// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet test signal (see rpr_gen.h).
#include "dect2/rpr/rpr_gen.h"
#include "dect2/rpr/rpr_dsp.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
constexpr int kLead = 3;          // symbols of ramp before the first symbol and after the last
constexpr int kPre = 8;           // preamble: 8 symbols per carrier x 8 carriers = 64 DPSK symbols, 160 ms
constexpr double kH = 1.0 / 12.0;  // assumed: the 20 ms symbol = 1/60 s active part (carrier spacing 60 Hz, orthogonal) + 3.33 ms guard, cross-faded over the guard
double win(double d) {            // weight of a symbol at distance d (in symbols) from its centre; neighbours cross-fade and sum to 1
    const double u = std::abs(d);
    if (u <= 0.5 - kH) return 1.0;
    if (u >= 0.5 + kH) return 0.0;
    const double c = std::cos(kPi * (u - (0.5 - kH)) / (4 * kH));
    return c * c;
}
}

struct RprGenerator::Impl {
    RprGenConfig cfg;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    std::vector<double> carriers;
    int S = 0;                                       // symbols per carrier in a burst (the first is the phase reference)
    int order = 2;
    static constexpr int kBursts = 400;              // then the symbols wrap
    std::vector<std::vector<uint8_t>> steps;         // [carrier][burst * (S-1) + i]: step of symbol i+1
    std::vector<std::vector<double>> phase;          // [carrier][burst * S + m]
    size_t n = 0;
    double sigma = 0, T = 0.02, burstLen = 0, period = 0;
    double fph[4] = {0, 1.3, 2.9, 4.1}, ffr[4] = {0, 0, 0, 0};
    static constexpr double kCtl = 8000.0;
    static constexpr long kBlock = 2000, kMargin = 4;
    long blockStart = -1000000;
    std::vector<std::vector<cd>> amps;
    std::vector<cd> rot, stp;

    explicit Impl(const RprGenConfig& c) : cfg(c), rng(c.seed) {
        order = c.level == 1 ? 2 : 4;
        carriers = rpr::carriersFor(c.audioCentreHz);
        S = 1 + kPre + std::max(1, c.burstSyms);
        burstLen = (S + 2 * kLead) * T;
        period = burstLen + c.gapSec;
        steps.assign(8, std::vector<uint8_t>((size_t)kBursts * (size_t)(S - 1)));
        phase.assign(8, std::vector<double>((size_t)kBursts * (size_t)S));
        std::mt19937 pr(777);   // the preamble pattern of this generator (our own, the real one is not public)
        std::vector<std::vector<uint8_t>> pre(8, std::vector<uint8_t>(kPre));
        for (auto& v : pre) for (auto& s : v) s = (uint8_t)(pr() % (unsigned)order);
        const double st = 2 * kPi / (double)order;
        for (size_t k = 0; k < 8; k++)
            for (int b = 0; b < kBursts; b++) {
                double p = 0;
                phase[k][(size_t)b * (size_t)S] = 0;
                for (int i = 0; i < S - 1; i++) {
                    const uint8_t s = i < kPre ? pre[k][(size_t)i] : (uint8_t)(rng() % (unsigned)order);
                    steps[k][(size_t)b * (size_t)(S - 1) + (size_t)i] = s;
                    p += (double)s * st;
                    phase[k][(size_t)b * (size_t)S + (size_t)i + 1] = p;
                }
            }
        if (c.snrDb < 99) {
            const double snr = std::pow(10.0, c.snrDb / 10);
            sigma = std::sqrt(8.0 * c.amp * c.amp / snr * c.rate / 2500.0 / 2.0);
        }
        for (int i = 0; i < 4; i++) ffr[i] = c.fadeHz * (0.6 + 0.35 * i) * 0.5;
        amps.assign(8, std::vector<cd>((size_t)(kBlock + 2 * kMargin + 2)));
        rot.assign(8, cd(1, 0)); stp.resize(8);
        for (size_t k = 0; k < 8; k++) {
            const double f = (carriers[k] + c.cfoHz) * (1.0 + c.clockPpm * 1e-6) + c.dialOffsetHz;
            stp[k] = std::polar(1.0, 2 * kPi * f / c.rate);
        }
    }

    cd ampAt(size_t k, double t) const {
        const long b = (long)std::floor(t / period);
        const double tau = t - (double)b * period;
        if (tau >= burstLen) return cd(0, 0);
        const size_t bi = (size_t)b % (size_t)kBursts;
        const double x = tau / T - kLead;           // symbol m peaks at x = m
        cd a(0, 0);
        for (long m = (long)std::floor(x); m <= (long)std::floor(x) + 1; m++) {
            if (m < 0 || m >= S) continue;
            a += std::polar(1.0, phase[k][bi * (size_t)S + (size_t)m]) * win(x - (double)m);
        }
        return a;
    }
    cd fade(double t) const {
        if (cfg.fadeHz <= 0) return cd(1, 0);
        cd g(0, 0);
        for (int i = 0; i < 4; i++) g += std::polar(0.5, 2 * kPi * ffr[i] * t * (i % 2 ? 1 : -1) + fph[i]);
        return g;
    }
    void fillBlock(long bs) {
        blockStart = bs;
        for (size_t k = 0; k < 8; k++)
            for (long m = -kMargin; m <= kBlock + kMargin; m++) {
                const double t = (double)(bs + m) / kCtl;
                amps[k][(size_t)(m + kMargin)] = ampAt(k, t) * fade(t) * cfg.amp;
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
        for (size_t k = 0; k < 8; k++) {
            s += (amps[k][mi] * (1 - fr) + amps[k][mi + 1] * fr) * rot[k];
            rot[k] *= stp[k];
            if ((n & 1023) == 0) rot[k] /= std::abs(rot[k]);
        }
        if (sigma > 0) s += cd(sigma * nd(rng), sigma * nd(rng));
        return s;
    }
};

RprGenerator::RprGenerator(const RprGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
RprGenerator::~RprGenerator() = default;
const std::vector<std::vector<uint8_t>>& RprGenerator::sent() const { return p_->steps; }
double RprGenerator::burstSec() const { return p_->burstLen; }
double RprGenerator::periodSec() const { return p_->period; }
void RprGenerator::generate(cf32* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const cd s = p_->next();
        out[i] = p_->cfg.analytic ? cf32((float)s.real(), (float)s.imag()) : cf32((float)s.real(), 0.f);
    }
}

namespace {
class RprSynth : public ModeSynth {
public:
    RprSynth(const SynthConfig& s, double rate) : rate_(rate) {
        RprGenConfig c;
        c.rate = rate; c.analytic = true;
        c.dialOffsetHz = -20000;   // the radio sits 20 kHz above the dial (tuneOffsetHz)
        c.cfoHz = s.cfoHz; c.clockPpm = s.sroPpm; c.snrDb = s.snrDb;
        c.level = s.modeOpt[0] == 1 ? 1 : 2;
        gen_ = std::make_unique<RprGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<RprGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeRprSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<RprSynth>(cfg, sampleRate);
}

} // namespace dect2
