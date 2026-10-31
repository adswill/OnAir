// Small helpers shared by the SSB conversation sources.
#pragma once
#include "dect2/ssbconv/ssbconv.h"
#include <cmath>
#include <utility>
#include <vector>

namespace dect2::ssbconv::detail {
inline void fft(std::vector<cf>& a) {
    constexpr double kPi = 3.14159265358979323846;
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const cf wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cf u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
}  // namespace dect2::ssbconv::detail
