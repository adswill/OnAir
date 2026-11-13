// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/t2/t2ofdm.h"
#include "dect2/common/window.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <vector>

namespace dect2 {

static double bessel0(double x) { return dect2::window::besselI0(x, 40, 0.0); }

void GridInterpolator::build(int S, double cutoff) {
    S_ = S;
    cut_ = cutoff;
    const int T = 2 * kHalf;
    taps_.assign((size_t)S * T, 0.f);
    const double beta = 5.0;
    for (int p = 0; p < S; p++) {
        double frac = (double)p / S, sum = 0;
        std::vector<double> h(T);
        for (int t = 0; t < T; t++) {
            double x = frac - (t - (kHalf - 1));
            double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double r = x / kHalf;
            double w = std::fabs(r) >= 1 ? 0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
            h[t] = sinc * w;
            sum += h[t];
        }
        // vDSP_conv computes a correlation, which is what the polyphase sum needs
        for (int t = 0; t < T; t++) taps_[(size_t)p * T + t] = (float)(h[t] / sum);
    }
    // a phase whose kernel is a single 1 (the output point sits on a grid point) needs no filtering
    delta_.assign(S, 0);
    for (int p = 0; p < S; p++) {
        bool d = std::fabs(taps_[(size_t)p * T + (kHalf - 1)] - 1.f) < 1e-5f;
        for (int t = 0; t < T && d; t++) if (t != kHalf - 1 && std::fabs(taps_[(size_t)p * T + t]) > 1e-5f) d = false;
        delta_[p] = d;
    }
    // grid smoothing: windowed-sinc lowpass at `cutoff` of the grid Nyquist
    smoothHalf_ = cutoff >= 0.9 ? 0 : std::min(48, std::max(6, (int)std::ceil(5.0 / cutoff)));
    smooth_.clear();
    if (smoothHalf_) {
        int L = 2 * smoothHalf_ + 1;
        smooth_.resize(L);
        double sum = 0;
        std::vector<double> h(L);
        for (int i = 0; i < L; i++) {
            double x = i - smoothHalf_;
            double a = cutoff * x;
            double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            double r = x / (smoothHalf_ + 1);
            h[i] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
            sum += h[i];
        }
        for (int i = 0; i < L; i++) smooth_[L - 1 - i] = (float)(h[i] / sum);
    }
}

// w_n = e^{j step n} by the double-precision recurrence (renormalised every 256 steps), as floats; rebuilt only when step or count change
void GridInterpolator::phasors(double step, int count, double& cachedStep, std::vector<cf32>& w) {
    if (step == cachedStep && (int)w.size() == count) return;
    cachedStep = step;
    w.resize((size_t)count);
    std::complex<double> z(1, 0), zs(std::cos(step), std::sin(step));
    for (int n = 0; n < count; n++) {
        w[(size_t)n] = cf32((float)z.real(), (float)z.imag());
        z *= zs;
        if ((n & 255) == 255) z /= std::abs(z);
    }
}

void GridInterpolator::run(const std::vector<cf32>& grid, int S, int K, int N, double tau0, std::vector<cf32>& H, double cutoff) {
    cutoff = std::min(1.0, std::max(0.05, cutoff));
    if (S != S_ || std::fabs(cutoff - cut_) > 1e-9) build(S, cutoff);
    const int M = (int)grid.size();
    const int T = 2 * kHalf;
    const int pad = smoothHalf_ + kHalf + 2;
    const int Mp = M + 2 * pad;
    pre_.assign(Mp, 0.f); pim_.assign(Mp, 0.f);
    // pre-rotation (delay centring) with a double-precision phasor recurrence. The phasors depend only on the step and the count, which stay the
    // same from symbol to symbol while the timing does: they are kept (the same float values the recurrence gave) and only rebuilt on a change.
    {
        const double step = 2 * M_PI * (double)S * tau0 / N;
        phasors(step, M, preStep_, preW_);
        const cf32* pw = preW_.data();
        for (int n = 0; n < M; n++) {
            cf32 v = grid[n] * pw[n];
            pre_[pad + n] = v.real(); pim_[pad + n] = v.imag();
        }
    }
    // odd reflection about both edges keeps linear trends intact
    for (int i = 1; i <= pad; i++) {
        int a = std::min(M - 1, i), b = std::max(0, M - 1 - i);
        pre_[pad - i] = 2 * pre_[pad] - pre_[pad + a];
        pim_[pad - i] = 2 * pim_[pad] - pim_[pad + a];
        pre_[pad + M - 1 + i] = 2 * pre_[pad + M - 1] - pre_[pad + b];
        pim_[pad + M - 1 + i] = 2 * pim_[pad + M - 1] - pim_[pad + b];
    }
    const float* gr = pre_.data();
    const float* gi = pim_.data();
    // stage 1: delay-domain denoising as a lowpass along the grid
    if (smoothHalf_) {
        const int L = 2 * smoothHalf_ + 1;
        const int outN = M + 2 * (kHalf + 2);
        sre_.assign(outN, 0.f); sim_.assign(outN, 0.f);
        const int start = pad - (kHalf + 2) - smoothHalf_;
        convCorr(pre_.data() + start, smooth_.data(), sre_.data(), 1, outN, L);
        convCorr(pim_.data() + start, smooth_.data(), sim_.data(), 1, outN, L);
        gr = sre_.data() + (kHalf + 2) - pad; // so that gr[pad + n] addresses grid point n again
        gi = sim_.data() + (kHalf + 2) - pad;
        // gr[pad+n] -> sre_[(kHalf+2)+n]
    }
    // stage 2: polyphase interpolation, one phase at a time (strided output)
    ore_.assign((size_t)S * M + S, 0.f); oim_.assign((size_t)S * M + S, 0.f);
    for (int p = 0; p < S; p++) {
        if (delta_[p]) {
            for (int n = 0; n < M; n++) { ore_[(size_t)n * S + p] = gr[pad + n]; oim_[(size_t)n * S + p] = gi[pad + n]; }
            continue;
        }
        const float* h = &taps_[(size_t)p * T];
        // out[n] = sum_t h[t] * g[n + t - (kHalf - 1)]  -> input starts at g[-(kHalf-1)] = gr[pad - (kHalf-1)]
        convCorr(gr + pad - (kHalf - 1), h, ore_.data() + p, S, M, T);
        convCorr(gi + pad - (kHalf - 1), h, oim_.data() + p, S, M, T);
    }
    // post-rotation
    H.resize(K);
    {
        const double step = -2 * M_PI * tau0 / N;
        phasors(step, K, postStep_, postW_);
        const cf32* pw = postW_.data();
        for (int k = 0; k < K; k++) H[k] = cf32(ore_[k], oim_[k]) * pw[k];
    }
}

bool delaySpan(const std::vector<cf32>& H, int N, int searchLo, int searchHi, int centre, int& tMin, int& tMax) {
    int lg = (int)std::lround(std::log2((double)N));
    std::vector<float> re(N, 0.f), im(N, 0.f);
    int K = (int)H.size();
    for (int k = 0; k < K; k++) {
        int f = k - (K - 1) / 2;
        int b = (f + N) % N;
        re[b] = H[k].real();
        im[b] = H[k].imag();
    }
    fftSplit(re.data(), im.data(), lg, true);
    std::vector<float> pw(N);
    double peak = 0;
    for (int n = 0; n < N; n++) { pw[n] = re[n] * re[n] + im[n] * im[n]; peak = std::max(peak, (double)pw[n]); }
    // The noise floor from the taps of the search range only: an estimate interpolated from a pilot grid holds noise only inside its delay
    // window (N / S wide), the rest of the N taps are nearly empty. Their median, taken over all N, sat far below the noise in the window, so
    // at a low SNR every noise tap counted as a path, the span came out as the whole window and the estimate was never narrowed (DVB-T2
    // L1 at 4 dB: its cells 3 dB noisier than the channel).
    std::vector<float> tmp;
    tmp.reserve((size_t)std::max(1, searchHi - searchLo + 1));
    for (int t = searchLo; t <= searchHi; t++) tmp.push_back(pw[(size_t)((((t + centre) % N) + N) % N)]);
    if (tmp.size() < 16) tmp = pw;
    std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
    double floorP = tmp[tmp.size() / 2] * 1.5; // median of noise-only taps, approx mean
    double thr = std::max(floorP * 12.0, peak * 3e-4);
    if (peak < floorP * 30.0) return false;
    bool any = false;
    tMin = 1 << 30; tMax = -(1 << 30);
    for (int t = searchLo; t <= searchHi; t++) {
        int idx = (((t + centre) % N) + N) % N;
        if (pw[idx] > thr) { any = true; tMin = std::min(tMin, t); tMax = std::max(tMax, t); }
    }
    return any;
}

void impulseResponse(const std::vector<cf32>& H, int N, int tauMin, int tauMax, int centre, std::vector<float>& outDb) {
    int lg = (int)std::lround(std::log2((double)N));
    std::vector<float> re(N, 0.f), im(N, 0.f);
    int K = (int)H.size();
    for (int k = 0; k < K; k++) {
        int f = k - (K - 1) / 2;
        int b = (f + N) % N;
        re[b] = H[k].real();
        im[b] = H[k].imag();
    }
    fftSplit(re.data(), im.data(), lg, true);
    outDb.assign(std::max(0, tauMax - tauMin), -120.f);
    double peak = 1e-30;
    for (int n = 0; n < N; n++) peak = std::max(peak, (double)re[n] * re[n] + (double)im[n] * im[n]);
    for (int t = tauMin; t < tauMax; t++) {
        int idx = ((t + centre) % N + N) % N;
        double p = ((double)re[idx] * re[idx] + (double)im[idx] * im[idx]) / peak;
        outDb[t - tauMin] = (float)(10 * std::log10(std::max(p, 1e-12)));
    }
}

} // namespace dect2
