// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 test signal: see vdl4_gen.h.
#include "dect2/vdl4/vdl4_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/vdl4/vdl4_proto.h"
#include "dect2/vdl4/vdl4_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;


struct Ev { double t; int ch; std::vector<uint8_t> bits; double car; double amp; bool operator>(const Ev& o) const { return t > o.t; } };

class Vdl4Synth : public ModeSynth {
public:
    Vdl4Synth(const SynthConfig& cfg, double rate) : cfg_(cfg), rate_(rate), centerHz_(vdl4Tuning().defMhz * 1e6), rng_(cfg.modeOpt[1] ? cfg.modeOpt[1] : 1), noise_(cfg.modeOpt[1] ? cfg.modeOpt[1] : 1) {
        ch_ = vdl4EuropeChannels();
        if (cfg.snrDb < 90) {
            const double n = 0.08 * 0.08 / std::pow(10.0, cfg.snrDb / 10.0);
            sigma_ = (float)std::sqrt(n * rate / 25000.0 / 2.0);
        }
        schedule(0.2);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32(0, 0));
        // A burst can span several blocks (12 ms is 24000 samples at 2 Msps and a block may be shorter or end mid-burst): events that start in this
        // block join the active list, and every active burst is rendered into every block it overlaps (it used to be cut off at the end of its first block).
        const double endT = (double)(pos_ + (int64_t)n) / rate_;
        while (!q_.empty() && q_.top().t < endT) { active_.push_back(q_.top()); q_.pop(); }
        const double T = rate_ / kVdl4BitRate;
        for (size_t k = 0; k < active_.size();) {
            const Ev& e = active_[k];
            const double startS = e.t * rate_;   // absolute sample position
            const double f = ch_[(size_t)e.ch % ch_.size()];
            vdl4RenderBits(e.bits, out, pos_, n, rate_, startS, f - centerHz_ + e.car + cfg_.cfoHz, e.amp, 0);
            if (startS + ((double)e.bits.size() + 4) * T < (double)(pos_ + (int64_t)n)) active_.erase(active_.begin() + (long)k);
            else k++;
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
        pos_ += (int64_t)n;
        if (q_.empty() && active_.empty() && pos_ / rate_ > 3.0) { pos_ = 0; schedule(0.1); }
    }

private:
    void schedule(double t0) {
        const int n = std::max(1, cfg_.modeOpt[0] ? cfg_.modeOpt[0] : 4);
        for (int i = 0; i < n; i++) {
            Ev e;
            e.t = t0 + i * 0.4;
            e.ch = i % (int)ch_.size();
            e.car = dect2::uniform_real_distribution<double>(-150, 150)(rng_);
            e.amp = 0.08;
            const uint32_t id = 0x4ca2b1u + (uint32_t)i;
            e.bits = vdl4SynthBurstBits((i & 1) == 0, id, 1, 51.4 + 0.02 * i, -0.5 + 0.02 * i, 35000, "VDL4 TEST");
            q_.push(e);
        }
    }

    SynthConfig cfg_;
    double rate_, centerHz_;
    std::mt19937 rng_;
    genutil::NoiseSource noise_;
    float sigma_ = 0;
    std::vector<double> ch_;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> q_;
    std::vector<Ev> active_;
    int64_t pos_ = 0;
};
} // namespace

std::vector<uint8_t> vdl4SynthBurstBits(bool sync, uint32_t icao, int srcType, double lat, double lon, int altFt, const std::string& dlsText) {
    Vdl4BurstFields f;
    f.autonomous = true;
    f.src = (uint32_t)((srcType & 7) << 24) | (icao & 0xFFFFFF);
    f.burstId = 1;
    if (sync) {
        f.msgId = 1;
        f.info = vdl4SyncBurstInfo(lat, lon, altFt, false, f.src, srcType);
    } else {
        f.msgId = 3;
        f.info = vdl4DlsUserInfo(dlsText);
    }
    const auto frame = vdl4PackBurst(f);
    return vdl4BurstLineBits(vdl4HdlcBits({frame}));
}

void vdl4RenderBits(const std::vector<uint8_t>& lineBits, cf32* out, int64_t outStart, size_t n, double rate, double start, double carHz, double amp, double phase) {
    const double T = rate / kVdl4BitRate;
    const double s0 = start - 2 * T, s1 = start + ((double)lineBits.size() + 2) * T;
    const int64_t a = std::max<int64_t>(outStart, (int64_t)std::ceil(s0));
    const int64_t b = std::min<int64_t>(outStart + (int64_t)n, (int64_t)std::floor(s1));
    if (a >= b) return;
    // The phase is the INTEGRAL of the Gaussian-filtered frequency (modulation index 0.5, deviation 4.8 kHz). It was once frequency x elapsed time,
    // which is not FM: the signal chirped away from the channel after the first few bits and nothing could decode it.
    const double sg = std::sqrt(std::log(2.0)) / (2 * kPi * 0.28), kk = 1 / (std::sqrt(2.0) * sg);
    auto ierf = [](double x) { return x * std::erf(x) + std::exp(-x * x) / std::sqrt(kPi); };
    auto gInt = [&](double tau) { return 0.5 / kk * (ierf((tau + 0.5) * kk) - ierf((tau - 0.5) * kk)) + 0.5; };   // integral of gfskPulse up to tau, 0 -> 1
    std::vector<double> pre(lineBits.size() + 1, 0.0);   // sum of the +-1 symbols before k: bits that are long past contribute their whole area
    for (size_t k = 0; k < lineBits.size(); k++) pre[k + 1] = pre[k] + (lineBits[k] ? 1.0 : -1.0);
    const double dev = 4800.0;
    for (int64_t i = a; i < b; i++) {
        const double u = ((double)i - start) / T;
        const int k0 = (int)std::floor(u);
        const int lo = std::max(0, k0 - 3), hi = std::min((int)lineBits.size() - 1, k0 + 3);
        double sum = pre[(size_t)std::min<int>(std::max(lo, 0), (int)lineBits.size())];
        for (int k = lo; k <= hi; k++) sum += (lineBits[(size_t)k] ? 1.0 : -1.0) * gInt(u - k - 0.5);
        const double ph = 2 * kPi * dev / kVdl4BitRate * sum + 2 * kPi * carHz * ((double)i - start) / rate + phase;
        const size_t o = (size_t)(i - outStart);
        out[o] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
    }
}

std::unique_ptr<ModeSynth> makeVdl4Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<Vdl4Synth>(cfg, sampleRate);
}

} // namespace dect2
