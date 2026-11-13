// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat: GFSK modulator, transmitter and test signal, see esubghz_gen.h
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/common/mathconst.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

using math::kPi;

namespace {

// The frequency pulse of one symbol after the Gaussian filter, sampled every 1/rate over +-2 symbols (area: one symbol of full deviation)
std::vector<float> gaussPulse(double rate, int& half) {
    const double sps = rate / esubghz::kBaud;
    half = (int)std::ceil(2 * sps);
    const double a = kPi * esubghz::kGaussBt * std::sqrt(2.0 / std::log(2.0));
    std::vector<float> g((size_t)(2 * half + 1));
    for (int i = -half; i <= half; i++) {
        const double t = i / sps;   // in symbols
        g[(size_t)(i + half)] = (float)(0.5 * (std::erf(a * (t + 0.5)) - std::erf(a * (t - 0.5))));
    }
    return g;
}

} // namespace

void esubghzModulate(const std::vector<uint8_t>& octets, double rate, double offsetHz, float amp, std::vector<cf32>& out, double& phase) {
    int half = 0;
    const std::vector<float> g = gaussPulse(rate, half);
    const double sps = rate / esubghz::kBaud;
    const size_t nb = octets.size() * 8;
    const size_t ns = (size_t)std::ceil((double)nb * sps);
    std::vector<float> f(ns, 0.f);   // frequency in units of the deviation
    for (size_t b = 0; b < nb; b++) {
        const float s = (octets[b / 8] >> (7 - b % 8)) & 1 ? 1.f : -1.f;
        const double c = ((double)b + 0.5) * sps;   // the symbol's centre in samples
        const long c0 = std::lround(c);
        for (int i = -half; i <= half; i++) {
            const long k = c0 + i;
            if (k < 0 || k >= (long)ns) continue;
            f[(size_t)k] += s * g[(size_t)(i + half)];
        }
    }
    // the carrier ramps up over the first symbol and down over one more symbol after the last (raised cosine): no key clicks
    const size_t ramp = std::max<size_t>(2, (size_t)std::lround(sps));
    const double w = 2 * kPi * esubghz::kDeviationHz / rate, wo = 2 * kPi * offsetHz / rate;
    out.reserve(out.size() + ns + ramp);
    for (size_t k = 0; k < ns + ramp; k++) {
        phase += w * f[std::min(k, ns - 1)] + wo;
        if (phase > kPi || phase < -kPi) phase = std::remainder(phase, 2 * kPi);
        float g = amp;
        if (k < ramp) g *= 0.5f - 0.5f * (float)std::cos(kPi * (double)k / (double)ramp);
        else if (k >= ns) g *= 0.5f + 0.5f * (float)std::cos(kPi * (double)(k - ns + 1) / (double)ramp);
        out.emplace_back(g * (float)std::cos(phase), g * (float)std::sin(phase));
    }
}

std::vector<cf32> esubghzMessageIq(const std::vector<uint8_t>& msg, double rate, double offsetHz, float amp, double leadSec, double tailSec) {
    std::vector<cf32> out((size_t)(leadSec * rate), cf32(0, 0));
    double phase = 0;
    const auto chunks = esubghz::splitMessage(msg);
    for (size_t i = 0; i < chunks.size(); i++) {
        if (i) out.insert(out.end(), (size_t)(esubghz::kChunkGapSec * rate), cf32(0, 0));
        esubghzModulate(esubghz::packetOctets(chunks[i].data(), chunks[i].size()), rate, offsetHz, amp, out, phase);
    }
    out.insert(out.end(), (size_t)(tailSec * rate), cf32(0, 0));
    return out;
}

std::vector<EsgLine> esubghzTestScript() {
    return {
        {"Flipper1 joined chat.", false, 0.6},
        {"Flipper1: hello from the test signal", false, 0.9},
        {"Flipper2: hi! plain text works", false, 0.9},
        {"Flipper1: this line is encrypted with AES-256-GCM", true, 0.9},
        {"Flipper2: and this one is long enough to need more than one packet on the air, so the receiver joins them", true, 1.2},
        {"Flipper2 left chat.", false, 1.5},
    };
}

// ---------------------------------------------------------------- transmitter

namespace {

class OneMessage : public TxGenerator {
public:
    OneMessage(std::vector<cf32> iq, double rate) : iq_(std::move(iq)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    bool finished() const override { return pos_ >= iq_.size(); }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = pos_ < iq_.size() ? iq_[pos_++] : cf32(0, 0);
    }
private:
    std::vector<cf32> iq_;
    double rate_;
    size_t pos_ = 0;
};

} // namespace

std::unique_ptr<TxGenerator> makeEsubghzTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < 4 * esubghz::kBaud) { err = "the Flipper chat transmitter needs a sample rate of at least 40 ksps"; return nullptr; }
    std::vector<uint8_t> msg;
    if (!r.payload.empty()) msg.assign(r.payload.begin(), r.payload.end());
    else msg = esubghz::encodePlain(r.text.empty() ? std::string("OnAir: hello") : r.text);
    if (msg.size() > esubghz::kMaxMessage) { err = "the message is longer than a Flipper takes in (1024 octets)"; return nullptr; }
    const double ifHz = r.o("esubghz.ifHz", 0);
    if (std::fabs(ifHz) + 2 * esubghz::kDeviationHz > 0.45 * rateHz) { err = "the transmit IF does not fit in the sample rate"; return nullptr; }
    return std::make_unique<OneMessage>(esubghzMessageIq(msg, rateHz, ifHz, 0.8f, 0.005, 0.01), rateHz);
}

// ---------------------------------------------------------------- test signal

namespace {

class EsgSynth : public ModeSynth {
public:
    EsgSynth(const SynthConfig& s, double rate)
        : rate_(rate), off_(-esubghzTuning().tuneOffsetHz + s.cfoHz), key_(esubghz::keyFromPassword(kEsubghzTestPassword)), f1_(0x1111111111111111ull),
          f2_(0x2222222222222222ull), rng_(7) {
        build();
        // carrier power amp^2; noise in 100 kHz at snrDb below it, spread over the whole rate
        sigma_ = s.snrDb >= 99 ? 0.f : (float)std::sqrt(kAmp * kAmp / std::pow(10.0, s.snrDb / 10) * rate / 100e3 / 2);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> nd(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            cf32 v = loop_[pos_];
            if (++pos_ >= loop_.size()) { pos_ = 0; build(); }
            if (sigma_ > 0) v += cf32(sigma_ * nd(rng_), sigma_ * nd(rng_));
            out[i] = v;
        }
    }
private:
    static constexpr float kAmp = 0.25f;
    // one pass of the conversation; every pass encrypts again with the next counters, as the Flippers would (no repeats for the replay guard)
    void build() {
        loop_.clear();
        for (const EsgLine& l : esubghzTestScript()) {
            esubghz::Sender& who = l.line.rfind("Flipper2", 0) == 0 ? f2_ : f1_;
            const std::vector<cf32> m = esubghzMessageIq(who.encode(l.encrypted ? &key_ : nullptr, l.line), rate_, off_, kAmp, 0.01, 0.0);
            loop_.insert(loop_.end(), m.begin(), m.end());
            loop_.insert(loop_.end(), (size_t)(l.gapSec * rate_), cf32(0, 0));
        }
    }
    double rate_, off_;
    esubghz::Key key_;
    esubghz::Sender f1_, f2_;
    std::vector<cf32> loop_;
    size_t pos_ = 0;
    float sigma_ = 0;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeEsubghzSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 4 * esubghz::kBaud) return nullptr;
    return std::make_unique<EsgSynth>(cfg, sampleRate);
}

} // namespace dect2
