// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab: small DSP helpers (FFT, Welch spectrum, mixer, channeliser). Self-contained so the library has no dependencies.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::signallab {

size_t nextPow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    if (n < 2) return;
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / (double)len * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= (double)n;
}

std::vector<double> psd(const cf* x, size_t n, size_t nfft) {
    nfft = nextPow2(std::max<size_t>(nfft, 16));
    std::vector<double> out(nfft, 0.0);
    if (!n) return out;
    std::vector<double> win(nfft);
    double wsum = 0;
    for (size_t i = 0; i < nfft; i++) { win[i] = 0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)nfft); wsum += win[i] * win[i]; }
    const size_t hop = nfft / 2;
    size_t segs = 0;
    std::vector<std::complex<double>> b(nfft);
    for (size_t s = 0; s == 0 || s + nfft <= n; s += hop) {
        for (size_t i = 0; i < nfft; i++) b[i] = s + i < n ? std::complex<double>(x[s + i]) * win[i] : 0.0;
        fft(b);
        for (size_t i = 0; i < nfft; i++) out[(i + nfft / 2) % nfft] += std::norm(b[i]);
        segs++;
        if (s + nfft >= n) break;
    }
    for (auto& v : out) v /= (double)segs * wsum;
    return out;
}

std::vector<cf> mix(const std::vector<cf>& x, double fs, double hz) {
    std::vector<cf> y(x.size());
    if (hz == 0 || fs <= 0) { y = x; return y; }
    const double w = 2 * M_PI * hz / fs;
    std::complex<double> ph(1, 0);
    const std::complex<double> step(std::cos(w), std::sin(w));
    for (size_t i = 0; i < x.size(); i++) {
        y[i] = cf((float)(x[i].real() * ph.real() - x[i].imag() * ph.imag()), (float)(x[i].real() * ph.imag() + x[i].imag() * ph.real()));
        ph *= step;
        if ((i & 1023) == 0) ph /= std::abs(ph);
    }
    return y;
}

std::vector<cf> channelise(const std::vector<cf>& x, double fs, double offHz, double bwHz, double& fsOut) {
    fsOut = fs;
    std::vector<cf> m = mix(x, fs, -offHz);
    if (bwHz <= 0 || bwHz * 2 >= fs) return m;
    const int dec = std::max(1, (int)std::floor(fs / (4 * bwHz)));
    // windowed-sinc low-pass, cut-off a little above half the bandwidth
    const double fc = std::min(0.45, 0.6 * bwHz / fs);
    int taps = std::clamp(8 * dec + 1, 31, 1023);
    if ((taps & 1) == 0) taps++;
    std::vector<float> h(taps);
    double hs = 0;
    const int c = taps / 2;
    for (int i = 0; i < taps; i++) {
        const double t = i - c;
        const double s = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[i] = (float)(s * w);
        hs += h[i];
    }
    for (auto& v : h) v = (float)(v / hs);
    std::vector<cf> y;
    y.reserve(m.size() / dec + 1);
    for (size_t n = 0; n < m.size(); n += dec) {
        cf acc(0, 0);
        for (int k = 0; k < taps; k++) {
            const long j = (long)n + k - c;
            if (j >= 0 && j < (long)m.size()) acc += m[j] * h[k];
        }
        y.push_back(acc);
    }
    fsOut = fs / dec;
    return y;
}

} // namespace dect2::signallab
