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

// State of the HDLC receiver: a plain struct (same layout as the Rust one in rust/onair_ax25/src/hdlc.rs) so that both implementations share it.
struct HdlcState {
    int32_t prev = 0;
    uint8_t pat = 0, acc = 0;
    int32_t cnt = 0;
    bool inFrame = false, hunting = true, resyncClock = false;
    int32_t flagRecent = 0;
    uint32_t len = 0;            // bytes in buf
    uint8_t buf[402] = {};       // frame bytes (at most 401 are stored)
};
// One bit through the receiver state: 0 nothing completed, 1 a frame with a good FCS (buf[0..len) without the FCS), 2 a frame of plausible length with
// a bad FCS (buf[0..len) all bytes). Rust (packet_ax25_rs.cpp).
int hdlcStep(HdlcState& s, int level, bool nrzi, size_t& len);
void hdlcReset(HdlcState& s);

// Receives the NRZI line levels (or, with nrzi = false, plain HDLC bits) one at a time and cuts out the frames.
class HdlcRx {
public:
    // ok = the FCS was right (data = the frame without FCS); !ok = a frame of a plausible length failed the check (data = all bytes)
    using Callback = std::function<void(const std::vector<uint8_t>& data, bool ok)>;
    explicit HdlcRx(Callback cb = nullptr, bool nrzi = true) : cb_(std::move(cb)), nrzi_(nrzi) {}
    void setCallback(Callback cb) { cb_ = std::move(cb); }
    // "Fix bits": with a raw callback set, the receiver also keeps the bits between two flags (after NRZI, still bit-stuffed) and hands
    // over every stretch of a frame's length that did not give a good frame, for repairFrame()
    using RawCallback = std::function<void(const std::vector<uint8_t>& stuffedBits)>;
    void setRawCallback(RawCallback cb) { rawCb_ = std::move(cb); raw_.clear(); rawOk_ = false; }
    void reset() { hdlcReset(st_); raw_.clear(); rawOk_ = false; rprev_ = 0; rpat_ = 0; }
    void bit(int level);
    // every line bit also goes here: the FX.25 and IL2P receivers listen to the same demodulator
    void setBitTap(std::function<void(int)> f) { bitTap_ = std::move(f); }
    bool inFrame() const { return st_.inFrame; }
    bool flagSeen() const { return st_.flagRecent > 0; }   // a flag came a short while ago
    bool takeClockResync() { const bool r = st_.resyncClock; st_.resyncClock = false; return r; }
private:
    Callback cb_;
    RawCallback rawCb_;
    std::function<void(int)> bitTap_;
    bool nrzi_;
    HdlcState st_;
    std::vector<uint8_t> raw_;
    bool rawOk_ = false;
    int rprev_ = 0;
    uint8_t rpat_ = 0;
};

// Destuffs the bits between two flags and checks the FCS: true with the frame (without FCS) in out
bool frameFromStuffedBits(const std::vector<uint8_t>& bits, std::vector<uint8_t>& out);
// The bit patterns "fix bits" tries, as offsets in the received bits: one wrong decision of the demodulator (level 1), and also two in a
// row (level 2). After NRZI one wrong line bit is two wrong bits in a row; with the G3RUH scrambler (scrambled = true) also 12 and 17 bits later.
std::vector<std::vector<int>> fixPatterns(int level, bool scrambled);
// Tries to repair a frame that failed: each pattern at every position, until one gives a good FCS and passes sane(). Returns the
// 1-based index of the pattern that worked (0 = no repair found); out is the frame without FCS.
int repairFrame(const std::vector<uint8_t>& stuffedBits, const std::vector<std::vector<int>>& patterns, std::vector<uint8_t>& out,
                const std::function<bool(const std::vector<uint8_t>&)>& sane);
// Multi-demodulator voting. The demodulators of the receiver (several tone correlators, slicer weights and clock loops) each cut the same
// transmission into stuffed bits; when none of them gets a good FCS their copies still disagree only where the signal was doubtful. The copies
// of the same length are voted bit by bit (the majority is the base), the positions where they disagree are the candidates (at most
// maxContested, the closest votes first), and every combination of flips there is tried, fewest flips first, until the FCS is right and sane()
// agrees. Returns false when nothing fits. flips: bits changed against the majority; copies: how many copies voted.
struct VoteResult { std::vector<uint8_t> frame; int flips = 0, copies = 0, contested = 0; };
bool voteRepair(const std::vector<std::vector<uint8_t>>& copies, const std::function<bool(const std::vector<uint8_t>&)>& sane, VoteResult& r, int maxContested = 8);
// A plausible AX.25 frame for a repaired one: addresses of capital letters and digits (space padded), a known control field, and an
// information field without control characters other than CR, LF and tab (Mic-E's 0x1C-0x1F allowed)
bool plausibleFrame(const std::vector<uint8_t>& frameNoFcs);

} // namespace ax25
} // namespace dect2
