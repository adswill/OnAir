// The fourth-order CIC decimator the DCS receivers share (goesdcs_live.cpp, hrdcp_live.cpp). Internal to core/src/goesdcs.
#pragma once
#include "dect2/common/ring.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dect2 {
namespace dcsdsp {

// Fourth-order CIC decimator on integers (wrap-around arithmetic), complex.
struct Cic4 {
    int D = 1, cnt = 0;
    uint64_t in[4][2] = {}, cb[4][2] = {};
    double norm = 1;
    void configure(int d) {
        D = std::max(1, d); cnt = 0;
        for (auto& a : in) a[0] = a[1] = 0;
        for (auto& a : cb) a[0] = a[1] = 0;
        norm = 1.0 / (16777216.0 * std::pow((double)D, 4));
    }
    static int64_t q(float v) {
        v = std::max(-8.f, std::min(8.f, v));
        return (int64_t)std::lrintf(v * 16777216.f);
    }
    bool push(cf32 x, cf32& out) {
        uint64_t s0 = (uint64_t)q(x.real()), s1 = (uint64_t)q(x.imag());
        for (int k = 0; k < 4; k++) { in[k][0] += s0; in[k][1] += s1; s0 = in[k][0]; s1 = in[k][1]; }
        if (++cnt < D) return false;
        cnt = 0;
        for (int k = 0; k < 4; k++) {
            const uint64_t t0 = s0 - cb[k][0], t1 = s1 - cb[k][1];
            cb[k][0] = s0; cb[k][1] = s1; s0 = t0; s1 = t1;
        }
        out = cf32((float)((double)(int64_t)s0 * norm), (float)((double)(int64_t)s1 * norm));
        return true;
    }
};

} // namespace dcsdsp
} // namespace dect2
