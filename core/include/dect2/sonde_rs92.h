// Vaisala RS92: frame decoder from hard symbols, and the frame builder for the test signal. Phase B of the radiosonde mode.
// Facts from rs1729/RS (demod/mod/rs92mod.c, rs92/rs92.txt, whose example frame the known-answer test uses).
//
// On air: 4800 symbols/s, Manchester coded (data 2400 bit/s, 10 -> 0, 01 -> 1), one frame per second of 240 bytes, each byte sent as
// start bit 0, eight bits LSB first, stop bit 1. Frame: 2A 2A 2A 2A 2A 10 | message 210 bytes | Reed-Solomon parity 24 bytes. The code is the
// same RS(255,231) as the RS41's, one codeword (parity before message in the array). The message holds blocks: id, length in 16 bit words,
// data, CRC-16 (init 0xFFFF, little endian): 0x65 configuration (frame number, serial, calibration subframe), 0x69 sensor counts,
// 0x67 GPS (time of week, 12 satellite channels), 0x68 auxiliary.
//
// What is not decoded, and why: the position needs the satellite ephemerides that the sonde does not send and that we cannot download
// (the frame holds pseudoranges only); temperature, humidity and pressure need the calibration data, which the sonde sends XOR-masked
// with a table that was not available. The receiver shows serial, frame number, announced frequency, GPS time of week and the number of
// tracked satellite channels (status nibbles).
#pragma once
#include "sonde_bits.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

constexpr int kRs92FrameLen = 240;
constexpr int kRs92CalFrames = 32;

class Rs92Decoder : public SondeBitDecoder {
public:
    Rs92Decoder();
    ~Rs92Decoder() override;
    const char* type() const override { return "RS92"; }
    double symbolRate() const override { return 4800.0; }
    void push(const uint8_t* sym, size_t n, double timeSec, std::vector<SondeFix>& out) override;
    void reset() override;
    // frame: 240 bytes as sent (header, message, parity), already unframed. Returns true when the header matched.
    bool decodeFrame(uint8_t* frame, SondeFix& fix);
    double announcedFreqHz() const;
    int calibrationDone() const;
    int towSeconds() const;                  // GPS time of week of the last good frame, seconds (-1 before the first)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<SondeBitDecoder> makeRs92Decoder();

// test signal side
struct Rs92Truth {
    std::string serial;
    int frame = 0;
    double unixTime = 0;
    int sats = 9;
    double freqHz = 405.3e6;
};
std::vector<uint8_t> rs92Frame(const Rs92Truth& t, int subframe);                 // 240 bytes with RS parity and block CRCs
std::vector<uint8_t> rs92Symbols(const std::vector<uint8_t>& frame);              // framed bytes, Manchester coded, 0/1 per entry

} // namespace dect2
