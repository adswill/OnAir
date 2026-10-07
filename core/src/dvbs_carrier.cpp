// QPSK carrier recovery, see dvbs_carrier.h.
#include "dvbs_carrier.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {
namespace dvbs {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

QpskEstimate qpskEstimate(const cf32* z, size_t n) {
    QpskEstimate e;
    int lg = 0;
    while ((size_t)(2 << lg) <= n && lg < 14) lg++;
    if (lg < 9) return e;
    const int N = 1 << lg;
    std::vector<float> re(N), im(N);
    for (int i = 0; i < N; i++) {
        // the fourth power of the symbol with its amplitude limited: a tone at four times the carrier offset
        const float a = std::abs(z[i]);
        cf32 u = a > 1e-9f ? z[i] / a : cf32(1, 0);
        const cf32 u2 = u * u;
        const cf32 u4 = u2 * u2;
        re[i] = u4.real(); im[i] = u4.imag();
    }
    fftSplit(re.data(), im.data(), lg, false);
    std::vector<float> mag(N);
    double sum = 0;
    for (int i = 0; i < N; i++) { mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]); sum += (double)mag[i] * mag[i]; }
    int kp = 0;
    for (int i = 1; i < N; i++) if (mag[i] > mag[kp]) kp = i;
    const double mean = (sum - (double)mag[kp] * mag[kp]) / (N - 1);
    e.ratio = (double)mag[kp] * mag[kp] / std::max(1e-30, mean);
    // parabolic interpolation of the line position
    const float a = mag[(kp + N - 1) % N], b = mag[kp], c = mag[(kp + 1) % N];
    const double den = (double)a - 2.0 * b + c;
    const double d = den != 0 ? 0.5 * ((double)a - c) / den : 0.0;
    double f4 = ((double)kp + d) / N;
    if (f4 > 0.5) f4 -= 1.0;
    e.freq = f4 / 4.0;
    // phase of the tone at the first symbol: the DFT value at the peak bin carries the phase of the tone plus the offset of the line from the bin centre
    const double argX = std::atan2((double)im[kp], (double)re[kp]);
    const double phi4 = argX - kPi * d * (N - 1.0) / N - kPi;      // the fourth power of a QPSK point is -1
    e.phase = std::fmod(phi4 / 4.0, kPi / 2);
    e.ok = e.ratio > 25.0;
    return e;
}

void QpskPll::setBandwidth(double bnT, double zeta) {
    const double kd = 1.4;
    const double th = bnT / (zeta + 1.0 / (4.0 * zeta));
    const double d = 1.0 + 2.0 * zeta * th + th * th;
    kp_ = 4.0 * zeta * th / d / kd;
    ki_ = 4.0 * th * th / d / kd;
}

void QpskPll::start(double freq, double phase, double bnT, double zeta) {
    theta_ = phase;
    omega_ = 2 * kPi * freq;
    setBandwidth(bnT, zeta);
    lock_ = 0; sigma2_ = 0.1f; amp_ = 0.70710678f;
    m2_ = 1; m4_ = 1; c4re_ = 0; count_ = 0;
}

void QpskPll::process(const cf32* z, size_t n, cf32* out) {
    double m2 = 0, m4 = 0, c4 = 0;
    // the detector is the maximum likelihood one for QPSK: the hard decisions are replaced by tanh of the soft ones, with the noise from the
    // moment estimate. At high signal to noise ratio this is the usual Costas detector.
    float gain = amp_ / std::max(sigma2_, 1e-3f);
    for (size_t i = 0; i < n; i++) {
        const float cs = (float)std::cos(theta_), sn = (float)std::sin(theta_);
        const cf32 y(z[i].real() * cs + z[i].imag() * sn, z[i].imag() * cs - z[i].real() * sn);    // z * e^{-j theta}
        out[i] = y;
        const float ti = std::tanh(gain * y.real()), tq = std::tanh(gain * y.imag());
        const float e = ti * y.imag() - tq * y.real();           // Im(y * conj(soft decision)) up to the constant factor A
        theta_ += omega_ + kp_ * e;
        omega_ += ki_ * e;
        if (theta_ > kPi) theta_ -= 2 * kPi; else if (theta_ < -kPi) theta_ += 2 * kPi;
        const double p = (double)y.real() * y.real() + (double)y.imag() * y.imag();
        m2 += p; m4 += p * p;
        const double a2 = (double)y.real() * y.real() - (double)y.imag() * y.imag();
        const double b2 = 2.0 * y.real() * y.imag();
        c4 += a2 * a2 - b2 * b2;                                   // Re(y^4)
        if ((++count_ & 255) == 0) {
            // noise per real dimension from the amplitude moments (constant modulus signal): S = sqrt(2 M2^2 - M4), N = M2 - S
            const double M2 = m2_, M4 = m4_;
            const double S = std::sqrt(std::max(0.0, 2 * M2 * M2 - M4)), N = std::max(1e-4, M2 - S);
            sigma2_ = (float)std::min(2.0, N / 2.0);
            amp_ = (float)std::sqrt(S / 2.0);
            gain = amp_ / std::max(sigma2_, 1e-3f);
            snrDb_ = (float)(10 * std::log10(std::max(1e-6, S / N)));
        }
    }
    if (n) {
        const double w = std::min(1.0, (double)n / 2048.0);
        m2_ += w * (m2 / n - m2_); m4_ += w * (m4 / n - m4_);
        const double pw = std::max(1e-9, m4 / n);
        c4re_ += w * (-(c4 / n) / pw - c4re_);
        lock_ = (float)std::max(0.0, std::min(1.0, c4re_ * (1.0 + 0.0)));
    }
}

} // namespace dvbs
} // namespace dect2
