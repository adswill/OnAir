// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mixed-radix complex FFT for lengths whose prime factors are 2, 3, 5 and 7 (DTMB: 3780 = 2^2 3^3 5 7). Stockham self-sorting passes on split
// (planar) arrays, so that the inner loops run over contiguous floats and vectorise.
#pragma once
#include "dect2/common/ring.h"
#include <vector>

namespace dect2::dtmb {

class MixedFft {
public:
    explicit MixedFft(int n);               // throws std::invalid_argument for other lengths
    int size() const { return n_; }
    // In place, unnormalised: forward uses exp(-j...), inverse exp(+j...) (the plain sum, like Fft in fftutil.h)
    void forward(cf32* x) { run(x, false); }
    void inverse(cf32* x) { run(x, true); }

private:
    struct Level { int r, n, m, s; std::vector<float> twr, twi; };   // twiddles exp(-j 2 pi p k / n) at [(k - 1) * m + p]
    void run(cf32* x, bool inv);
    void runBody(cf32* x, bool inv);        // compiled twice on x86-64: baseline and AVX2 (see dect2/dsp/simd.h)
    void runAvx2(cf32* x, bool inv);
    int n_;
    std::vector<Level> levels_;             // in the order of computation
    std::vector<float> ar_, ai_, br_, bi_;
};

} // namespace dect2::dtmb
