// Mixed radix complex FFT for the DRM OFDM symbol sizes (1152, 1024, 704, 448 samples at 48 kHz, 216 at 96 kHz ...): any size whose prime factors are small.
#pragma once
#include "ring.h"
#include <vector>

namespace dect2 { namespace drm {

class DrmFft {
public:
    explicit DrmFft(int n = 1);
    int size() const { return n_; }
    void forward(cf32* x) const { run(x, false); }     // X[k] = sum x[n] exp(-j 2 pi n k / N)
    void inverse(cf32* x) const { run(x, true); }      // x[n] = sum X[k] exp(+j 2 pi n k / N), not divided by N

private:
    void run(cf32* x, bool inv) const;
    void work(cf32* out, const cf32* in, size_t fstride, const int* factors, bool inv) const;
    int n_ = 1;
    std::vector<int> factors_;      // pairs: radix p, remaining size m
    std::vector<cf32> tw_;          // exp(-j 2 pi i / N)
    mutable std::vector<cf32> scratch_, tmp_;
};

}} // namespace dect2::drm
