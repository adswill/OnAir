// DECT NR+ (DECT-2020 NR, ETSI TS 103 636-3 / -4 V1.5.1): packets on a 1.728 MHz channel (subcarrier scaling mu = 1, Fourier scaling beta = 1: 64-point FFT, 27 kHz
// subcarriers, 56 occupied), one transmit stream. Finds the STF (7 repetitions of a 16-sample sequence under the cover sequence), equalises on the DRS and decodes the
// Physical Control Channel: 98 QPSK resource elements, scrambling with 0x44454354, rate matching and the rate 1/3 turbo code of clause 6.1 (the LTE design), CRC-16,
// blind over the 40-bit (Type 1) and 80-bit (Type 2) physical layer control field (TS 103 636-4 clause 6.2.1): packet length, short network ID, transmitter and receiver
// identities, MCS. The CRC masks 0x5555 (closed-loop MIMO) and 0xAAAA (beamforming) are tried as well.
//
// Not decoded: the Physical Data Channel (MAC PDUs), wider channels (beta > 1), mu > 1, two or more transmit streams. Receive only.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::xlink::nrplus {

constexpr double kRate = 1.728e6;
constexpr int kFft = 64, kCp = 8, kSym = 72, kStf = 112;

struct Header {
    int type = 1;                 // 1: 40 bits, 2: 80 bits
    int format = 0, lengthType = 0, length = 0, networkId = 0, txId = 0, power = 0, mcs = 0;
    int rxId = -1, streams = 0, rv = 0, ndi = 0, harq = 0, fbFormat = 0, fbInfo = 0;   // Type 2 only
    int crcMask = 0;              // 0, 0x5555 or 0xAAAA
    double cfoHz = 0;
    float snrDb = 0;
    int64_t at = 0;               // sample of the STF start
};
std::string describe(const Header& h);

// Test signal: one packet at kRate: STF, DRS on symbols 1, 6, 11 ..., the PCC carrying 'bits' (40 or 80) with the given CRC mask, random QPSK elsewhere.
std::vector<cf32> txPacket(const std::vector<uint8_t>& pccBits, int symbols, unsigned seed, int crcMask = 0);
std::vector<uint8_t> headerBits(const Header& h);   // the control field of h as 40 or 80 bits, MSB first

// building blocks, exposed for the tests
std::vector<uint8_t> turboEncode(const std::vector<uint8_t>& c);          // 3 (K + 4) bits: d0, d1, d2 streams
std::vector<uint8_t> pccEncode(const std::vector<uint8_t>& bits, int crcMask);   // 196 bits before scrambling
bool pccDecode(const std::vector<float>& soft196, std::vector<uint8_t>& bits, int& crcMask);   // soft > 0 a one

class Receiver {
public:
    using Cb = std::function<void(const Header&)>;
    Receiver();
    ~Receiver();
    void setCallback(Cb cb);
    void reset();
    void feed(const cf32* x, size_t n);    // complex samples at kRate
    uint64_t syncs() const;                // STF detections
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::xlink::nrplus
