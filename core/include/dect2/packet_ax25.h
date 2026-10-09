// AX.25 link layer for packet radio: the frame check sequence, HDLC bit handling (flags, bit stuffing, NRZI), frame parsing and building.
// Plain data in, plain data out; the radio side is in packet_rx.cpp and the test signal in packet_gen.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace ax25 {

// CRC-16-CCITT as AX.25 uses it (reflected, start 0xFFFF, result inverted). appendFcs() adds it low byte first.
uint16_t fcs(const uint8_t* d, size_t n);
void appendFcs(std::vector<uint8_t>& frame);
bool fcsOk(const uint8_t* d, size_t n);                 // n includes the two FCS bytes

struct Address {
    std::string call;            // without the SSID, up to 6 characters
    int ssid = 0;                // 0..15
    bool h = false;              // "has been repeated" bit (digipeaters), command/response bit (source and destination)
    std::string str() const;     // CALL or CALL-SSID
};

struct Frame {
    Address to, from;
    std::vector<Address> path;   // digipeaters
    uint8_t control = 0x03;
    int pid = 0xF0;              // -1: the frame type carries none
    std::string info;
    std::string pathStr() const;     // "WIDE1-1*,WIDE2-1": the star marks the last digipeater that has repeated the frame
    std::string tnc2() const;        // FROM>TO,PATH:info
    std::string typeName() const;    // "UI", "I", "RR", "SABM", ...
    bool isUi() const { return (control & 0xEF) == 0x03; }
};

// Parses a frame without its FCS. False when the addresses are malformed.
bool parseFrame(const uint8_t* d, size_t n, Frame& f);
// Builds the bytes of a frame, FCS included.
std::vector<uint8_t> buildFrame(const Frame& f);
// Frame bytes (with FCS) -> bits ready for NRZI: flags in front, bit stuffing, flags behind. LSB first as on the air.
std::vector<uint8_t> hdlcBits(const std::vector<uint8_t>& frameWithFcs, int flagsBefore, int flagsAfter);
// NRZI: a 0 changes the level, a 1 keeps it. Returns the levels.
std::vector<uint8_t> nrziEncode(const std::vector<uint8_t>& bits, uint8_t startLevel = 0);
// G3RUH scrambler x^17 + x^12 + 1 for 9600 baud (state: the last 17 output bits)
std::vector<uint8_t> scramble(const std::vector<uint8_t>& bits, uint32_t& state);

// Receives the NRZI line levels (or, with nrzi = false, plain HDLC bits) one at a time and cuts out the frames.
class HdlcRx {
public:
    // ok = the FCS was right (data = the frame without FCS); !ok = a frame of a plausible length failed the check (data = all bytes)
    using Callback = std::function<void(const std::vector<uint8_t>& data, bool ok)>;
    explicit HdlcRx(Callback cb = nullptr, bool nrzi = true) : cb_(std::move(cb)), nrzi_(nrzi) {}
    void setCallback(Callback cb) { cb_ = std::move(cb); }
    void reset();
    void bit(int level);
    bool inFrame() const { return inFrame_; }
    bool flagSeen() const { return flagRecent_ > 0; }   // a flag came a short while ago
private:
    Callback cb_;
    bool nrzi_;
    int prev_ = 0;
    uint8_t pat_ = 0, acc_ = 0;
    int cnt_ = 0;
    bool inFrame_ = false;
    int flagRecent_ = 0;
    std::vector<uint8_t> buf_;
};

} // namespace ax25
} // namespace dect2
