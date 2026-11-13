// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPU FFT / power spectrum / FIR decimator (Metal on macOS; elsewhere available() is false and the CPU code is used: Vulkan and Direct3D 11
// versions are not written yet, the app says "GPU: macOS only" for the modes that depend on it). Complex float, power-of-two sizes.
// The FFT is a Stockham radix-2 autosort in device memory (log2 N passes, ping-pong buffers), the batch is the grid's second dimension.
// Results equal the CPU transform to float rounding (tests/dsp/test_gpu_fft.cpp reports the error); it is not bit-exact with the CPU code.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {

class GpuFft {
public:
    static GpuFft& instance();
    bool available() const;
    const char* deviceName() const;
    // `nb` blocks of `n` complex samples (x: nb * n), each multiplied by `window` (n floats, nullptr = none) and transformed; |X[k]|^2 of every
    // block is added into acc[k] (n floats, natural FFT order, not scaled). Blocks until the GPU is done. false: nothing done, use the CPU.
    bool powerAccumulate(const cf32* x, int nb, size_t n, const float* window, float* acc);
    // `nb` transforms of n points in place (x: nb * n interleaved complex). inverse: the unscaled inverse. false: use the CPU.
    bool transform(cf32* x, int nb, size_t n, bool inverse);
    // FIR filter + decimation of a complex stream: out[i] = sum_t taps[t] * x[i * dec + t] for i < nOut (x holds (nOut - 1) * dec + nTaps samples).
    bool firDecimate(const cf32* x, size_t nOut, const float* taps, int nTaps, int dec, cf32* out);

private:
    GpuFft();
    ~GpuFft();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
