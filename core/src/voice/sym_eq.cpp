// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/voice/sym_eq.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace dect2::symeq {

namespace {
std::atomic<bool> gOn{false};   // off by default: the real-IQ echo test (tests/dmr/test_dmr_equalizer.cpp) shows no net gain through the FM front end; Advanced turns it on
constexpr int kC = kTaps / 2;

inline float slice(float y) { return y >= 2.f ? 3.f : y >= 0.f ? 1.f : y >= -2.f ? -1.f : -3.f; }
inline float at(const float* z, int n, int i) { return z[i < 0 ? 0 : i >= n ? n - 1 : i]; }   // the edge sample repeats outside the burst
}

bool enabled() { return gOn.load(); }
void setEnabled(bool on) { gOn.store(on); }

float eyeError(const float* z, int n) {
    if (n <= 0) return 0.f;
    double e = 0;
    for (int i = 0; i < n; i++) { const float d = z[i] - slice(z[i]); e += (double)d * d; }
    return (float)(e / n);
}

void SymEq::reset() {
    memset(w_, 0, sizeof(w_));
    w_[kC] = 1.f;
    active_ = false;
}

bool SymEq::train(const float* z, int n, int atPos, const float* ref, int m, float ridge) {
    if (m < kTaps + 4 || atPos < 0 || atPos + m > n) return false;
    // normal equations R w = p + ridge * e_centre, R = X'X + ridge I
    double R[kTaps][kTaps] = {}, p[kTaps] = {};
    for (int k = 0; k < m; k++) {
        float x[kTaps];
        for (int i = 0; i < kTaps; i++) x[i] = at(z, n, atPos + k + i - kC);
        for (int i = 0; i < kTaps; i++) {
            p[i] += (double)x[i] * ref[k];
            for (int j = 0; j < kTaps; j++) R[i][j] += (double)x[i] * x[j];
        }
    }
    for (int i = 0; i < kTaps; i++) { R[i][i] += ridge; p[i] += i == kC ? ridge : 0.0; }
    // Gaussian elimination with partial pivoting
    double a[kTaps][kTaps + 1];
    for (int i = 0; i < kTaps; i++) { for (int j = 0; j < kTaps; j++) a[i][j] = R[i][j]; a[i][kTaps] = p[i]; }
    for (int c = 0; c < kTaps; c++) {
        int piv = c;
        for (int r = c + 1; r < kTaps; r++) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-9) return false;
        if (piv != c) for (int j = 0; j <= kTaps; j++) std::swap(a[c][j], a[piv][j]);
        for (int r = c + 1; r < kTaps; r++) {
            const double f = a[r][c] / a[c][c];
            for (int j = c; j <= kTaps; j++) a[r][j] -= f * a[c][j];
        }
    }
    double w[kTaps];
    for (int i = kTaps - 1; i >= 0; i--) {
        double s = a[i][kTaps];
        for (int j = i + 1; j < kTaps; j++) s -= a[i][j] * w[j];
        w[i] = s / a[i][i];
    }
    for (int i = 0; i < kTaps; i++) { if (!std::isfinite(w[i])) return false; }
    for (int i = 0; i < kTaps; i++) w_[i] = (float)w[i];
    return true;
}

void SymEq::apply(const float* z, int n, float* y) const {
    for (int k = 0; k < n; k++) {
        float s = 0;
        for (int i = 0; i < kTaps; i++) s += w_[i] * at(z, n, k + i - kC);
        y[k] = s;
    }
}

bool SymEq::run(const float* z, int n, int atPos, const float* ref, int m, float* y, float* eyeBefore, float* eyeAfter) {
    const float e0 = eyeError(z, n);
    if (eyeBefore) *eyeBefore = e0;
    memcpy(y, z, (size_t)n * sizeof(float));
    if (eyeAfter) *eyeAfter = e0;
    if (!enabled() || n < kTaps + 4) return false;
    const bool haveRef = ref && m > 0 && atPos >= 0 && atPos + m <= n;
    if (!haveRef) {
        // no known symbols in this burst: the taps learned on the last sync, frozen, and only if they visibly open the eye
        if (!active_ || kEyeFrozen <= 0.f) return false;
        apply(z, n, y);
        const float e1 = eyeError(y, n);
        if (eyeAfter) *eyeAfter = e1;
        if (e1 < kEyeFrozen * e0) return true;
        memcpy(y, z, (size_t)n * sizeof(float));
        if (eyeAfter) *eyeAfter = e0;
        return false;
    }
    // a burst with a sync word: the truth is known, so the fit can be checked honestly on it
    double s0 = 0;
    for (int k = 0; k < m; k++) { const double a = z[atPos + k] - ref[k]; s0 += a * a; }
    if (!active_ && e0 < kEyeOn && s0 / m < kSyncOn) return false;   // an open eye: leave the slicer alone
    SymEq cand;
    if (!cand.train(z, n, atPos, ref, m)) return false;
    // decision-directed least squares over the burst: the first fit (24 outer-level symbols) opens the eye enough for the decisions to be mostly
    // right, and 132 symbols of all four levels fit the taps much better than the sync alone; the known symbols stay the truth
    float* t = y;
    for (int it = 0; it < 3; it++) {
        cand.apply(z, n, t);
        float dref[512];
        const int nn = std::min(n, 512);
        for (int k = 0; k < nn; k++) dref[k] = slice(t[k]);
        for (int k = 0; k < m && atPos + k < nn; k++) dref[atPos + k] = ref[k];
        cand.train(z, nn, 0, dref, nn, 1.f);
    }
    cand.apply(z, n, t);
    const float e1 = eyeError(t, n);
    if (eyeAfter) *eyeAfter = e1;
    double s1 = 0;
    for (int k = 0; k < m; k++) { const double b = t[atPos + k] - ref[k]; s1 += b * b; }
    if (s1 < kSyncGain * s0 && e1 < kEyeGain * e0) {
        *this = cand;
        active_ = true;
        return true;
    }
    memcpy(y, z, (size_t)n * sizeof(float));
    if (eyeAfter) *eyeAfter = e0;
    if (active_ && e0 < 0.5f * kEyeOn) active_ = false;   // the channel became clean: back to the plain slicer
    return false;
}

} // namespace dect2::symeq
