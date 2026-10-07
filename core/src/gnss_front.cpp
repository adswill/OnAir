// Front end of one GNSS signal band (see gnss_front.h).
#include "dect2/gnss_front.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "dect2/dsp_compat.h"

namespace dect2 {

namespace {
double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
constexpr int kMixBlock = 256;
}

void GnssBand::init(double fsIn, double offsetHz, double fsOut, size_t keepSamples) {
    fsIn_ = fsIn; fsOut_ = fsOut; offset_ = offsetHz; ratio_ = fsIn / fsOut; keep_ = keepSamples;
    // Kaiser windowed sinc, cutoff at half the output rate (or 0.47 of the input rate when that is lower); the length follows the transition width
    int taps = (int)std::ceil(10.3 * std::max(1.0, ratio_));
    taps = std::min(128, std::max(16, (taps + 3) & ~3));
    taps_ = taps;
    const double fc = std::min(0.5 * fsOut, 0.47 * fsIn) / fsIn;      // cycles per input sample
    const double beta = 6.5, i0b = bessel0(beta);
    h_.assign((size_t)kPhases * taps_ * 2, 0.f);
    for (int q = 0; q < kPhases; q++) {
        const double frac = (double)q / kPhases;
        double sum = 0;
        std::vector<double> t(taps_);
        for (int k = 0; k < taps_; k++) {
            const double x = (k - (taps_ / 2 - 1)) - frac;                 // distance of tap k from the output position, input samples
            const double arg = 2 * fc * x;
            const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(M_PI * arg) / (M_PI * arg);
            const double r = x / (taps_ / 2.0);
            const double win = std::fabs(r) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - r * r)) / i0b;
            t[k] = sinc * win;
            sum += t[k];
        }
        for (int k = 0; k < taps_; k++) { h_[((size_t)q * taps_ + k) * 2] = (float)(t[k] / sum); h_[((size_t)q * taps_ + k) * 2 + 1] = (float)(t[k] / sum); }
    }
    mix_.resize(kMixBlock);
    const double w = -2 * M_PI * offset_ / fsIn_;
    for (int i = 0; i < kMixBlock; i++) mix_[i] = cf32((float)std::cos(w * i), (float)std::sin(w * i));
    reset();
}

void GnssBand::reset() {
    hist_.assign((size_t)taps_, cf32(0.f, 0.f));
    inBase_ = -taps_;
    nextOut_ = 0; inTotal_ = 0; mixPhase_ = 0;
    buf_.clear();
    base_ = 0;
    raw_.clear(); rawBase_ = 0; exNextFrame_ = 0;
    exTail_.assign((size_t)kExHop, cf32(0.f, 0.f));
    excisedBins = frames = 0;
    if (exWin_.empty()) {
        exWin_.resize(kExN);
        for (int i = 0; i < kExN; i++) exWin_[i] = (float)std::sin(M_PI * (i + 0.5) / kExN);     // sqrt of a Hann window
        exRe_.resize(kExN); exIm_.resize(kExN); exPow_.resize(kExN); exA_.resize(kExN); exB_.resize(kExN); exKill_.resize(kExN);
    }
}

void GnssBand::excise() {
    // Frames start every kExHop samples. Frame j is complete when raw_ holds its kExN samples; its first half completes the output of the previous half-frame.
    while (rawBase_ + (int64_t)raw_.size() >= exNextFrame_ + kExN) {
        const cf32* x = &raw_[(size_t)(exNextFrame_ - rawBase_)];
        for (int i = 0; i < kExN; i++) { exRe_[i] = x[i].real() * exWin_[i]; exIm_[i] = x[i].imag() * exWin_[i]; }
        bool cut = false;
        if (exEnable_) {
            exA_ = exRe_; exB_ = exIm_;
            fftSplit(exRe_.data(), exIm_.data(), 11, false);
            double sum = 0;
            for (int i = 0; i < kExN; i++) { exPow_[i] = exRe_[i] * exRe_[i] + exIm_[i] * exIm_[i]; sum += exPow_[i]; }
            // bins far above the mean power (a continuous wave holds a few bins of a flat spectrum): the mean is little raised by the few lines that matter
            const float thr = (float)(14.0 * sum / kExN);
            std::fill(exKill_.begin(), exKill_.end(), 0);
            for (int i = 0; i < kExN; i++)
                if (exPow_[i] > thr) { cut = true; for (int d = -2; d <= 2; d++) exKill_[(i + d + kExN) % kExN] = 1; }
            if (cut) {
                for (int i = 0; i < kExN; i++) if (exKill_[i]) { exRe_[i] = 0; exIm_[i] = 0; excisedBins++; }
                fftSplit(exRe_.data(), exIm_.data(), 11, true);
                const float sc = 1.f / kExN;
                for (int i = 0; i < kExN; i++) { exRe_[i] *= sc; exIm_[i] *= sc; }
            } else { exRe_ = exA_; exIm_ = exB_; }
        }
        // overlap-add: output the first half (this frame's first half plus the previous frame's second half)
        for (int i = 0; i < kExHop; i++) {
            buf_.push_back(cf32(exTail_[i].real() + exRe_[i] * exWin_[i], exTail_[i].imag() + exIm_[i] * exWin_[i]));
            exTail_[i] = cf32(exRe_[kExHop + i] * exWin_[kExHop + i], exIm_[kExHop + i] * exWin_[kExHop + i]);
        }
        frames++;
        exNextFrame_ += kExHop;
        // forget raw samples that no later frame needs
        const int64_t drop = exNextFrame_ - rawBase_;
        if (drop > 4 * kExN) { raw_.erase(raw_.begin(), raw_.begin() + (long)drop); rawBase_ += drop; }
    }
}

void GnssBand::process(const cf32* x, size_t n) {
    if (!valid() || n == 0) return;
    // mix into the tail of hist_
    const size_t old = hist_.size();
    hist_.resize(old + n);
    cf32* m = hist_.data() + old;
    const double w = -2 * M_PI * offset_ / fsIn_;
    if (offset_ == 0.0) {
        std::memcpy(m, x, n * sizeof(cf32));
    } else {
        size_t i = 0;
        while (i < n) {
            const size_t len = std::min<size_t>(kMixBlock, n - i);
            const double ph = mixPhase_ * 2 * M_PI;
            const float cr = (float)std::cos(ph), ci = (float)std::sin(ph);
            const float* xr = reinterpret_cast<const float*>(x + i);
            const float* wr = reinterpret_cast<const float*>(mix_.data());
            float* o = reinterpret_cast<float*>(m + i);
            for (size_t k = 0; k < len; k++) {
                // (x * w[k]) * c
                const float a = xr[2 * k] * wr[2 * k] - xr[2 * k + 1] * wr[2 * k + 1];
                const float b = xr[2 * k] * wr[2 * k + 1] + xr[2 * k + 1] * wr[2 * k];
                o[2 * k] = a * cr - b * ci;
                o[2 * k + 1] = a * ci + b * cr;
            }
            mixPhase_ += (w * (double)len) / (2 * M_PI);
            mixPhase_ -= std::floor(mixPhase_);
            i += len;
        }
    }
    inTotal_ += (int64_t)n;
    // produce outputs while the filter window lies inside the data: output m centre at input position m * ratio; taps start (taps/2 - 1) before it
    const int64_t inEnd = inBase_ + (int64_t)hist_.size();
    while (true) {
        const double pos = (double)nextOut_ * ratio_;
        const int64_t fl = (int64_t)std::floor(pos);
        int q = (int)std::lround((pos - (double)fl) * kPhases);
        int64_t i0 = fl - (taps_ / 2 - 1);
        if (q == kPhases) { q = 0; i0 += 1; }
        if (i0 + taps_ > inEnd) break;
        const float* h = &h_[(size_t)q * taps_ * 2];
        const float* s = reinterpret_cast<const float*>(hist_.data() + (i0 - inBase_));
        // taps are stored twice (for the real and imaginary parts), so the sum over interleaved samples is one plain dot product in eight lanes
        float acc[8] = {};
        for (int k = 0; k < taps_ * 2; k += 8)
            for (int l = 0; l < 8; l++) acc[l] += h[k + l] * s[k + l];
        raw_.push_back(cf32(acc[0] + acc[2] + acc[4] + acc[6], acc[1] + acc[3] + acc[5] + acc[7]));
        nextOut_++;
    }
    excise();
    // keep only what the next outputs need
    const int64_t needFrom = (int64_t)std::floor((double)nextOut_ * ratio_) - (taps_ / 2 - 1) - 1;
    if (needFrom > inBase_) {
        const size_t drop = (size_t)std::min<int64_t>(needFrom - inBase_, (int64_t)hist_.size());
        hist_.erase(hist_.begin(), hist_.begin() + (long)drop);
        inBase_ += (int64_t)drop;
    }
}

void GnssBand::trimTo(int64_t idx) {
    const int64_t end = base_ + (int64_t)buf_.size();
    const int64_t floorIdx = end - (int64_t)keep_;
    int64_t cut = std::min(idx, floorIdx);
    if (cut <= base_ + (int64_t)keep_ / 2) return;       // trim in big steps
    cut = std::min(cut, end);
    buf_.erase(buf_.begin(), buf_.begin() + (long)(cut - base_));
    base_ = cut;
}

} // namespace dect2
