// Shared helpers of the W1A voiceband modes (private to core/src/w1a).
#pragma once
#include "dect2/w1a/w1a_dsp.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace dect2::w1a {

constexpr double kPi = 3.14159265358979323846;

// Windowed-sinc low-pass (Hamming), streaming
class Fir {
public:
    Fir() = default;
    Fir(double cutoffHz, int taps) { design(cutoffHz, taps); }
    void design(double cutoffHz, int taps) {
        n_ = taps | 1;
        h_.assign((size_t)n_, 0.f);
        const int c = n_ / 2;
        const double fc = cutoffHz / kRate;
        double s = 0;
        for (int i = 0; i < n_; i++) {
            const double t = i - c;
            const double v = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
            h_[(size_t)i] = (float)(v * (0.54 - 0.46 * std::cos(2 * kPi * i / (n_ - 1))));
            s += h_[(size_t)i];
        }
        for (float& v : h_) v = (float)(v / s);
        d_.assign((size_t)(2 * n_), 0.f);
        w_ = 0;
    }
    int delay() const { return n_ / 2; }
    float process(float x) {
        d_[(size_t)w_] = d_[(size_t)(w_ + n_)] = x;
        w_ = (w_ + 1) % n_;
        float acc = 0;
        for (int i = 0; i < n_; i++) acc += d_[(size_t)(w_ + i)] * h_[(size_t)i];
        return acc;
    }
private:
    std::vector<float> h_, d_;
    int n_ = 1, w_ = 0;
};

std::unique_ptr<Decoder> makePacketDecoder();
std::unique_ptr<Decoder> makeInvertDecoder();
std::unique_ptr<Decoder> makeEltDecoder();

}  // namespace dect2::w1a
