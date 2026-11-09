// MDC-1200 (Motorola) burst: 1200 bit/s, tones 1200 and 1800 Hz (a bit that differs from the one before is sent as 1800 Hz, a repeated one as
// 1200 Hz), 7 bytes of 0x55 and the sync 07 09 2A 44 6F, then 14 bytes: op, argument, unit ID (two bytes), CRC-16 (low byte first), a zero byte and
// seven bytes of a rate 1/2 convolutional parity (taps 0, 2, 5, 6 of a 7 bit register), the 112 bits interleaved over 7 rows of 16. A "double" packet
// (ops 0x35 and 0x55) is followed at once by a second 14 byte block with four more data bytes.
//
// Written from the format as described by the public decoder of Matthew Kaufman (github.com/atmatthewat/mdc-encode-decode, GPL-2.0): none of its code
// is used here (the licence is not compatible with GPL-3); the format facts, which a licence does not cover, were checked against it. Meaning of
// the opcodes: only the PTT ID (op 0x01) is named, every other op is shown as a number.
// Not implemented: the single-bit error correction of the parity bytes (a packet with a CRC error is dropped).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace chv {

struct MdcPacket {
    uint8_t op = 0, arg = 0;
    uint16_t unitId = 0;
    bool dbl = false;              // a double packet: extra[] holds the second block's four data bytes
    uint8_t extra[4] = {0, 0, 0, 0};
    double timeSec = 0;            // where the packet ended in the audio fed so far
};

std::string mdcOpText(const MdcPacket& p);     // "PTT ID (pre)", "op 0x23 arg 0x80"
uint16_t mdcCrc(const uint8_t* d, size_t n);   // CRC-16, polynomial 0x1021 reflected, start 0, final xor 0xFFFF

// The burst as audio (full scale = amp): `leadBytes` 0x55 bytes (7 in a real transmission), then sync and data. A double packet needs op 0x35 or 0x55
// and the four extra bytes.
std::vector<float> mdcEncode(const MdcPacket& p, double rate, double amp = 0.5, int leadBytes = 7);

class MdcDecoder {
public:
    explicit MdcDecoder(double rate = 48000.0);
    void setCallback(std::function<void(const MdcPacket&)> cb) { cb_ = std::move(cb); }
    void reset();
    void feed(const float* x, size_t n);
    uint64_t packets() const { return count_; }
    uint64_t crcFailures() const { return crcBad_; }   // bursts whose sync was found but whose CRC did not check

private:
    static constexpr int kPhases = 8;
    struct Phase {
        uint64_t reg = 0;          // the last 64 decisions (1 = 1800 Hz = the bit changed)
        double next = 0;           // the sample time of its next bit boundary
        int state = 0;             // 0 hunting, 1 first block, 2 second block
        int got = 0;
        uint8_t blk[112];
        MdcPacket first;
    };
    void decide(int ph, double t);
    bool block(const uint8_t* x, uint8_t d[14]);   // x: the 112 decisions; true when the CRC checks
    void emit(const MdcPacket& p);
    double rate_, spb_;
    int len_;
    std::vector<float> ring_;
    size_t wr_ = 0;
    uint64_t n_ = 0;
    std::vector<float> c12_, s12_, c18_, s18_;
    Phase ph_[kPhases];
    std::function<void(const MdcPacket&)> cb_;
    uint64_t count_ = 0, crcBad_ = 0;
    MdcPacket last_; double lastEnd_ = -1;
};

} // namespace chv
} // namespace dect2
