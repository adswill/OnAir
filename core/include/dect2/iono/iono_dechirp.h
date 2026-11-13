// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The dechirp inner loop of the ionospheric sounder: accumulate x[k] * z[k] where z is a chirp phasor advanced by a chirp-rate-modulated step
// (z <- z w, w <- w dw). Run as four chains that start at exactly computed phases a quarter of the run apart, so the four recurrences overlap in the CPU
// instead of each waiting on the previous multiply; every chain starts from a fresh sin / cos, which also keeps the phase error lower than one long
// recurrence. The sum differs from the one-chain loop only in the last bits of a double.
#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include "dect2/common/mathconst.h"

namespace dect2::iono {

using cd = std::complex<double>;

// One sample at a time: the reference.
inline void dechirpScalar(const std::complex<float>* x, size_t n, cd& z, cd& w, cd dw, cd& acc) {
    for (size_t k = 0; k < n; k++) {
        acc += cd(x[k].real(), x[k].imag()) * z;
        z *= w; w *= dw;
    }
}

// n samples: acc += sum x[k] z_k; z and w move on by n samples. beta: the phase of dw (radians per sample).
inline void dechirpRun(const std::complex<float>* x, size_t n, cd& z, cd& w, cd dw, double beta, cd& acc) {
    if (n < 64) { dechirpScalar(x, n, z, w, dw, acc); return; }
    constexpr int C = 4;
    const size_t L = n / C;
    const double alpha = std::atan2(w.imag(), w.real());
    double zr[C], zi[C], wr[C], wi[C], ar[C] = {}, ai[C] = {};
    for (int c = 0; c < C; c++) {
        const double s = (double)((size_t)c * L);
        const double pz = alpha * s + 0.5 * beta * s * (s - 1);   // z advances by w_0 .. w_{s-1}: the phases add up to s alpha + beta s (s - 1) / 2
        const double pw = alpha + beta * s;                         // and w by dw^s
        const double mw = std::abs(w);
        const cd zc = z * cd(std::cos(pz), std::sin(pz));
        zr[c] = zc.real(); zi[c] = zc.imag();
        wr[c] = mw * std::cos(pw); wi[c] = mw * std::sin(pw);
    }
    const double dr = dw.real(), di = dw.imag();
    for (size_t t = 0; t < L; t++) {
        for (int c = 0; c < C; c++) {
            const std::complex<float> v = x[(size_t)c * L + t];
            const double xr = v.real(), xi = v.imag();
            ar[c] += xr * zr[c] - xi * zi[c];
            ai[c] += xr * zi[c] + xi * zr[c];
            const double nzr = zr[c] * wr[c] - zi[c] * wi[c], nzi = zr[c] * wi[c] + zi[c] * wr[c];
            zr[c] = nzr; zi[c] = nzi;
            const double nwr = wr[c] * dr - wi[c] * di, nwi = wr[c] * di + wi[c] * dr;
            wr[c] = nwr; wi[c] = nwi;
        }
    }
    for (int c = 0; c < C; c++) acc += cd(ar[c], ai[c]);
    z = cd(zr[C - 1], zi[C - 1]);
    w = cd(wr[C - 1], wi[C - 1]);
    const size_t done = (size_t)C * L;
    dechirpScalar(x + done, n - done, z, w, dw, acc);
}

} // namespace dect2::iono
