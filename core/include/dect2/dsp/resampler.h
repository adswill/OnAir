// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Rational polyphase resampler (windowed sinc) for complex samples, used to bring any capture rate to the T2 native rate.
#pragma once
#include "dect2/common/ring.h"
#include <vector>

namespace dect2 {

class RationalResampler {
public:
    // Finds a small L/M approximating outRate/inRate. Returns false if no good ratio exists.
    // occupiedHz > 0 (decimating): the signal is that wide, so the filter is sized for it, not for the whole output band (fewer taps).
    bool configure(double inRate, double outRate, double* relError = nullptr, double occupiedHz = 0);
    bool passthrough() const { return L_ == 1 && M_ == 1; }
    int L() const { return L_; }
    int M() const { return M_; }
    void reset();
    // Appends resampled samples to `out`.
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    // false: the split path even where the interleaved one could run (tests: both must give the same bits with the portable desamp)
    void allowInterleaved(bool on) { allowInter_ = on; inter_ = on && interOk_; }
    // x86-64 with AVX2: the interleaved path as well (call before configure()); its sums are in another order than desamp's, so the default
    // keeps the split path and the exact output the other modes have always had
    void allowFastX86(bool on) { fastX86_ = on; }

private:
    bool allowInter_ = true, interOk_ = false;
    void processSplit(const cf32* in, size_t n, std::vector<cf32>& out);
    void processInterleaved(const cf32* in, size_t n, std::vector<cf32>& out);
    int L_ = 1, M_ = 1, H_ = 24, T_ = 48;
    std::vector<std::vector<float>> taps_; // per output phase (0..L-1), length T_
    std::vector<int> base_;                // input offset of the first output of each phase
    std::vector<int> nOfPhase_;            // n_p
    std::vector<float> re_, im_;           // pending input (split), includes history; the first head_ values are consumed
    size_t head_ = 0;
    std::vector<float> tr_, ti_;           // one phase's outputs (scratch kept between calls: no allocation per block)
    // The interleaved path (ARM NEON): the samples stay complex, every output is one pass over its taps with each tap duplicated for I
    // and Q, written in output order. The sums are added in the same order as desamp() (Accelerate's on macOS, the portable one
    // elsewhere), so the output has the same bits as the split path; it saves the split copy, the second pass and the scattered writes.
    bool inter_ = false;
    std::vector<cf32> buf_;                // pending input (interleaved), with history; the first head_ samples are consumed
    std::vector<float> tapsD_;             // per output index within a period (0..L-1): T_ taps, each twice (I, Q)
    std::vector<int> startOf_;             // per output index within a period: its first input sample, relative to the period's start
    bool fastX86_ = false;   // (was an unused flag: same size, so objects built with the old header stay compatible)
};

} // namespace dect2
