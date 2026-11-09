// DCF77 phase code: the carrier is phase keyed +-15.6 degrees by a 512-chip pseudo-random sequence (chip 120 carrier cycles, 1.548 ms) from 200 ms into each second; the
// sequence is inverted for a time-code 1 (PTB; the code is the same BCD time as the AM keying). This reads each second by correlation, and finds the second by itself.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2 {

struct Dcf77PhaseStatus {
    bool locked = false;
    int secondOffset = -1;      // the sample (0..7999) of the 8 kHz stream where the chip sequence starts, -1 until locked
    float corr = 0;             // normalised correlation of the last second, 0..1
    uint64_t seconds = 0;       // seconds read
    int lastBit = -1;           // -1 none
};

class Dcf77Phase {
public:
    Dcf77Phase();
    void feed(std::complex<float> z8k);           // one sample at 8 kHz, carrier at 0 Hz
    const Dcf77PhaseStatus& status() const { return st_; }
    const std::vector<int8_t>& pn() const { return pn_; }
    // The last 60 seconds read in order, oldest first (bit 0 / 1, -1 not read); the receiver lines these up with the minute marker of the AM side.
    std::array<int, 60> lastBits() const { return bits_; }
    void reset();
private:
    void second();
    std::vector<int8_t> pn_;
    std::vector<std::complex<float>> buf_;
    size_t n_ = 0;
    double corrPrev_ = 0;
    Dcf77PhaseStatus st_;
    std::array<int, 60> bits_;
    int lockOff_ = -1, seen_ = 0;
};

}  // namespace dect2
