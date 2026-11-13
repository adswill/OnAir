// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
// Spectrum analyser resolution: the FFT size that gets closest to the bin width the user asked for, and the bin width that really results.
// An FFT of N points at sample rate R has bins of R / N, and N is a power of two, so only some widths exist. The HackRF's own sweep takes
// 20 Msps blocks of at most 4096 points (finest 4.88 kHz); a stepped sweep or a file can use up to 65536 points.
#include <cmath>
#include <vector>

namespace dect2 {

struct SaRes { int nfft = 64; double binHz = 0; };

inline double saRateOf(bool native, double rateHz) { return native ? 20e6 : rateHz; }
inline int saMaxFft(bool native) { return native ? 4096 : 65536; }

// the power-of-two size the receiver uses for a wanted bin width: the smallest FFT whose bins are no wider than asked (1 % slack), or the
// largest one allowed when even that is too wide (the HackRF sweep: 4.88 kHz is the finest, whatever is asked below it)
inline SaRes saPickRes(double wantHz, double rateHz, bool native) {
    SaRes o;
    const double r = saRateOf(native, rateHz);
    if (r <= 0 || wantHz <= 0) return o;
    const int maxN = saMaxFft(native);
    int n = 64;
    while (n < maxN && r / n > wantHz * 1.01) n *= 2;
    o.nfft = n;
    o.binHz = r / n;
    return o;
}

// every bin width the current rate allows, coarsest first (what the resolution box offers; the UI cuts it at 1 MHz)
inline std::vector<double> saBinChoices(double rateHz, bool native) {
    std::vector<double> v;
    const double r = saRateOf(native, rateHz);
    if (r <= 0) return v;
    for (int k = 64; k <= saMaxFft(native); k *= 2) v.push_back(r / k);
    return v;
}

} // namespace dect2
