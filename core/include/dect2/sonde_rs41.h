// Vaisala RS41: frame decoder from hard symbols, and the frame builder for the test signal.
// Facts (frame layout, whitening mask, CRC, RS code, field positions, temperature / humidity formulas) are from rs1729/RS
// (demod/mod/rs41mod.c, rs41/rs41ptu.c, demod/mod/bch_ecc_mod.c) and from the example frame in rs41/rs41.txt there.
//
// On air: 4800 bit/s GFSK, a 320 bit preamble of alternating bits, then the frame, each byte sent LSB first. Every byte of the frame
// (header included) is XORed with a 64 byte mask. Frame of 320 bytes (518 when byte 0x38 is 0xF0):
//   0x00 header 86 35 F4 40 93 DF 1A 60 | 0x08 RS parity 2 x 24 | 0x38 frame type | blocks from 0x39: id, length, data, CRC-16 (LE)
// The RS(255,231) code is shortened and interleaved: even data bytes (from 0x38) form codeword 1, odd ones codeword 2; parity of codeword 1
// is at 0x08..0x1F, of codeword 2 at 0x20..0x37.
#pragma once
#include "sonde_bits.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

constexpr int kRs41FrameLen = 320;
constexpr int kRs41FrameLenExt = 518;
constexpr int kRs41CalFrames = 51;

const uint8_t* rs41Mask();                               // the 64 byte whitening mask
uint16_t rs41Crc16(const uint8_t* p, int n);             // CRC-16/CCITT-FALSE as the block checks use it (poly 0x1021, init 0xFFFF)

// Reed-Solomon over GF(256), polynomial 0x11D, roots alpha^0 .. alpha^23. A codeword is cw[0..23] = parity, cw[24..n-1] = data
// (array index = power of x); n - 24 <= 231.
void rs41RsParity(const uint8_t* data, int k, uint8_t* parity24);
int rs41RsDecode(uint8_t* cw, int n);                    // corrects in place; number of corrected bytes, or -1 when it cannot

// Decoder. Besides the SondeBitDecoder interface it can decode a frame that is already bytes (not whitened).
class Rs41Decoder : public SondeBitDecoder {
public:
    Rs41Decoder();
    ~Rs41Decoder() override;
    const char* type() const override { return "RS41"; }
    double symbolRate() const override { return 4800.0; }
    void push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) override;
    void reset() override;                               // forgets a half received frame, keeps the calibration of the sonde
    // frame: 320 or 518 bytes, de-whitened, RS parity as received. Returns true when it was a frame (header matched);
    // fix.crcOk tells whether it is intact.
    bool decodeFrame(uint8_t* frame, int len, SondeFix& fix);
    double announcedFreqHz() const;                      // frequency the sonde reports in its configuration (0 until subframe 0 arrives)
    int calibrationDone() const;                         // subframes collected, of 51
    uint64_t framesBadHeaderOk() const;                  // frames whose header matched but that failed the checks
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<SondeBitDecoder> makeRs41Decoder();

// ---- test signal side ----
// Calibration data (51 subframes of 16 bytes) of a made-up sonde: realistic structure and magnitudes, not a real sonde's numbers.
struct Rs41Cal {
    uint8_t bytes[kRs41CalFrames * 16] = {};
};
Rs41Cal rs41MakeCal(double freqHz, int killTimerS = -1);
// 24 bit counts of the PTU packet for a temperature (C) and relative humidity (%), inverse of the receiver's formulas
void rs41PtuCounts(const Rs41Cal& cal, double tempC, double rh, uint32_t meas[12]);
// A normal frame (320 bytes, not whitened, with RS parity and block CRCs). `subframe` (0..50) is the calibration subframe it carries.
std::vector<uint8_t> rs41Frame(const SondeTruth& t, const Rs41Cal& cal, int subframe);
// On-air bits (one per byte, 0/1) of a frame: optional preamble, then the whitened bytes LSB first
std::vector<uint8_t> rs41Symbols(const std::vector<uint8_t>& frame, bool preamble = true);

} // namespace dect2
