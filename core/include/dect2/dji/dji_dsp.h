// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sample-rate conversion for the DJI mode: a windowed-sinc polyphase table (256 phases, linearly interpolated), used by the receiver to bring a burst from the
// radio's rate to 15.36 Msps (centred on the burst) and by the test signal to go the other way.
#pragma once
#include "dect2/common/ring.h"
#include <cmath>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2::dji {

class Resampler {
public:
    // cutHz: the pass band edge (half the width of what is kept)
    Resampler(double inRate, double outRate, double cutHz) : inRate_(inRate), outRate_(outRate) {
        const double fc = cutHz / inRate;                       // cycles per input sample
        hw_ = (int)std::ceil(6.0 / (2 * fc));                   // six zero crossings each side
        taps_ = 2 * hw_;
        table_.assign((size_t)(kPhases + 1) * (size_t)taps_, 0.f);
        for (int p = 0; p <= kPhases; p++) {
            const double frac = (double)p / kPhases;            // the output sits frac after input sample j0
            for (int t = 0; t < taps_; t++) {
                const double d = (double)(t - hw_ + 1) - frac;  // distance of input sample (j0 + t - hw + 1) from the output
                const double a = 2 * fc * d;
                const double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(dect2::math::kPi * a) / (dect2::math::kPi * a);
                const double w = std::abs(d) >= hw_ ? 0.0 : 0.42 + 0.5 * std::cos(dect2::math::kPi * d / hw_) + 0.08 * std::cos(2 * dect2::math::kPi * d / hw_);
                table_[(size_t)p * (size_t)taps_ + (size_t)t] = (float)(2 * fc * sinc * w);
            }
        }
    }
    // Output m is taken at input position start + m * inRate / outRate (input samples; may be fractional). Positions outside the input read as zero.
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("fp-contract=off")))   // no fused multiply-add in one path only: the four-output and single paths must round the same
#endif
    std::vector<cf32> run(const cf32* in, size_t nIn, double start, size_t nOut) const {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        std::vector<cf32> out(nOut);
        const double step = inRate_ / outRate_;
        // Four outputs per pass: each keeps its own sequential sum (so every result is bit-identical to computing it alone), but the four chains
        // overlap in the CPU instead of waiting on each other's adds. Outputs whose window leaves the input go through the single path.
        constexpr int W = 4;
        struct Pos { const float* h0; const float* h1; long first; float a; };
        size_t m = 0;
        while (m < nOut) {
            Pos ps[W];
            bool inside = m + W <= nOut;
            for (int k = 0; k < W && inside; k++) {
                const double pos = start + (double)(m + (size_t)k) * step;
                const double fl = std::floor(pos);
                const long j0 = (long)fl;
                const double ph = (pos - fl) * kPhases;
                const int p0 = (int)ph;
                ps[k].a = (float)(ph - p0);
                ps[k].h0 = &table_[(size_t)p0 * (size_t)taps_];
                ps[k].h1 = ps[k].h0 + taps_;
                ps[k].first = j0 - hw_ + 1;
                if (ps[k].first < 0 || ps[k].first + taps_ > (long)nIn) inside = false;
            }
            if (inside) {
                float re[W] = {0, 0, 0, 0}, im[W] = {0, 0, 0, 0};
                const cf32* v[W] = {in + ps[0].first, in + ps[1].first, in + ps[2].first, in + ps[3].first};
                for (int t = 0; t < taps_; t++) {
                    for (int k = 0; k < W; k++) {
                        const float h = ps[k].h0[t] + ps[k].a * (ps[k].h1[t] - ps[k].h0[t]);
                        const cf32 x = v[k][t];
                        re[k] += h * x.real(); im[k] += h * x.imag();
                    }
                }
                for (int k = 0; k < W; k++) out[m + (size_t)k] = cf32(re[k], im[k]);
                m += W;
                continue;
            }
            const double pos = start + (double)m * step;
            const double fl = std::floor(pos);
            const long j0 = (long)fl;
            const double ph = (pos - fl) * kPhases;
            const int p0 = (int)ph;
            const float a = (float)(ph - p0);
            const float* h0 = &table_[(size_t)p0 * (size_t)taps_];
            const float* h1 = h0 + taps_;
            float re = 0, im = 0;
            const long first = j0 - hw_ + 1;
            const int lo = first < 0 ? (int)(-first) : 0;
            const int hi = first + taps_ > (long)nIn ? (int)((long)nIn - first) : taps_;
            for (int t = lo; t < hi; t++) {
                const float h = h0[t] + a * (h1[t] - h0[t]);
                const cf32 v = in[first + t];
                re += h * v.real(); im += h * v.imag();
            }
            out[m] = cf32(re, im);
            m++;
        }
        return out;
    }
    std::vector<cf32> run(const std::vector<cf32>& in, double start, size_t nOut) const { return run(in.data(), in.size(), start, nOut); }
private:
    static constexpr int kPhases = 256;
    double inRate_, outRate_;
    int hw_ = 0, taps_ = 0;
    std::vector<float> table_;
};

// multiply by exp(j 2 pi f n / rate) starting at sample n0
inline void mixInPlace(cf32* x, size_t n, double f, double rate, double n0 = 0) {
    const double step = 2 * dect2::math::kPi * f / rate;
    double ph = std::fmod(step * n0, 2 * dect2::math::kPi);
    cf32 p((float)std::cos(ph), (float)std::sin(ph));
    const cf32 inc((float)std::cos(step), (float)std::sin(step));
    for (size_t i = 0; i < n; i++) {
        x[i] *= p;
        p *= inc;
        if ((i & 1023) == 1023) p /= std::abs(p);
    }
}

} // namespace dect2::dji
