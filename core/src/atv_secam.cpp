// Analog TV receiver, SECAM colour. See atv_secam.h.
#include "atv_secam.h"
#include "atv_dsp.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

using namespace atvdsp;

constexpr int kBellTaps = 27;       // 10 us at 2.5 Msps: the bell (Q about 16 at 4.3 MHz) rings for a few microseconds

void AtvSecam::configure(double videoRate) {
    fv_ = videoRate;
    dec_ = std::max(1, (int)std::lround(videoRate / 2.5e6));
    fd_ = videoRate / dec_;
    // The subcarrier and its FM sidebands: 4.286 MHz +-0.9 MHz. What lies above fd - 0.9 MHz would fold into that band.
    lp_ = lowpass(0.9e6, std::max(1.3e6, fd_ - 0.9e6), videoRate, 45, 121);
    hlf_ = (int)lp_.size() / 2;
    auto resp = [&](double f) -> std::complex<double> { return 1.0 / atvSecamHfPreEmph(kSecamBellHz + f); };
    const auto t = fromResponse(resp, fd_, kBellTaps, 5.0);
    bell_.resize(t.size());
    for (size_t k = 0; k < t.size(); k++) bell_[k] = cx((float)t[k].real(), (float)t[k].imag());
    post_ = lowpass(0.7e6, std::min(1.25e6, 0.49 * fd_), fd_, 35, 25);
    // low-frequency de-emphasis: the inverse of (1 + s/w1) / (1 + s/(3 w1)), bilinear
    const double a = 2 * fd_ / (2 * kPi * kSecamLf1Hz);
    b0_ = 1 + a / 3; b1_ = 1 - a / 3; a0_ = 1 + a; a1_ = 1 - a;
}

size_t AtvSecam::baseband(const float* zi, const float* zq, size_t n, std::vector<cx>& out) {
    out.clear();
    const size_t nt = lp_.size();
    if (n < nt) return 0;
    mi_.resize(n); mq_.resize(n);
    const double w = -2 * kPi * kSecamBellHz / fv_;
    {
        const std::complex<double> r1 = std::polar(1.0, w);
        std::complex<float> p[4];
        for (int q = 0; q < 4; q++) p[q] = std::complex<float>(std::pow(r1, q));
        const std::complex<float> r4(std::pow(r1, 4));
        size_t j = 0;
        for (; j + 4 <= n; j += 4) {
            for (int q = 0; q < 4; q++) {
                mi_[j + q] = zi[j + q] * p[q].real() - zq[j + q] * p[q].imag();
                mq_[j + q] = zi[j + q] * p[q].imag() + zq[j + q] * p[q].real();
                p[q] *= r4;
            }
            if ((j & 127) == 124) for (int q = 0; q < 4; q++) p[q] /= std::abs(p[q]);
        }
        for (; j < n; j++) {
            const std::complex<float> v = std::complex<float>(zi[j], zq[j]) * p[0];
            mi_[j] = v.real(); mq_[j] = v.imag();
            p[0] *= std::complex<float>(r1);
        }
    }
    const size_t m = (n - nt) / (size_t)dec_ + 1;
    out.resize(m);
    ti_.resize(m); tq_.resize(m);
    desamp(mi_.data(), dec_, lp_.data(), ti_.data(), (int)m, (int)nt);
    desamp(mq_.data(), dec_, lp_.data(), tq_.data(), (int)m, (int)nt);
    for (size_t k = 0; k < m; k++) out[k] = cx(ti_[k], tq_[k]);
    return m;
}

void AtvSecam::tone(const std::vector<cx>& bb, size_t a, size_t b, double& fHz, double& amp) const {
    fHz = 0; amp = 0;
    b = std::min(b, bb.size());
    if (b < a + 4) return;
    std::complex<double> acc = 0;
    for (size_t m = a + 1; m < b; m++) {
        const double wgt = 0.5 - 0.5 * std::cos(2 * kPi * ((double)(m - a) - 0.5) / (double)(b - a));
        const std::complex<double> x(bb[m].real(), bb[m].imag()), y(bb[m - 1].real(), bb[m - 1].imag());
        acc += wgt * x * std::conj(y);
    }
    const double phi = std::arg(acc);                       // radians per output
    fHz = kSecamBellHz + phi * fd_ / (2 * kPi);
    std::complex<double> s = 0;
    double w2 = 0;
    for (size_t m = a; m < b; m++) {
        const double wgt = 0.5 - 0.5 * std::cos(2 * kPi * ((double)(m - a) + 0.5) / (double)(b - a));
        s += wgt * std::complex<double>(bb[m].real(), bb[m].imag()) * std::polar(1.0, -phi * (double)(m - a));
        w2 += wgt;
    }
    amp = std::abs(s) / std::max(w2, 1e-9);
}

void AtvSecam::demod(const std::vector<cx>& bb, bool isR, std::vector<float>& d, float gate, size_t ra, size_t rb) {
    const size_t M = bb.size();
    d.assign(M, 0.f);
    if (M < 4) return;
    // the bell: centred convolution, the stretch outside the data is taken as zero
    const long nb = (long)bell_.size(), mb = nb / 2;
    belled_.resize(M);
    for (long m = 0; m < (long)M; m++) {
        cx acc(0, 0);
        const long t0 = std::max(0L, m + mb - ((long)M - 1)), t1 = std::min(nb, m + mb + 1);
        for (long t = t0; t < t1; t++) acc += bell_[(size_t)t] * bb[(size_t)(m + mb - t)];
        belled_[(size_t)m] = acc;
    }
    {
        double sum = 0;
        size_t c = 0;
        for (size_t m = ra; m < std::min(rb, M); m++) { sum += std::abs(belled_[m]); c++; }
        amp_ = c ? (float)(sum / (double)c) : 0.f;
    }
    // discriminator: the phase step from the output before; where there is no subcarrier (nothing, or noise) the frequency is the rest frequency
    const double f0 = isR ? kSecamF0R : kSecamF0B, dev = isR ? kSecamDevR : kSecamDevB;
    const float g2 = gate * gate;
    disc_.assign(M, 0.f);
    for (size_t m = 1; m < M; m++) {
        const cx pr = belled_[m] * std::conj(belled_[m - 1]);
        if (std::norm(pr) < g2 * g2) { disc_[m] = 0.f; continue; }
        disc_[m] = (float)((kSecamBellHz + (double)fastAtan2(pr.imag(), pr.real()) * fd_ / (2 * kPi) - f0) / dev);
    }
    // de-emphasis
    double xp = 0, yp = 0;
    for (size_t m = 0; m < M; m++) {
        const double x = disc_[m];
        const double y = (b0_ * x + b1_ * xp - a1_ * yp) / a0_;
        xp = x; yp = y;
        disc_[m] = (float)y;
    }
    // low-pass (centred), the ends taken as the nearest value
    const long np = (long)post_.size(), mp = np / 2;
    for (long m = 0; m < (long)M; m++) {
        float acc = 0;
        for (long t = 0; t < np; t++) { const long q = std::min((long)M - 1, std::max(0L, m + t - mp)); acc += post_[(size_t)t] * disc_[(size_t)q]; }
        d[(size_t)m] = acc;
    }
}

} // namespace dect2
