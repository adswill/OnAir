// Radio sample formats to complex float, and the engine's non-finite guard: the per-sample work every mode pays before its receiver.
#pragma once
#include "dect2/common/ring.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dect2 {

// int8 pairs (HackRF, cs8 files): x / 128, exact
inline void convertCs8(const int8_t* p, size_t n, cf32* o) {
    for (size_t k = 0; k < n; k++) o[k] = cf32(p[2 * k] * (1.0f / 128), p[2 * k + 1] * (1.0f / 128));
}

// The same straight into a ring, in pieces of 4096 samples
inline void convertCs8Ring(const int8_t* p, size_t n, IqRing& ring) {
    constexpr size_t kChunk = 4096;
    cf32 tmp[kChunk];
    for (size_t i = 0; i < n;) {
        const size_t m = std::min(kChunk, n - i);
        convertCs8(p + 2 * i, m, tmp);
        ring.write(tmp, m);
        i += m;
    }
}

// uint8 pairs (RTL-SDR): (x - offset) / den, the division as written (not a multiplication by 1 / den) so the bits are those of the old loops
inline void convertCu8(const uint8_t* b, size_t n, cf32* o, float offset, float den) {
    for (size_t i = 0; i < n; i++) o[i] = cf32((b[2 * i] - offset) / den, (b[2 * i + 1] - offset) / den);
}

// int16 pairs: x * scale (scale = 1/32768 for full scale)
inline void convertCs16(const int16_t* s, size_t n, cf32* o, float scale) {
    for (size_t i = 0; i < n; i++) o[i] = cf32(s[2 * i] * scale, s[2 * i + 1] * scale);
}

// A radio that delivers floating-point samples (LimeSDR, SoapySDR, a cf32 recording) can hand over NaN or infinite values after a
// glitch or with mismatched firmware. They would poison every filter, FFT and gain loop after them, so they become silence here. So do
// values of 2^20 and more: no radio delivers them (full scale is 1, or 32768 for a driver that hands over unscaled 16-bit samples), and
// squared and summed in a filter or an FFT they overflow to infinity, which then reaches float-to-int conversions (undefined behaviour,
// found by fuzz_engine_modes with samples of 1e30 and 3.4e38).
constexpr uint32_t kSampleExpLimit = (127u + 20u) << 23;   // the exponent bits of 2^20
inline void dropNonFinite(cf32* x, size_t n) {
    // looks at the exponent bits of every float: at or above 2^20 (all ones: NaN or infinity). One branch per block of 256 values, the loop vectorises.
    static_assert(sizeof(cf32) == 2 * sizeof(float), "cf32 layout");
    uint32_t* w = reinterpret_cast<uint32_t*>(x);
    const size_t total = 2 * n;
    for (size_t b = 0; b < total; b += 256) {
        const size_t e = std::min<size_t>(b + 256, total);
        uint32_t bad = 0;
        for (size_t i = b; i < e; i++) bad |= (uint32_t)((w[i] & 0x7F800000u) >= kSampleExpLimit);
        if (bad)
            for (size_t i = b & ~(size_t)1; i < e; i += 2)
                if ((w[i] & 0x7F800000u) >= kSampleExpLimit || (w[i + 1] & 0x7F800000u) >= kSampleExpLimit) { w[i] = 0; w[i + 1] = 0; }
    }
}

} // namespace dect2
