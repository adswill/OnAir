// NAVTEX / SITOR-B (FEC) receive and transmit side: CCIR 476 code (ITU-R M.476, M.625), time diversity, ZCZC ... NNNN framing (ITU-R M.540).
// The decoder works on bits, so it is tested without the radio layer.
#pragma once
#include "marine_tel.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

// Service signals of the CCIR 476 code (7 bits, four of them mark, first bit sent = least significant bit of the value, as fldigi navtex.cxx reads them; alpha 0x0F goes out as BBBBYYY)
constexpr uint8_t kAlpha = 0x0F, kBeta = 0x33, kRep = 0x66, kLtrs = 0x5A, kFigs = 0x36, kSpare = 0x6A;
constexpr uint8_t kSpace = 0x5C, kCr = 0x78, kLf = 0x6C;

bool ccir476Valid(unsigned code);                          // 7 bits with exactly four set
// The character of a code word in the letter or figure set; 0 for service signals and the bell. LTRS and FIGS are not handled here.
int ccir476Char(uint8_t code, bool figs);
bool ccir476Code(char c, bool figs, uint8_t& code);        // false when the set has no such character
const char* navtexSubjectName(char b2);

// Text -> code words, with LTRS/FIGS shifts where the set changes; the shift is restated after every line feed.
std::vector<uint8_t> navtexEncode(const std::string& text);
// FEC mode B on the air: tokens are DX characters (a code, or -1 for the idle signal). Result: 7-bit codes in transmission order,
// DX at even places, the retransmission (5 places later) at odd places, alpha where there is nothing to retransmit.
std::vector<uint8_t> sitorBSlots(const std::vector<int>& dxTokens);
std::vector<uint8_t> codesToBits(const std::vector<uint8_t>& codes);   // LSB first, one bit per byte

class NavtexDecoder {
public:
    using MsgCb = std::function<void(const NavtexMessage&)>;
    explicit NavtexDecoder(bool invert = false);
    void setCallback(MsgCb cb) { cb_ = std::move(cb); }
    void reset();
    // One received bit. soft > 0: the 'B' (1) state of the code, |soft| 0..1 = how sure. With invert the sense is swapped.
    void pushBit(float soft);
    bool locked() const { return locked_; }
    int parity() const { return parity_; }
    uint64_t charsDecoded() const { return charsOut_; }
    uint64_t validChars() const { return validChars_; }     // code words with four marks seen while locked
    uint64_t badChars() const { return badChars_; }
    void flush();                                            // finish a message in progress as incomplete
private:
    struct Slot { uint8_t code = 0; bool valid = false; uint8_t bit[7] = {}; float conf[7] = {}; uint64_t endBit = 0; };
    void onChar(const Slot& s);
    void process(bool force);
    void emitChar(int ch, bool err);
    void finish(bool complete);
    bool combine(const Slot& a, const Slot& b, uint8_t& code) const;

    bool invert_;
    MsgCb cb_;
    uint8_t reg_[7] = {}; float regConf_[7] = {};
    uint64_t bitCount_ = 0;
    int validRun_[7] = {};
    uint8_t hcode_[7][32] = {}; bool hvalid_[7][32] = {}; float hconf_[7][32] = {}; int hpos_[7] = {};   // per bit phase: the last 32 code words
    bool phaseGood(int ph) const;
    int phaseScore(int ph, int* validRun = nullptr) const;
    bool locked_ = false;
    int lockPhase_ = 0;
    std::deque<Slot> slots_;
    uint64_t slotBase_ = 0;            // absolute index of slots_.front()
    uint64_t slotNext_ = 0;            // absolute index of the next slot
    int recent_ = 0; uint32_t recentMask_ = 0;   // last 24 slots: bit set = invalid
    int evid_[2] = {};
    int parity_ = -1;
    uint64_t nextDx_ = 0;
    bool figs_ = false;
    uint64_t charsOut_ = 0, validChars_ = 0, badChars_ = 0;
    uint64_t sinceChar_ = 0;           // bits since the last slot while unlocked
    // message
    int mstate_ = 0;                   // 0 outside, 1 header, 2 body
    std::string tail_;                 // last four characters, raw
    std::string hdr_, body_;
};

} // namespace marine
} // namespace dect2
