// SITOR-A / AMTOR (ITU-R M.476-5 Annex 1 section 3.1, ARQ mode A) monitor and test generator. Written from the Recommendation's text:
// 100 Bd FSK, 170 Hz shift, the same 7-unit 4-of-7 code as SITOR-B; the information sending station (ISS) sends a block of three
// characters (210 ms) every 450 ms, the receiving station (IRS) answers in the pause with one 70 ms control signal (CS1 asks for
// "Block 1", CS2 for "Block 2", CS3 for a change of direction). A monitor hears both, so it can tell a new block from a repeat:
// the ISS sends a new block only when the control signal differs from the previous one. Receive only; nothing is transmitted.
//
// Bits are 100 Bd decisions (soft > 0 = B, the higher tone), as the marine FSK slicer gives them; words are 7 bits, the first bit
// received is the least significant one (marine_navtex.h convention), so CS1 BYBYYBB is 0x65.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2::sitor {

// Service signals of mode A (ITU-R M.476-5 table 2), first bit sent = least significant bit
constexpr uint8_t kCs1 = 0x65, kCs2 = 0x6A, kCs3 = 0x59;   // BYBYYBB, YBYBYBB, BYYBBYB
constexpr uint8_t kArqBeta = 0x33, kArqAlpha = 0x0F, kArqRq = 0x66;   // idle beta BBYYBBY, idle alpha BBBBYYY, signal repetition YBBYYBB
constexpr int kCycleBits = 45;          // 450 ms at 100 Bd
constexpr int kBlockBits = 21;          // three 7-unit characters, 210 ms

struct ArqStats {
    bool locked = false;
    uint64_t blocks = 0;        // valid blocks read while locked
    uint64_t newBlocks = 0;     // blocks printed (not repeats)
    uint64_t repeats = 0;       // blocks recognised as repeats of the previous one
    uint64_t bad = 0;           // block slots with a mutilated character
    uint64_t controlSignals = 0;   // CS1 / CS2 / CS3 heard in the pause
    int csOffsetBits = -1;      // where the control signal sits after the block end (-1: not found yet)
    uint64_t chars = 0;
    int changes = 0;            // changes of direction seen (CS3 / "+?" and the new ISS phase)
};

class ArqMonitor {
public:
    explicit ArqMonitor(bool invert = false);
    void reset();
    void pushBit(float soft);
    const ArqStats& stats() const { return st_; }
    const std::string& text() const { return text_; }
    const std::string& selcall() const { return selcall_; }
    void setEventSink(std::function<void(const std::string&)> cb) { ev_ = std::move(cb); }
    void setTextSink(std::function<void(const std::string&)> cb) { txt_ = std::move(cb); }
private:
    bool wordAt(uint64_t endBit, uint8_t& w) const;     // the 7-bit word whose last bit is endBit
    bool blockAt(uint64_t endBit, uint8_t w[3]) const;
    void onBlock(const uint8_t w[3], uint64_t endBit);
    void onBadBlock();
    void scanCs(uint64_t blockEnd);
    void emit(const uint8_t w[3]);
    void event(const std::string& s);
    void unlock();

    bool invert_;
    uint8_t bits_[256] = {};         // ring of hard bits, index = bit number & 255
    uint64_t n_ = 0;                 // bits received
    uint32_t hist_[kCycleBits] = {}; // per phase: one bit per cycle, set when three valid words ended there
    int phase_ = -1;                 // locked phase (bit number mod 45 of the block's last bit)
    uint64_t nextEnd_ = 0;           // the last bit of the next expected block
    int missRun_ = 0;
    float csHist_[kCycleBits] = {};  // control-signal evidence per offset after the block end
    int csOff_ = -1;
    int csPrev_ = -1, csPrev2_ = -1; // the last two control signals (0 none / unknown, 1, 2, 3)
    uint64_t pendingCsEnd_ = 0;      // the block end whose control signal is still to be read
    bool havePending_ = false;
    bool prevValid_ = false;
    uint8_t prevBlock_[3] = {};
    uint8_t callA_[3] = {}; bool callAValid_ = false;
    bool figs_ = false;
    std::string tail_;               // last decoded characters (for the "+?" change-over)
    ArqStats st_;
    std::string text_, selcall_;
    std::function<void(const std::string&)> ev_, txt_;
};

// ---- Test signal (complex baseband, no noise; nothing is transmitted) -----------------------------------------------------------
struct ArqGenCfg {
    double rate = 24000;         // output sample rate
    double centreHz = 0;         // where the FSK centre sits in the output
    double shiftHz = 170, baud = 100;
    double irsLevel = 0.5;       // the receiving station's level relative to the sending one
    double irsDelayBits = 8.4;   // the control signal starts this many bit periods after the block end (round trip and tE)
    double irsOffsetHz = 4;      // the second transmitter's own frequency error
    double driftHzPerSec = 0;    // slow drift of both transmitters
    int repeatEvery = 5;         // the IRS asks for a repeat after every n-th block (0: never)
    bool call = true;            // start with the two call blocks and the IRS answer
    int callPairs = 4;           // the caller repeats the pair of call blocks until the called station answers (3.1.4.4)
    std::string selcall = "QCXT";
    bool changeOver = false;     // end the first text with "+?" and let the other station send `text2`
    std::string text2;
    bool endOfComm = true;       // finish with the end-of-communication block (three alpha)
    double leadSec = 0.5, tailSec = 0.5;
};
// The ISS blocks of a text (letters / figures shifts added, padded with idle beta)
std::vector<uint8_t> arqWords(const std::string& text);
std::vector<cf32> genSitorArq(const ArqGenCfg& c, const std::string& text);

}  // namespace dect2::sitor
