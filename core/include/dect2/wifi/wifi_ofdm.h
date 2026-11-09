// Wi-Fi OFDM PHY for the 5 and 6 GHz beacons: IEEE 802.11-2020 clause 17 (OFDM PHY, "802.11a"), the non-HT format that every access point
// sends its beacons in on those bands (6 Mbit/s: BPSK, rate 1/2) whatever the channel width of the data it carries. A 40 / 80 / 160 MHz
// channel sends the same non-HT frame duplicated in each 20 MHz part (clause 17.3.8.2 "non-HT duplicate"), so listening to the 20 MHz primary
// is enough. Receive only; the transmit half (txPpdu) builds the test signal in memory.
//
// Everything is at 20 Msps: 64-point FFT (312.5 kHz spacing), 16-sample guard interval, 52 used carriers (48 data, 4 pilots at -21, -7, 7, 21).
//   preamble: short training (10 x 16 samples, 8 us) then long training (32-sample guard, 2 x 64 samples, 8 us), then the SIGNAL symbol
//   (RATE, LENGTH, parity, tail; BPSK 1/2) and the DATA symbols (16 SERVICE bits, the PSDU, 6 tail bits, pad; scrambled, K = 7 convolutional code
//   g0 = 133, g1 = 171 octal, interleaved, mapped to the 48 data carriers).
// OfdmRx finds the short training by its 16-sample periodicity, estimates the carrier offset (coarse from the short, fine from the long
// training), finds the long training by cross-correlation, equalises with the long training's channel estimate and the pilots' common phase
// and slope, decodes SIGNAL, then DATA by soft Viterbi, and checks the FCS (CRC-32).
#pragma once
#include "dect2/common/ring.h"
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2::wifi::ofdm {

constexpr double kRate = 20e6;           // samples per second
constexpr int kFft = 64, kCp = 16, kSym = 80;
constexpr int kStfLen = 160, kLtfLen = 160, kPreamble = 320;
constexpr int kRate6 = 0xB;              // RATE field 1101, read with R1 as the lowest bit: 6 Mbit/s (Table 17-6)
constexpr int kRate12 = 0xA;             // RATE field 0101: 12 Mbit/s, QPSK rate 1/2 (802.11p sends it at 6 Mbit/s on a 10 MHz channel)

// Pieces shared by the transmit side, the receiver and the tests
int pilotPolarity(int n);                                      // p_n, +1 or -1 (n mod 127), Equation 17-25
const std::vector<cf32>& ltfSymbol();                          // the 64 time samples of one long training symbol (unit power)
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& in);                   // rate 1/2: bit A (g0) then bit B (g1) for every input bit; the encoder starts at state 0
int viterbiDecode(const float* soft, int nBits, uint8_t* out);                     // nBits outputs from 2 nBits soft values (> 0: a one); start and end in state 0; returns the path metric
void scramble(std::vector<uint8_t>& bits, uint8_t state);       // x^7 + x^4 + 1 (clause 17.3.5.5); the same call descrambles

// The whole PPDU as 20 Msps samples (unit average power in the data part): preamble, SIGNAL, DATA. mpdu includes its FCS. scrSeed: the 7-bit
// scrambler start state (non-zero). The 6 Mbit/s rate only (what beacons use).
std::vector<cf32> txPpdu(const std::vector<uint8_t>& mpdu, uint8_t scrSeed = 0x5D);

struct Info {
    double cfoHz = 0;       // carrier offset measured on the preamble (the signal sits this far above where it was expected)
    float snrDb = 0;        // from the decoded carriers' error
    float rssiDb = -200;    // mean power of the packet, dB (full scale 0)
    int64_t at = 0;         // sample index of the long training start
    int length = 0;         // PSDU bytes (FCS included)
};

class OfdmRx {
public:
    using FrameCb = std::function<void(const uint8_t* mpdu, size_t nNoFcs, const Info&)>;
    OfdmRx();
    ~OfdmRx();
    void setCallback(FrameCb cb);
    void setAllowQpsk(bool on);                                // also read QPSK rate 1/2 frames (802.11p); off by default
    void reset();                                              // forget the buffer; the counters stay
    void feed(const cf32* x, size_t n);                        // 20 Msps samples, any block size
    struct Stats { uint64_t triggers = 0, noSync = 0, signalOk = 0, signalBad = 0, otherRate = 0, framesOk = 0, framesBad = 0; };
    const Stats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::wifi::ofdm
