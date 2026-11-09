// ProVoice, the digital voice of EDACS: 9600 bit/s, a 768 bit block (32 sync bits, then 736 bits with four IMBE 7100x4400 frames of 142 bits, 80 ms in all). The frames are error
// corrected and rewritten as IMBE 7200x4400 code words, the format of the user-installed IMBE plug-in (no codec is part of this program). The sync words, the bit schedule and the error
// control follow DSD / DSD-FME and mbelib (ISC licence, credited in THIRD_PARTY.md).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::provoice {

constexpr int kBlockBits = 768, kSyncBits = 32, kFrameCodeBits = 142, kInfoBits = 88;

// A sync word as payload bits (first bit sent first), standard and Extended Addressing (EA) variants; polarity is found by trying both these and their inverses.
constexpr uint32_t kSyncStd = 0, kSyncEa = 1;   // choices for makeBlock; the words are defined in provoice.cpp
extern const uint32_t kSyncStdValue, kSyncEaValue;

// A channel-coded IMBE 7100x4400 frame (7 x 24 bits, the layout of mbelib)
struct Frame7100 { char b[7][24]; };
// IMBE 7200x4400 code words, as P25 and the plug-in use them (8 x 23)
struct Frame7200 { char b[8][23]; };

// The 88 information bits (the order of mbelib's imbe_d, "7100 arrangement") into a channel-coded frame, and back
void encode7100(const uint8_t info[kInfoBits], Frame7100& out);
bool decode7100(Frame7100 in, uint8_t info[kInfoBits], int* errs = nullptr);          // false when the frame is beyond repair
void convert7100to7200(uint8_t info[kInfoBits]);                                      // rearranges in place
void convert7200to7100(uint8_t info[kInfoBits]);                                      // the inverse (the generator)
void encode7200(const uint8_t info[kInfoBits], Frame7200& out);                       // information bits (7200 arrangement) -> the channel coded code words

// The block of four frames as bits (the 32 sync bits first), payload-bit polarity (1 = lower frequency on a non-inverted channel)
std::vector<uint8_t> makeBlock(const Frame7100 frames[4], uint32_t sync = kSyncStd, uint16_t lid = 0);   // sync: kSyncStd, kSyncEa, or a 32 bit word
// 736 bits after the sync -> four frames
void splitBlock(const uint8_t bits736[736], Frame7100 frames[4], uint16_t* lid = nullptr);

} // namespace dect2::provoice
