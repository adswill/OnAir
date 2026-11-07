// IEEE 802.15.4 sub-GHz BPSK PHY (868 MHz channel 0: 300 kchip/s, 20 kbit/s; 902 - 928 MHz channels 1 - 10: 600 kchip/s, 40 kbit/s).
// Every PSDU bit is differentially encoded (R_n = E_n xor R_{n-1}) and spread by the 15-chip m-sequence 111101011001000 (a 1 sends the
// inverse); the chips are BPSK with a raised-cosine pulse (roll-off 1). PPDU: 32 zero bits of preamble, SFD 0xA7 (sent LSB first: 1 1 1 0 0 1 0 1),
// PHR (7-bit length), PSDU with the CRC-16 FCS, every octet LSB first.
// The demodulator takes 4 samples per chip (complex, already filtered). A carrier offset of tens of kHz (crystals of +-40 ppm at 868 / 915 MHz)
// is far above the bit rate, so detection uses the chip-to-chip phase change (x[k] x*[k-1] against the known chip-product pattern), which only
// rotates by the offset over one chip, and the offset estimated from the preamble is removed before the bits are despread and compared with the
// bit before (differential detection).
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {

class Bpsk154Demod {
public:
    struct Result { std::vector<uint8_t> psdu; float rssiDb; double cfoHz; double snrDb; double endSample; };
    void init(double chipRateHz);
    void reset();
    void push(std::complex<float> y);                 // one sample at 4 per chip
    std::function<void(const Result&)> onFrame;       // a frame whose SFD and PHR were read (the FCS is the caller's to check)
    uint64_t preambles = 0;
private:
    static constexpr int kSps = 4, kChips = 15, kBit = kSps * kChips;
    struct Cand { int64_t at; };   // sample index of the window start of the last symbol seen
    std::complex<float> at(int64_t i) const;
    bool qAt(int64_t s, std::complex<float>& q, float& e) const;
    int decode(int64_t s);   // 0 finished (a frame or not one), 1 more samples are needed
    double chipRate_ = 300e3;
    std::vector<std::complex<float>> buf_;
    int64_t base_ = 0, g_ = -1;
    int run_[kBit] = {};
    int64_t skip_ = 0;
    std::vector<Cand> cands_;
    int64_t decodedUntil_ = 0, needAt_ = 0;
};

} // namespace dect2
