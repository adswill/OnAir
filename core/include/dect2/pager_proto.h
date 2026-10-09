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

class PocsagParser {
public:
    void reset();
    void word(uint32_t raw, int slot, std::vector<PagerMessage>& out);   // slot 0 to 15 in the batch; finished pages are appended to out
    void flush(std::vector<PagerMessage>& out);
    int ok = 0, fixed = 0, failed = 0;             // code words since the caller cleared them
private:
    void finish(std::vector<PagerMessage>& out);
    bool have_ = false, damaged_ = false;
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

struct FlexPage {
    uint32_t cap = 0;                              // capcode, 1 to 1933000
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

} // namespace pager
} // namespace dect2
