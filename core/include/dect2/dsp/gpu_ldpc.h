// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPU LDPC decoder (Metal on macOS, Direct3D 11 on Windows, Vulkan on Linux): layered normalised min-sum, one threadgroup (360 threads, one per
// check of a layer) per FEC block.
#pragma once
#include "ldpc.h"
#include "qc_ldpc.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class GpuLdpc {
public:
    static GpuLdpc& instance();
    bool available() const;
    const char* deviceName() const;
#ifdef _WIN32
    size_t dedicatedMemoryMb() const;   // video memory of the chosen adapter that belongs to it (about 128 for integrated graphics)
#endif
    // Decodes `nb` blocks of n LLRs each (llr > 0 means bit 0). hard receives nb*n bits (one per byte),
    // ok[b] = all parity checks satisfied, iters[b] = iterations used. Blocks the caller until the GPU is done. `boxCorr`: the two-minimum
    // correction of LdpcCode::decodeFast (Metal and Direct3D 11; the Vulkan kernel runs plain normalised min-sum, and the CPU retry of the frames
    // it fails gives the same answer).
    bool decode(const LdpcCode& code, const float* llr, int nb, int maxIter, uint8_t* hard, uint8_t* ok, int* iters, float alpha = 0.78f, bool boxCorr = false);

    // The same for a table-driven quasi-cyclic code (layered normalised min-sum, the schedule and arithmetic of the DTMB CPU decoder, one thread group per
    // word). llr: nb * sentBits() values (> 0 means bit 0). info: nb * infoBits() bits (one per byte). ok[b]: all checks satisfied; iters[b]: iterations.
    // Returns false when the backend has no such kernel (Direct3D 11 for now) or the call failed: the caller decodes on the CPU.
    bool qcAvailable() const;   // decodeQc has a kernel on this backend and device (Metal, Vulkan)
    bool decodeQc(const QcLdpc& code, const float* llr, int nb, int maxIter, uint8_t* info, uint8_t* ok, int* iters, float alpha, bool boxCorr = false);
    // the kernels of this backend apply `boxCorr` (Metal, Direct3D 11); where they do not (Vulkan), the caller decodes the words they fail on the CPU
    bool boxCorrKernels() const;

private:
    bool selfTest();   // (Direct3D 11 and Vulkan builds) decodes known blocks; the GPU decoder disables itself if the result is wrong
    GpuLdpc();
    ~GpuLdpc();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
