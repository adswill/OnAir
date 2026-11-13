// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fm/fm_tone.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace {

constexpr double kPi = 3.14159265358979323846;

// the RBJ cookbook shelf with slope 1 (A = 10^(dB/40)); a zero gain is the identity
FmShelf shelf(double gainDb, double fc, double fs, bool high) {
    if (gainDb == 0) return FmShelf{};
    const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * fc / fs;
    const double cw = std::cos(w), alpha = std::sin(w) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
    }
    return FmShelf{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// one sample through a shelf, transposed direct form II
inline double run(const FmShelf& c, double x, double (&s)[2]) {
    const double y = c.b0 * x + s[0];
    s[0] = c.b1 * x - c.a1 * y + s[1];
    s[1] = c.b2 * x - c.a2 * y;
    return y;
}

double clampDb(int v) { return std::max(-kToneMaxDb, std::min(kToneMaxDb, (double)v)); }

} // namespace

FmShelf fmLowShelf(double gainDb, double fc, double fs) { return shelf(gainDb, fc, fs, false); }
FmShelf fmHighShelf(double gainDb, double fc, double fs) { return shelf(gainDb, fc, fs, true); }

double fmShelfDb(const FmShelf& s, double f, double fs) {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * f / fs), z2 = z1 * z1;
    const std::complex<double> num = s.b0 + s.b1 * z1 + s.b2 * z2, den = 1.0 + s.a1 * z1 + s.a2 * z2;
    return 20 * std::log10(std::abs(num) / std::abs(den));
}

double fmTrebleGainDb(int trebleDb, bool warm) { return clampDb(trebleDb) - (warm ? kWarmCutDb : 0); }

void FmTone::configure(int bassDb, int trebleDb, bool warm, double fs) {
    const double bass = clampDb(bassDb);
    on_ = bass != 0 || clampDb(trebleDb) != 0 || warm;
    bass_ = fmLowShelf(bass, kToneBassHz, fs);
    treble_ = fmHighShelf(fmTrebleGainDb(trebleDb, warm), kToneTrebleHz, fs);
}

void FmTone::reset() {
    for (int c = 0; c < 2; c++) sb_[c][0] = sb_[c][1] = st_[c][0] = st_[c][1] = 0;
}

void FmTone::process(float* left, float* right, size_t n) {
    if (!on_) return;
    for (size_t i = 0; i < n; i++) {
        left[i] = (float)run(treble_, run(bass_, left[i], sb_[0]), st_[0]);
        right[i] = (float)run(treble_, run(bass_, right[i], sb_[1]), st_[1]);
    }
}

} // namespace dect2
