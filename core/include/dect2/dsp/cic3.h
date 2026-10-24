// Three-stage CIC decimator core for complex samples, exact for as long as it runs: fixed point with wrapping 64-bit integrators (the
// two's-complement wrap cancels in the combs). The double-precision integrators this replaces grow without bound (the third like N^2.5 on
// noise) and lose their precision: at 30 Msps and a decimation of 300 the output was below the error after about a minute, which looked like
// a receiver whose lock decays until a retune resets it.
//
// Budget: 2^20 per unit (a quantum of about -120 dBFS), inputs clamped to +-16, 3 log2(D) bits of growth: D up to 1000 stays inside 64 bits.
// comb() returns the sum of the window in input units (D^3 times the mean, as the double version did).
#pragma once
#include <algorithm>
#include <complex>
#include <cstdint>

namespace dect2 {

struct Cic3 {
    static constexpr double kScale = 1048576.0;
    uint64_t ir[3] = {}, ii[3] = {}, cr[3] = {}, ci[3] = {};

    void reset() { for (int k = 0; k < 3; k++) ir[k] = ii[k] = cr[k] = ci[k] = 0; }

    static uint64_t fixed(double x) { return (uint64_t)(int64_t)((x == x ? std::clamp(x, -16.0, 16.0) : 0.0) * kScale); }   // NaN: 0
    void integrate(std::complex<double> v) {
        const uint64_t re = fixed(v.real()), im = fixed(v.imag());
        ir[0] += re; ir[1] += ir[0]; ir[2] += ir[1];
        ii[0] += im; ii[1] += ii[0]; ii[2] += ii[1];
    }

    std::complex<double> comb() {
        uint64_t r = ir[2], i = ii[2];
        for (int k = 0; k < 3; k++) {
            const uint64_t tr = r, ti = i;
            r -= cr[k]; i -= ci[k];
            cr[k] = tr; ci[k] = ti;
        }
        return {(double)(int64_t)r / kScale, (double)(int64_t)i / kScale};
    }
};

} // namespace dect2
