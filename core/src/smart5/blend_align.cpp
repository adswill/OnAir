// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio blend alignment meter (see blend_align.h).
#include "dect2/smart5/blend_align.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

std::string BlendReport::summary() const {
    if (!valid) return "";
    char b[160];
    snprintf(b, sizeof b, "blend: digital %+.1f ms against analogue, level %+.1f dB%s%s", offsetMs, levelDb, misaligned ? " - misaligned, the blend will echo or skip" : "", levelMismatch ? " - level mismatch" : "");
    return b;
}

void BlendAlign::configure(double audioRate, BlendParams p) {
    p_ = p;
    D_ = std::max(1, (int)std::lround(audioRate / p.workRate));
    rate_ = audioRate / D_;
    need_ = (size_t)(p.windowSec * rate_);
    a_.clear(); d_.clear(); accA_ = accD_ = 0; nA_ = nD_ = 0; r_ = {}; lags_.clear();
}

void BlendAlign::feed(const float* x, size_t n, std::deque<float>& q, double& acc, int& k) {
    for (size_t i = 0; i < n; i++) {
        acc += x[i];
        if (++k < D_) continue;
        q.push_back((float)(acc / D_)); acc = 0; k = 0;
        if (q.size() > 2 * need_) q.pop_front();
    }
}

bool BlendAlign::analyse() {
    if (!need_ || a_.size() < need_ || d_.size() < need_) return false;
    const size_t N = need_;
    int L = 1;
    while (L < (int)(2 * N)) L <<= 1;
    std::vector<cf32> A(L), B(L);
    double ea = 0, eb = 0, ma = 0, mb = 0;
    for (size_t i = 0; i < N; i++) { ma += a_[a_.size() - N + i]; mb += d_[d_.size() - N + i]; }
    ma /= N; mb /= N;
    for (size_t i = 0; i < N; i++) {
        const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (N - 1));
        const double av = a_[a_.size() - N + i] - ma, bv = d_[d_.size() - N + i] - mb;
        ea += av * av; eb += bv * bv;
        A[i] = cf32((float)(av * w), 0); B[i] = cf32((float)(bv * w), 0);
    }
    a_.erase(a_.begin(), a_.begin() + N / 3);   // the next window overlaps by two thirds
    d_.erase(d_.begin(), d_.begin() + N / 3);
    if (ea <= 0 || eb <= 0) return false;
    Fft f(L);
    f.forward(A.data()); f.forward(B.data());
    for (int k = 0; k < L; k++) {   // PHAT: keep the phase of the cross spectrum only
        std::complex<float> c = B[k] * std::conj(A[k]);
        const float m = std::abs(c);
        A[k] = m > 1e-20f ? c / m : cf32(0, 0);
    }
    f.inverse(A.data());
    const int maxLag = std::min((int)(p_.maxLagSec * rate_), (int)N - 1);
    int best = 0; double bv = -1e30, sum = 0, sum2 = 0; int cnt = 0;
    for (int l = -maxLag; l <= maxLag; l++) {
        const double v = A[(l + L) % L].real();
        sum += v; sum2 += v * v; cnt++;
        if (v > bv) { bv = v; best = l; }
    }
    const double mean = sum / cnt, sd = std::sqrt(std::max(1e-30, sum2 / cnt - mean * mean));
    const double y0 = A[(best - 1 + L) % L].real(), y2 = A[(best + 1 + L) % L].real(), den = y0 - 2 * bv + y2;
    const double frac = den < 0 ? 0.5 * (y0 - y2) / den : 0;
    const double lagMs = (best + frac) / rate_ * 1000.0;
    const double z = (bv - mean) / sd;   // a clean peak is tens of standard deviations high
    const double conf = std::clamp((z - 5) / 25, 0.0, 1.0);
    r_.levelDb = 10 * std::log10(eb / ea);
    r_.confidence = conf;
    if (conf >= p_.minConfidence) {
        lags_.push_back(lagMs);
        if (lags_.size() > 5) lags_.erase(lags_.begin());
        std::vector<double> s = lags_;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        r_.offsetMs = s[s.size() / 2];
        r_.valid = true;
        r_.misaligned = std::fabs(r_.offsetMs) > p_.tolMs;
        r_.levelMismatch = std::fabs(r_.levelDb) > p_.levelTolDb;
    }
    return true;
}

} // namespace smart5
} // namespace dect2
