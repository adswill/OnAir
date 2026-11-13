// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL test signal for the engine's synthetic source (see kaal_gen.h).
#include "dect2/kaal/kaal_gen.h"
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/kaal/kaal_tx.h"
#include <cmath>
#include <random>
#include <vector>

namespace dect2 {
namespace {

class KaalSynth : public ModeSynth {
public:
    KaalSynth(const SynthConfig& s, double rate) : rate_(rate), rng_(0x4B41414CULL) {
        int kb = s.modeOpt[0] > 0 ? s.modeOpt[0] : 16;
        if (kb != 12 && kb != 16 && kb != 24 && kb != 32) kb = 16;
        tx_.configure(rate);
        tx_.setBitrate(kb);
        tx_.setSweepCallsign("ONAIR");
        kaal::BeaconFields f;
        f.callsign = "ONAIR"; f.grid = "KO26ab"; f.lat = 54.687; f.lon = 25.279; f.powerDbm = 20; f.frameType = 1;
        tx_.setBeacon(f, "OnAir test signal: KAAL voice and beacon.");
        tx_.setBeaconMix(true);
        dph_ = 2 * 3.141592653589793 * (1200.0 + s.cfoHz) / rate;
        noise_ = (float)(0.23 / std::pow(10.0, s.snrDb / 20.0) / std::sqrt(2.0));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            const size_t got = tx_.popIq(out + have, n - have);
            have += got;
            if (have >= n) break;
            int16_t pcm[320];   // 20 ms of a speech-like tone sequence: a wobbling pitch with a slow level change, a 6 s sentence repeated
            for (int i = 0; i < 320; i++, pulled_++) {
                const double t = std::fmod((double)pulled_ / 16000.0, 6.0);
                pcm[i] = (int16_t)(3000.0 * std::sin(2 * 3.141592653589793 * (150 + 60 * std::sin(2 * 3.141592653589793 * 1.3 * t)) * t) * (0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 3 * t)));
            }
            tx_.pushAudio(pcm, 320);
        }
        std::normal_distribution<float> g(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            ph_ += dph_;
            if (ph_ > 6.283185307179586) ph_ -= 6.283185307179586;
            out[i] = out[i] * cf32((float)std::cos(ph_), (float)std::sin(ph_)) + cf32(noise_ * g(rng_), noise_ * g(rng_));
        }
    }
private:
    double rate_, dph_ = 0, ph_ = 0;
    float noise_ = 0;
    uint64_t pulled_ = 0;
    kaal::VoiceTx tx_;
    std::mt19937_64 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeKaalSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 48000.0 - 1) return nullptr;
    return std::make_unique<KaalSynth>(cfg, sampleRate);
}

} // namespace dect2
