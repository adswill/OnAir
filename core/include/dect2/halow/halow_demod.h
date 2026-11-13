// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) OFDM demodulation pieces shared by the receive chain: the FFT of one symbol window, pilot phase and slope tracking,
// equalised tones with their noise variance, soft bits of a data symbol (any of BPSK ... 256-QAM, deinterleaved) and the decoding of a whole data
// field (depuncturing, Viterbi per BCC encoder, descrambler) down to the PSDU bytes. IEEE 802.11-2020 clause 23.3.11.
#pragma once
#include "dect2/dsp/fftutil.h"
#include "dect2/halow/halow_phy.h"
#include <vector>

namespace dect2::halow {

class Demod {
public:
    explicit Demod(int n) : N(n), fft_(n) {}
    int N;
    void fftWin(const cf32* x, cf32* Y);                         // N samples -> N bins, scaled 1 / N
    // SIG field symbol: the copies of the 2 MHz layout added with their channel weights; re / im: soft bits for BPSK and for QBPSK (the Q axis)
    bool sigSym(const cf32* x, const std::vector<Layout>& lays, int polIdx, const cf32* H, std::vector<float>& re, std::vector<float>& im, double& nsum, int& ncnt);
    // Data symbol: equalised tones (constellation units) and their noise variance per real dimension; sigma2Ltf is the noise power of one tone of Y from the training symbols
    bool dataSym(const cf32* x, const Layout& lay, int polIdx, const cf32* H, float sigma2Ltf, std::vector<cf32>& z, std::vector<float>& nv);
private:
    Fft fft_;
    std::vector<cf32> Y_;
};

// Soft coded bits of one data symbol in transmit order (demapped, deinterleaved; the two copies of a repeated symbol added)
void symbolLlr(const std::vector<cf32>& z, const std::vector<float>& nv, const WidthInfo& wi, int mcs, std::vector<float>& coded);
// All data symbols' coded bits -> PSDU bytes (with the FCS). false when the field is shorter than it should be
bool decodeDataField(const std::vector<float>& coded, int w, int mcs, int nsym, int len, std::vector<uint8_t>& psdu);

} // namespace dect2::halow
