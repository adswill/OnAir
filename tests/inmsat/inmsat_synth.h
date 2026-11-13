// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the Inmarsat all-carriers mode: carriers of chosen width at chosen offsets (two-pole filtered noise, so any
// width), with Doppler drift, slow fading and an optional DC spike, in white noise. Only for tests.
#pragma once
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdint>
#include <vector>

using dect2::cf32;
namespace inmsat_test {

struct CarrierSpec {
    double offsetHz = 0;
    double widthHz = 1000;       // 3 dB width of the two-pole noise shaping (the occupied width is a few times this at high SNR)
    double snrDb = 20;           // carrier power over noise power in the carrier's own bandwidth ... see amp below
    double dopplerHzPerSec = 0;
    double fadeDepthDb = 0;      // slow fading, peak to peak
    double fadeHz = 0.3;
};

class Rng {
public:
    explicit Rng(uint64_t s) : s_(s ? s : 1) {}
    float uni() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return (float)((s_ >> 11) & 0xFFFFFF) / 16777216.f - 0.5f; }
    float gauss() { return (uni() + uni() + uni() + uni()) * 1.7320508f; }   // sum of 4 uniforms of variance 1/12: unit variance
private:
    uint64_t s_;
};

class WideSynth {
public:
    // noise: complex white noise with unit total variance per sample. Carrier power is set so that its peak PSD stands snrDb over the noise PSD.
    WideSynth(double rate, std::vector<CarrierSpec> cars, double dcSpike = 0, uint64_t seed = 7) : fs_(rate), rng_(seed), dc_(dcSpike) {
        for (const auto& c : cars) {
            St s;
            s.c = c;
            s.a = 1.0 - 2.0 * M_PI * c.widthHz / rate * 0.8;
            // two cascaded poles: output variance of the filtered unit noise is 1/((1-a)(...)) ; normalise numerically below
            // peak PSD of the shaped carrier = gain^2 * |H(0)|^2 with H(0) = 1/(1-a)^2 for two poles; noise PSD = 1 per sample
            const double h0 = 1.0 / ((1 - s.a) * (1 - s.a));
            s.gain = std::sqrt(std::pow(10.0, c.snrDb / 10.0)) / h0;
            st_.push_back(s);
        }
    }
    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++, t_++) {
            const double time = (double)t_ / fs_;
            cf32 acc(rng_.gauss() * 0.7071068f, rng_.gauss() * 0.7071068f);
            for (auto& s : st_) {
                const cf32 w(rng_.gauss() * 0.7071068f, rng_.gauss() * 0.7071068f);
                s.y1 = (float)s.a * s.y1 + w;
                s.y2 = (float)s.a * s.y2 + s.y1;
                float amp = (float)s.gain;
                if (s.c.fadeDepthDb > 0) amp *= (float)std::pow(10.0, 0.5 * s.c.fadeDepthDb / 20.0 * std::sin(2 * M_PI * s.c.fadeHz * time));
                // phase of the carrier: offset plus Doppler (frequency = offset + rate * time)
                s.ph += 2 * M_PI * (s.c.offsetHz + s.c.dopplerHzPerSec * time) / fs_;
                if (s.ph > 2 * M_PI) s.ph -= 2 * M_PI;
                if (s.ph < -2 * M_PI) s.ph += 2 * M_PI;
                acc += s.y2 * amp * cf32((float)std::cos(s.ph), (float)std::sin(s.ph));
            }
            if (dc_ > 0) acc += cf32((float)dc_, 0);
            out[i] = acc;
        }
    }
private:
    struct St { CarrierSpec c; double a = 0.99, gain = 1, ph = 0; cf32 y1{}, y2{}; };
    double fs_;
    Rng rng_;
    double dc_;
    std::vector<St> st_;
    uint64_t t_ = 0;
};

} // namespace inmsat_test
