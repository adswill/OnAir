// Narrowband-interferer erasure for the OFDM TV receivers (DVB-T, DVB-T2, ISDB-T).
//
// The receivers already weight every cell by its channel state (n0 = noise / |H|^2, so a faded carrier gives soft values of low reliability).
// What that cannot see is a carrier that is bad for another reason: a CW tone, an analogue carrier or LTE leakage lands on a few carriers and
// adds power that the pilot-based noise estimate (spread over the whole channel) does not contain. This tracker watches each carrier's
// equalised power over time. Data cells have unit mean power (all the constellations are normalised), so a carrier whose slow average
// power sits well above 1 + 2 n0 is carrying something that is not signal plus the common noise; its noise variance is raised by the
// excess (and the cell erased when the excess is huge). On clean carriers nothing changes (the threshold is several sigma of the average).
// DECT2_OFDM_NO_ERASE=1 switches it off, DECT2_OFDM_FLAT_N0=1 additionally drops the channel-state weighting (both for A/B tests only).
#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <vector>

namespace dect2::ofdm {

inline bool envOn(const char* name) { const char* v = getenv(name); return v && *v && *v != '0'; }

class CarrierInterference {
public:
    CarrierInterference() : off_(envOn("DECT2_OFDM_NO_ERASE")) {}
    void reset(int K) { pw_.assign((size_t)K, 0.f); nz_.assign((size_t)K, 0.f); cnt_.assign((size_t)K, 0); lastErased_ = 0; }
    bool enabled() const { return !off_; }
    void setEnabled(bool on) { off_ = !on; }
    int lastErased() const { return lastErased_; }
    // eq: equalised cells of one symbol; n0: their noise variance per real dimension (raised in place); idx: carrier of each cell (null: 0..n-1)
    void apply(const std::complex<float>* eq, float* n0, const int* idx, int n, int K) {
        if (off_) return;
        if ((int)pw_.size() != K) reset(K);
        int erased = 0;
        for (int i = 0; i < n; i++) {
            const int k = idx ? idx[i] : i;
            if (k < 0 || k >= K || n0[i] > 1e3f) continue;   // an already erased cell tells nothing about the carrier
            const float p = std::norm(eq[i]), nz = 2.f * n0[i];
            const float lim = 40.f * (1.f + nz);
            const uint16_t c = cnt_[(size_t)k];
            const float a = std::max(kAlpha, 1.f / (float)(c + 1));
            pw_[(size_t)k] += a * (std::min(p, lim) - pw_[(size_t)k]);
            nz_[(size_t)k] += a * (nz - nz_[(size_t)k]);
            if (c < 65000) cnt_[(size_t)k] = (uint16_t)(c + 1);
            if (c < kWarm) continue;
            const float expect = 1.f + nz_[(size_t)k];
            const float ratio = pw_[(size_t)k] / expect;
            if (ratio < kThresh) continue;
            if (ratio > kErase) { n0[i] = 1e6f; erased++; }
            else { n0[i] += 0.5f * (pw_[(size_t)k] - expect); erased++; }
        }
        lastErased_ = erased;
    }
private:
    static constexpr float kAlpha = 1.f / 32.f, kThresh = 1.6f, kErase = 10.f;
    static constexpr uint16_t kWarm = 24;
    bool off_ = false;
    std::vector<float> pw_, nz_;
    std::vector<uint16_t> cnt_;
    int lastErased_ = 0;
};

}  // namespace dect2::ofdm
