// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the multi-channel TV tests: several DVB-T2 / DVB-T channels side by side in one wide capture. Each channel comes from the test
// generator (its own service id and name in the transport stream), is resampled from its native rate to the capture rate, shifted to its
// offset, and the channels are added with noise. Streamed, so a capture of seconds at 28 Msps needs no memory.
#pragma once
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2gen.h"
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace multitvtest {
using namespace dect2;

struct ChanSpec {
    bool dvbt = false;
    double offsetHz = 0;
    double bwMhz = 8;
    int serviceId = 1;
    std::string name = "Test";
    double levelDb = 0;      // relative to the other channels
    int plpId = 0;           // T2: PLP id (the generator's PLP 0 is renumbered in L1 by the id here)
    int cellId = 0x1001;
    unsigned seed = 1;
};

class ChanSource {
public:
    explicit ChanSource(const ChanSpec& s, double outRate) : s_(s), outRate_(outRate) {
        native_ = nativeRateHz(s.bwMhz);
        // the resampler goes up by at most 4: a wide capture (61.44 Msps from 9.14) takes two stages
        two_ = outRate > 3.9 * native_;
        if (two_) { rs_.configure(native_, native_ * 4); rs2_.configure(native_ * 4, outRate); }
        else rs_.configure(native_, outRate);
        if (s.dvbt) {
            dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi8; p.mod = dvbt::k64Qam; p.crHp = p.crLp = dvbt::kR23; p.cellId = s.cellId & 0xffff;
            g_.reset(new dvbt::Generator(p, dvbt::testTsSourceNamed(s.seed, s.serviceId, s.name), s.seed));
        } else {
            TxParams tp;
            tp.s1 = 0; tp.s2field1 = 1; tp.giIdx = 2; tp.payload = true; tp.plpMod = 2; tp.plpCod = 2; tp.plpRot = true; tp.plpTi = 3;
            tp.cellId = s.cellId; tp.seed = s.seed; tp.plpId = s.plpId;
            tp.tsSource = dvbt::testTsSourceNamed(s.seed, s.serviceId, s.name);
            t2_.reset(new T2Generator(tp));
        }
        gain_ = (float)std::pow(10.0, s.levelDb / 20.0);
    }
    // n samples at the capture rate, shifted to the channel's offset
    void pull(size_t n, std::vector<cf32>& out) {
        while (ready_.size() < n) {
            std::vector<cf32> f;
            if (s_.dvbt) g_->nextSymbol(f); else t2_->nextFrame(f);
            // unit RMS before the resampler
            double pw = 0;
            for (auto& v : f) pw += std::norm(v);
            const float k = (float)(1.0 / std::sqrt(std::max(pw / f.size(), 1e-12)));
            if (rms_ == 0) rms_ = k;
            for (auto& v : f) v *= rms_;
            if (two_) { mid_.clear(); rs_.process(f.data(), f.size(), mid_); rs2_.process(mid_.data(), mid_.size(), ready_); }
            else rs_.process(f.data(), f.size(), ready_);
        }
        out.assign(ready_.begin(), ready_.begin() + (long)n);
        ready_.erase(ready_.begin(), ready_.begin() + (long)n);
        // the shift is a phasor turned by a fixed step (a cos / sin per sample is too slow at 60 Msps); renormalised every block
        const double w = 2 * M_PI * s_.offsetHz / outRate_;
        if (!phInit_) { ph_ = std::polar(1.0, w * (double)pos_); phInit_ = true; }
        const std::complex<double> step = std::polar(1.0, w);
        std::complex<double> ph = ph_;
        for (size_t i = 0; i < n; i++) {
            out[i] *= gain_ * cf32((float)ph.real(), (float)ph.imag());
            ph *= step;
        }
        ph_ = ph / std::abs(ph);
        pos_ += n;
    }

private:
    ChanSpec s_;
    double outRate_, native_ = 0;
    ExactResampler rs_, rs2_;
    bool two_ = false;
    std::vector<cf32> mid_;
    std::unique_ptr<T2Generator> t2_;
    std::unique_ptr<dvbt::Generator> g_;
    std::vector<cf32> ready_;
    float gain_ = 1, rms_ = 0;
    uint64_t pos_ = 0;
    std::complex<double> ph_{1, 0};
    bool phInit_ = false;
};

// The sum of the channels plus white noise; snrDb: signal to noise inside one channel (7.6 MHz)
class Capture {
public:
    Capture(const std::vector<ChanSpec>& ch, double rate, double snrDb, unsigned seed = 5) : rate_(rate), rng_(seed) {
        for (auto& c : ch) src_.emplace_back(new ChanSource(c, rate));
        // channel power is 1 (unit RMS, level 0 dB) inside 7.6 MHz; noise spreads over the whole span
        sigma_ = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) * (rate / 7.6e6) / 2.0);
    }
    void pull(size_t n, std::vector<cf32>& out) {
        out.assign(n, cf32(0, 0));
        std::vector<cf32> t;
        for (auto& s : src_) { s->pull(n, t); for (size_t i = 0; i < n; i++) out[i] += t[i]; }
        dect2::normal_distribution<float> nd(0.f, sigma_);
        for (auto& v : out) { v += cf32{nd(rng_), nd(rng_)}; v *= 0.15f; }   // headroom: the sum of three OFDM signals peaks high
    }
    double rate() const { return rate_; }

private:
    double rate_;
    std::vector<std::unique_ptr<ChanSource>> src_;
    std::mt19937 rng_;
    float sigma_ = 0;
};

} // namespace multitvtest
