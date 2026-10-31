// RDS / RBDS data layer (IEC 62106, NRSC-4-B): block synchronisation, error correction and the group decoder.
//
// The bits come from the FM receiver's 1187.5 bit/s demodulator (after the differential decoding), each with its reliability. The decoder
// finds the block boundaries from the checkwords, corrects the errors the code can correct (a wrong symbol of the biphase signal is two
// neighbouring bits after the differential decoding: every burst of one or two bits, and with the reliabilities two weak symbols), and
// decodes the groups: PI, PS, PTY, TP/TA, M/S, alternative frequencies (0A), ECC, language and PIN (1A/1B), RadioText A/B (2A/2B),
// open data announcements (3A) with RadioText+ (RT+), enhanced RadioText (eRT, UTF-8 or UCS-2) and TMC, clock time (4A), PTYN (10A) and
// other networks' names (EON, 14A). A group that holds a corrected block is used only when the same group has come before, so that a
// miscorrection cannot put a wrong character on screen.
#pragma once
#include "rds_tmc.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One RadioText+ tag: its content type (1 title, 4 artist, ...) and the text it points at in the RadioText.
struct RtPlusTag {
    int type = 0;
    std::string name;    // "Title", "Artist", ...
    std::string text;
};

// One other network (EON, group 14A): its PI and the name it sends.
struct RdsEon {
    int pi = 0;
    std::string ps;      // 8 characters once complete, "" before
};

// What the decoder knows about the station, filled into FmTelemetry.
struct RdsInfo {
    std::vector<double> afMhz;          // alternative frequencies (0A), MHz, ascending
    std::string ptyn;                   // programme type name (10A), 8 characters
    int ecc = -1;                       // extended country code (1A variant 0), -1 unknown
    int language = -1;                  // language code (1A variant 3), -1 unknown
    int pinDay = 0, pinHour = 0, pinMinute = 0;   // programme item number (1A/1B), day 0 = none
    int64_t clockUtc = 0;               // clock time (4A), seconds since 1970 UTC, 0 = not received
    int clockOffsetMin = 0;             // the local time offset sent with it, minutes
    double clockAt = 0;                 // when it was received (seconds since 1970, the computer's clock)
    std::vector<std::pair<int, int>> oda;   // open data applications (3A): (AID, group type code 0..31: type * 2 + version B)
    std::vector<RtPlusTag> rtPlus;      // RadioText+ tags of the current item, newest per type
    bool rtPlusRunning = false;         // RT+ "item running" flag
    std::string ert;                    // enhanced RadioText (UTF-8)
    std::vector<RdsEon> eon;            // other networks
    std::string psPartial;              // the name being collected, '_' where nothing has come yet ("" when nothing has)
    uint64_t blocksGood = 0, blocksCorrected = 0, blocksBad = 0;
    uint64_t blocksVoted = 0;   // blocks rebuilt by voting over repeated copies (a subset of blocksCorrected)
};

// Content type names of RadioText+ (the RT+ specification, 64 classes)
const char* rtPlusTypeName(int type);

// A group as redsea prints it in --output hex: "PPPP BBBB CCCC DDDD", a bad block as "----"
std::string rdsHexGroup(const uint16_t blocks[4], const uint8_t err[4]);

class RdsDecoder {
public:
    // err per block: 0 good, 1 corrected, 2 bad (not usable)
    using GroupTap = std::function<void(const uint16_t blocks[4], const uint8_t err[4])>;

    void reset();
    // One bit after the differential decoding and its reliability (any positive scale: only the order among the bits of a block counts).
    void pushBit(int bit, float reliability = 1.f);
    // A whole group from elsewhere (tests, a file of groups): the group decoder alone.
    void onGroup(const uint16_t blocks[4], const uint8_t err[4], double now);
    void setGroupTap(GroupTap tap) { tap_ = std::move(tap); }
    // Error correction on (default): off keeps only the blocks whose checkword is right. Before the first bit (not thread safe).
    void setCorrection(bool burst, bool soft) { ecBurst_ = burst; ecSoft_ = soft; ecVote_ = soft; }   // voting (repeated copies of a block) comes with the top level

    void setVoting(bool on) { ecVote_ = on; }   // after setCorrection; on by default

    // state
    bool synced = false;
    float okPct = 0;                    // share of blocks usable (good or corrected), recent
    uint64_t groups = 0;                // groups decoded (blocks B and D usable)
    int pi = 0;
    bool tp = false, ta = false, ms = true;
    int pty = 0;
    std::string ps, rt;
    bool rtComplete = false;
    // Bytes 0x80..0xFF of PS / RadioText / PTYN / EON names: 0 EBU Latin only, 1 (default) GB2312 when the bytes pair up as Chinese characters, else EBU Latin, 2 GB2312 always (#350)
    int charset = 1;
    TmcDecoder tmc;
    RdsInfo info;

private:
    void block(uint32_t word26, uint8_t err, int pos, uint32_t raw);
    void decodeGroup(const uint16_t blocks[4], const uint8_t err[4], double now, uint32_t rawHash);
    bool correct(uint32_t& reg, uint32_t syndrome, int pos) const;
    bool seenBefore(const uint16_t* b, const uint8_t* err, uint32_t raw);
    void rtPlusGroup(uint16_t b, uint16_t c, uint16_t d);
    void ertGroup(uint16_t b, uint16_t c, uint16_t d);
    void rtPlusRefresh();

    uint32_t reg_ = 0;
    uint64_t bitNo_ = 0;
    float rel_[32] = {};                // the reliabilities of the last 32 bits (ring, by bitNo_)
    int cnt_ = 0, expect_ = 0, badRun_ = 0;
    struct Hit { uint64_t bit; int pos; };
    Hit hits_[8] = {};
    int nHits_ = 0;
    uint16_t blk_[4] = {};
    uint8_t err_[4] = {2, 2, 2, 2};
    bool ecBurst_ = true, ecSoft_ = true, ecVote_ = true;
    // Voting: the bad blocks of one position (A, B, C, D) are kept with their bit reliabilities. Groups repeat (block A is the PI in every
    // group, a PS or RT segment comes again every few seconds), so three or more copies that lie close together vote bit by bit (weighted
    // by reliability), and the result is taken only when its checkword is right.
    struct VoteCopy { uint32_t w; float llr[26]; uint64_t at; };
    static constexpr int kVote = 8;
    VoteCopy vote_[4][kVote] = {};
    int nVote_[4] = {}, votePos_[4] = {};
    bool voteBlock(uint32_t& w, int pos);
    GroupTap tap_;
    // group decoder
    char psBuf_[9] = "        ";
    uint8_t psSeen_ = 0;
    char psAnyBuf_[9] = "        ";   // the latest bytes of each segment whatever came before: a scrolling name never fills psBuf_ (#350)
    uint8_t psAnySeen_ = 0;
    int psSince_ = 0;                 // type 0 groups since the name last completed
    char rtBuf_[65] = {};
    int rtAb_ = -1;
    uint16_t rtSeen_ = 0;
    bool rt2B_ = false;
    char ptynBuf_[9] = "        ";
    uint8_t ptynSeen_ = 0;
    int ptynAb_ = -1;
    int rtPlusGroup_ = -1, ertGroup_ = -1;   // the group type codes carrying RT+ / eRT (from 3A), -1 none
    int ertUtf8_ = 1;
    uint8_t ertBuf_[128] = {};
    uint32_t ertSeen_ = 0;
    int rtPlusToggle_ = -1;
    struct RawTag { int type, start, len; };
    RawTag rtPlusRaw_[2] = {{0, 0, 0}, {0, 0, 0}};
    struct EonBuf { int pi; char ps[9]; uint8_t seen; };
    std::vector<EonBuf> eonBuf_;
    struct Sig { uint16_t b, c, d; uint8_t cOk; uint32_t raw; };   // raw: a hash of the received words of the corrected blocks, 0 = clean
    uint32_t rawHash_ = 0;
    static constexpr int kSigs = 64;   // a station's whole cycle of groups (PS, 16 RT segments, the rest)
    Sig sigs_[kSigs] = {};
    int nSig_ = 0, sigPos_ = 0;
};

} // namespace dect2
