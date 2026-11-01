// Mixer + 4-stage CIC decimator in integers (wraps cleanly), input scaled to 16 bits: the front end of every Aero channel (P and C).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

namespace dect2 {

class AeroFront {
public:
    void setup(double fsIn, double freqHz, int decim) {
        fsIn_ = fsIn; D_ = decim;
        setFreq(freqHz);
        osc_ = {1.0, 0.0};
        std::fill(std::begin(iI_), std::end(iI_), 0); std::fill(std::begin(iQ_), std::end(iQ_), 0);
        std::fill(std::begin(cI_), std::end(cI_), 0); std::fill(std::begin(cQ_), std::end(cQ_), 0);
        cnt_ = 0;
        gain_ = 1.0 / (std::pow((double)D_, 4) * 1048576.0);
    }
    void setFreq(double hz) {
        freq_ = hz;
        const double w = -2 * dect2::math::kPi * hz / fsIn_;
        rot_ = {std::cos(w), std::sin(w)};
    }
    double freq() const { return freq_; }
    // returns the number of outputs written to out
    size_t process(const cf32* x, size_t n, cf32* out) {
        size_t m = 0;
        float oR = (float)osc_.real(), oI = (float)osc_.imag();
        const float rR = (float)rot_.real(), rI = (float)rot_.imag();
        constexpr float kScale = 1048576.f;          // 2^20: truncation then leaves a bias far below any carrier
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real() * oR - x[i].imag() * oI;
            const float im = x[i].real() * oI + x[i].imag() * oR;
            const float nr = oR * rR - oI * rI;
            oI = oR * rI + oI * rR;
            oR = nr;
            int64_t a = (int64_t)(re * kScale), b = (int64_t)(im * kScale);
            for (int s = 0; s < 4; s++) { iI_[s] = (int64_t)((uint64_t)iI_[s] + (uint64_t)a); a = iI_[s]; iQ_[s] = (int64_t)((uint64_t)iQ_[s] + (uint64_t)b); b = iQ_[s]; }
            if (++cnt_ < D_) continue;
            cnt_ = 0;
            for (int s = 0; s < 4; s++) {
                const int64_t ta = (int64_t)((uint64_t)a - (uint64_t)cI_[s]); cI_[s] = a; a = ta;
                const int64_t tb = (int64_t)((uint64_t)b - (uint64_t)cQ_[s]); cQ_[s] = b; b = tb;
            }
            out[m++] = cf32((float)((double)a * gain_), (float)((double)b * gain_));
            // keep the oscillator on the unit circle (float rounding), once per output sample
            const float g = 1.5f - 0.5f * (oR * oR + oI * oI);
            oR *= g; oI *= g;
        }
        osc_ = {oR, oI};
        return m;
    }
private:
    double fsIn_ = 1, freq_ = 0, gain_ = 1;
    int D_ = 1, cnt_ = 0;
    std::complex<double> osc_{1, 0}, rot_{1, 0};
    int64_t iI_[4] = {}, iQ_[4] = {}, cI_[4] = {}, cQ_[4] = {};
};

} // namespace dect2
