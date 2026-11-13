// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-SH OFDM (SH-A) signal generator: MPEG transport stream in, baseband IQ at the native OFDM rate (dvbsh::sampleRate) out.
// The full EN 302 583 chain: EFRAMEs, turbo code, bit-wise interleaver, SH frames with padding, the time interleaver, bit demultiplexing,
// symbol interleaver, QPSK / 16-QAM mapping (hierarchical with an LP stream), pilots and the DVB-SH TPS.
#pragma once
#include "dvbsh.h"
#include <deque>
#include <memory>

namespace dect2 {
class Fft;
namespace dvbsh {

class Generator {
public:
    using PacketSource = std::function<void(uint8_t* pkt188)>;
    explicit Generator(const Params& p, PacketSource hp = nullptr, PacketSource lp = nullptr);
    ~Generator();
    const Params& params() const { return p_; }
    double sampleRate() const { return dvbsh::sampleRate(p_.bwMhz); }
    int symbolLength() const { return N_ + G_; }
    // One OFDM symbol with its guard interval (N + G samples, unit mean power)
    void nextSymbol(std::vector<cf32>& out);
    int symbolIndex() const { return sym_; }
    int frameCount() const { return frames_; }   // OFDM frames sent so far (frame 0 starts a SH frame)
    long codewordsSent(bool lp = false) const { return lp ? lp_.codewords : hp_.codewords; }

private:
    struct Chain {
        int pat = 4;
        PacketSource src;
        TimeInterleaver<uint8_t> ti;
        std::deque<uint8_t> bits;   // interleaved stream
        long codewords = 0;
        void fillShFrame();
    };
    Params p_;
    int N_, G_, K_, sym_ = 0, frames_ = 0;
    Chain hp_, lp_;
    std::vector<float> tpsVal_;
    std::unique_ptr<Fft> fft_;
};

} // namespace dvbsh
} // namespace dect2
