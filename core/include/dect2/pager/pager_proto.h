// POCSAG and FLEX at the bit level: the BCH(31,21) code word, the message parsers and the encoders (the test signal and the tests use the encoders).
// POCSAG: ITU-R M.584-2. FLEX has no free specification; the layout here follows the public descriptions of the protocol.
// Bit order: a "word" is 32 bits with the first transmitted bit in bit 31: 21 data bits, 10 BCH check bits, one even parity bit.
#pragma once
#include "pager_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace pager {

constexpr uint32_t kPocsagSync = 0x7CD215D8, kPocsagIdle = 0x7A89C197;
constexpr uint32_t kFlexMarker = 0xA6C6AAAA;       // the middle of sync 1
constexpr int kFlexPhaseWords = 88;                // 11 blocks of 8 words per phase

// ---- the code word
uint32_t encodeWord(uint32_t data21);              // data21 (first transmitted bit in bit 20) -> the 32-bit word
struct Fixed {
    bool ok = false;
    int bits = 0;                                  // bits that were wrong (the parity bit counts)
    uint32_t data = 0;                             // the 21 data bits
};
Fixed correctWord(uint32_t w);                     // up to two wrong bits
uint32_t rev21(uint32_t v);

// ---- POCSAG
struct PocsagPage {
    uint32_t ric = 0;                              // 21 bits; the low three select the frame of the batch
    int fn = 0;                                    // function bits 0 to 3
    int type = kPagerAlpha;                        // kPagerNumeric, kPagerAlpha or kPagerTone (no message)
    std::string text;
};
// A whole transmission as bits: the preamble, then batches (sync word and 16 code words each)
std::vector<uint8_t> pocsagBits(const std::vector<PocsagPage>& pages, int preambleBits = 576);

// The message bits (first transmitted first) read as 7-bit characters look like real text: mostly letters, digits and spaces, few odd signs,
// no run of control characters, at least 3 characters. Auto mode reads such a message as alphanumeric whatever its function code.
bool pocsagLooksAlpha(const std::vector<uint8_t>& bits);
// The same for the German character set: the places [ \ ] { | } ~ of the 7-bit text are umlauts and sharp s, so they count as letters
// (a short page such as "Brand Gro~" is text, not noise). alpha: the text as alphaFromBits gives it.
bool pocsagLooksAlphaGerman(const std::string& alpha);

class PocsagParser {
public:
    int readMode = kPocsagReadAuto;                // PocsagReadMode
    void reset();
    void word(uint32_t raw, int slot, std::vector<PagerMessage>& out);   // slot 0 to 15 in the batch; finished pages are appended to out
    void flush(std::vector<PagerMessage>& out);
    int ok = 0, fixed = 0, failed = 0;             // code words since the caller cleared them
private:
    void finish(std::vector<PagerMessage>& out);
    bool have_ = false, damaged_ = false;
    int gap_ = 0;                                  // unrepaired message words since the last good one (20 bits each)
    uint32_t ric_ = 0;
    int fn_ = 0, fixedBits_ = 0;
    std::vector<uint8_t> bits_;
};

// ---- FLEX
struct FlexMode {
    uint16_t code;                                 // sync code A of sync 1
    int baud, levels;                              // symbol rate of the data and 2 or 4 levels
    int speed;                                     // PagerSpeed
};
const FlexMode* flexModeForCode(uint16_t code, int maxDistance);   // the table entry closest to code, or nullptr
const FlexMode* flexModeForSpeed(int speed);
uint32_t flexFiwData(int cycle, int frame);        // the frame information word (field value, first transmitted bit in bit 0) with its checksum
bool flexFiwCheck(uint32_t f, int& cycle, int& frame);

// The 4-bit check of the block information word, the vector words and the FIW: bits 0-3 set so that the nibbles of the 21 bits plus bit 20 sum to 0xF
uint32_t flexWithCheck(uint32_t f);
// Capcodes above kFlexLongCapBase go out as a long address (two address words and two vector words); the decoder's formula is
// cap = w1 + ((w2 ^ 0x1FFFFF) << 15) + 0x1F9000 with w1 from 1 to 0x8000
constexpr uint32_t kFlexLongCapBase = 0x1F9000;
bool flexLongCap(uint32_t cap);
void flexLongWords(uint32_t cap, uint32_t& w1, uint32_t& w2);

struct FlexPage {
    uint32_t cap = 0;                              // capcode, 1 to 1933312 (short address) or above 2068480 (long address)
    int type = kPagerAlpha;                        // kPagerTone, kPagerNumeric or kPagerAlpha
    std::string text;
};
std::vector<uint8_t> flexPhaseBits(const std::vector<FlexPage>& pages);   // 2816 bits of one phase, interleaved and protected

struct FlexPhaseStat {
    int ok = 0, fixed = 0, failed = 0;
    bool biwOk = false;                            // the phase carried a block information word
};
void flexDecodePhase(const uint8_t* bits, std::vector<PagerMessage>& out, FlexPhaseStat& st);   // bits: 2816 transmitted bits of one phase

// 32 transmitted bits t[0..31] (t[0] first) -> the 32-bit word above
inline uint32_t wordFromBits(const uint8_t* t) { uint32_t w = 0; for (int i = 0; i < 32; i++) w = (w << 1) | (t[i] & 1u); return w; }

// ---- internals shared by pager_proto.cpp (C++) and pager_proto_rs.cpp (Rust-backed), also what the differential test compares
std::string alphaFromBits(const std::vector<uint8_t>& bits);                 // 7-bit characters, first bit least significant; padding dropped
std::string numericFromBits(const std::vector<uint8_t>& bits, bool flex);    // 4-bit digits; flex: FLEX character set and filler 12 dropped
struct PocsagWord {                                // what one received POCSAG code word is
    enum Kind { Failed, Idle, Address, Data };
    int kind = Failed;
    uint32_t a = 0, b = 0;                         // address: the top 18 address bits and the function bits; data: the 20 data bits
    int bits = 0;                                  // bits the BCH code corrected
    bool fixed = false;                            // the word needed a repair
};
PocsagWord pocsagClassify(uint32_t raw);

} // namespace pager
} // namespace dect2
