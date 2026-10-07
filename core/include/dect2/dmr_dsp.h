// Filters shared by the DMR receiver and the DMR test signal.
#pragma once
#include "dsp_compat.h"
#include "ring.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace dect2 {
namespace dmr {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSymbolRate = 4800.0;
constexpr int kSps = 10;                       // samples per symbol at the working rate of 48 kHz
constexpr double kWorkRate = kSymbolRate * kSps;
constexpr double kDevUnitHz = 648.0;           // deviation of one level step (table 10.3): +-3 is 1944 Hz, +-1 is 648 Hz
constexpr double kRrcAlpha = 0.2;              // the square root raised cosine of clause 10.2.2.2 passes up to 1920 Hz and ends at 2880 Hz

// Square root raised cosine for the 4FSK pulse shape, 2*span*sps+1 taps with the sum of the taps equal to 1. Both the transmit filter (taps times sps,
// so that a run of equal symbols gives the nominal deviation) and the receive filter use it: together they are a raised cosine without intersymbol
// interference at the symbol instants.
inline std::vector<float> rrcTaps(int sps = kSps, int span = 12, double alpha = kRrcAlpha) {
    const int n = 2 * span * sps + 1, m = n / 2;
    std::vector<double> h(n);
    for (int i = 0; i < n; i++) {
        const double t = (double)(i - m) / sps;      // in symbol periods
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - alpha + 4.0 * alpha / kPi;
        else if (std::fabs(std::fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
        else v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h[i] = v;
    }
    double sum = 0;
    for (double v : h) sum += v;
    std::vector<float> o(n);
    for (int i = 0; i < n; i++) o[i] = (float)(h[i] / sum);
    return o;
}

// Kaiser-windowed low-pass with unity gain at DC
inline std::vector<float> lowpassTaps(double fpass, double fstop, double fs, double attenDb = 60, int minTaps = 9, int maxTaps = 1501) {
    fstop = std::min(fstop, fs * 0.5 * 0.999);
    const double fc = 0.5 * (fpass + fstop) / fs;
    const double df = std::max((fstop - fpass) / fs, 1e-4);
    int n = (int)std::ceil((attenDb - 7.95) / (14.36 * df)) + 1;
    n = std::min(maxTaps, std::max(minTaps, n)) | 1;
    const double beta = attenDb > 50 ? 0.1102 * (attenDb - 8.7) : 0.5842 * std::pow(attenDb - 21, 0.4) + 0.07886 * (attenDb - 21);
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    std::vector<float> h(n);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = x / (m + 0.5);
        const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// FIR filter that keeps every D-th output; T is float or cf32. Streams: any chunk sizes.
template <class T>
class Fir {
public:
    void design(std::vector<float> taps, int decim = 1) {
        h_ = std::move(taps);
        std::reverse(h_.begin(), h_.end());      // so that the dot product runs forward through the input
        d_ = std::max(1, decim);
        reset();
    }
    void reset() { hist_.assign(h_.size() > 0 ? h_.size() - 1 : 0, T()); phase_ = 0; }
    int decim() const { return d_; }
    size_t taps() const { return h_.size(); }
    void process(const T* in, size_t n, std::vector<T>& out) {
        const size_t nt = h_.size();
        if (!nt) return;
        const size_t base = nt - 1;
        // output j (input index of the newest sample it uses) every d_ samples; phase_ counts the inputs since the last output. Output j uses the
        // nt samples x_[j .. j + nt - 1] of the history-extended buffer: the decimating correlation of dsp_compat.
        const size_t j0 = (size_t)((d_ - 1 - phase_) % d_);
        const size_t nout = j0 < n ? (n - j0 + (size_t)d_ - 1) / (size_t)d_ : 0;
        if constexpr (std::is_same_v<T, cf32>) {
            // the real and the imaginary parts are two real streams
            xr_.resize(base + n); xi_.resize(base + n);
            for (size_t i = 0; i < base; i++) { xr_[i] = hist_[i].real(); xi_[i] = hist_[i].imag(); }
            for (size_t i = 0; i < n; i++) { xr_[base + i] = in[i].real(); xi_[base + i] = in[i].imag(); }
            if (nout) {
                yr_.resize(nout); yi_.resize(nout);
                desamp(xr_.data() + j0, d_, h_.data(), yr_.data(), (int)nout, (int)nt);
                desamp(xi_.data() + j0, d_, h_.data(), yi_.data(), (int)nout, (int)nt);
                const size_t o = out.size();
                out.resize(o + nout);
                for (size_t i = 0; i < nout; i++) out[o + i] = cf32(yr_[i], yi_[i]);
            }
            for (size_t i = 0; i < base; i++) hist_[i] = cf32(xr_[n + i], xi_[n + i]);
        } else {
            x_.resize(base + n);
            std::copy(hist_.begin(), hist_.end(), x_.begin());
            std::copy(in, in + n, x_.begin() + (ptrdiff_t)base);
            if (nout) {
                const size_t o = out.size();
                out.resize(o + nout);
                desamp(x_.data() + j0, d_, h_.data(), out.data() + o, (int)nout, (int)nt);
            }
            std::copy(x_.end() - (ptrdiff_t)base, x_.end(), hist_.begin());
        }
        phase_ = (int)((phase_ + n) % (size_t)d_);
    }

private:
    std::vector<float> h_;
    std::vector<T> hist_, x_;
    std::vector<float> xr_, xi_, yr_, yi_;
    int d_ = 1;
    int phase_ = 0;
};

} // namespace dmr
} // namespace dect2
