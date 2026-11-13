// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A test signal for the engine's synthetic source made from a mode's own transmit generator (TxGenerator): the real transmitter's output, played as an
// endless loop. When the generator has finished (a message, a file, a stream) the loop sends a short gap of noise and builds a new generator, so a receiver
// that starts at any moment joins in. A carrier offset (the radios' oscillators) and a little noise are added; the noise is set from the signal's own
// measured power, so the SNR is the same whatever the generator's level is.
// Used by the KAPP, KAFT, KAIT, KAVB and KAWS synthetic signals (kapp_gen.h and so on).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/tx/tx.h"
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace dect2 {

class TxLoopSynth : public ModeSynth {
public:
    // makeGen(pass) returns the generator of pass number `pass` (0, 1, 2 ...) or nullptr when it cannot be made.
    using Factory = std::function<std::unique_ptr<TxGenerator>(int pass)>;

    TxLoopSynth(double rate, double carrierHz, double snrDb, double gapSec, Factory f, uint64_t seed)
        : rate_(rate), gapLeft_((uint64_t)(gapSec * rate)), gapSec_(gapSec), make_(std::move(f)), rng_(seed) {
        dph_ = 2 * 3.141592653589793 * carrierHz / rate;
        snrLin_ = std::pow(10.0, snrDb / 20.0);
        gapLeft_ = 0;
        next();
    }
    bool ok() const { return gen_ != nullptr; }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (gapLeft_ > 0) {
                const size_t k = (size_t)std::min<uint64_t>(gapLeft_, n - have);
                std::fill(out + have, out + have + k, cf32(0.f, 0.f));
                have += k; gapLeft_ -= k;
                if (gapLeft_ == 0) next();
                continue;
            }
            if (!gen_) { std::fill(out + have, out + n, cf32(0.f, 0.f)); break; }
            const size_t k = std::min<size_t>(n - have, 4096);
            gen_->generate(out + have, k);
            have += k;
            if (gen_->finished()) { gapLeft_ = (uint64_t)(gapSec_ * rate_); if (gapLeft_ == 0) next(); }
        }
        std::normal_distribution<float> g(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            const float p = std::norm(out[i]);
            if (p > 1e-8f) { pow_ += 0.0005 * ((double)p - pow_); }   // the power of the samples that carry signal
            ph_ += dph_;
            if (ph_ > 6.283185307179586) ph_ -= 6.283185307179586;
            const float sg = (float)(std::sqrt(pow_) / snrLin_ / std::sqrt(2.0));
            out[i] = out[i] * cf32((float)std::cos(ph_), (float)std::sin(ph_)) + cf32(sg * g(rng_), sg * g(rng_));
        }
    }
private:
    void next() {
        gen_ = make_(pass_++);
        if (gen_ && std::fabs(gen_->sampleRate() - rate_) > 1.0) gen_.reset();
    }
    double rate_;
    uint64_t gapLeft_;
    double gapSec_;
    Factory make_;
    std::unique_ptr<TxGenerator> gen_;
    int pass_ = 0;
    double dph_ = 0, ph_ = 0, snrLin_ = 31.6, pow_ = 0.05;
    std::mt19937_64 rng_;
};

} // namespace dect2
