// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WWVB phase modulated time code (NIST "Enhanced WWVB Broadcast Format", 2013): 1 bit a second BPSK on the 60 kHz carrier, frames of 60 bits starting with a 13-bit sync word, a 26-bit minute counter since 2000
// with Hamming(31,26) parity. Works without the amplitude code. The second boundary and the 180 degree ambiguity are found from the signal.
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2 {

struct WwvbPhaseStatus {
    bool locked = false;        // a frame with a good code word was read
    int64_t utcMinute = 0;      // of the last good frame: seconds since 1970 (a whole minute)
    int dstOn = -1, leap = -1;  // dst_on[1:0], leap_sec[1:0] (table 4); -1 not a valid code word
    int corrected = 0;          // bits corrected in the last code word (0 or 1)
    uint64_t framesOk = 0, framesBad = 0;
};

class WwvbPhase {
public:
    WwvbPhase();
    void feed(std::complex<float> z8k);
    const WwvbPhaseStatus& status() const { return st_; }
    void reset();
    // tests and the signal generator: the 60 bits of a time frame
    static std::array<uint8_t, 60> frameBits(int64_t utcMinute);
private:
    void newSecond();
    void searchAlignment();
    void tryFrame();
    std::vector<std::complex<float>> buf_;
    uint64_t n_ = 0;
    int align_ = -1, sinceSearch_ = 0;
    std::vector<int> bits_;
    std::vector<std::complex<float>> z_;
    WwvbPhaseStatus st_;
    double fPerSec_ = 0;
    uint32_t lastT_ = 0; bool haveLast_ = false;
};

}  // namespace dect2
