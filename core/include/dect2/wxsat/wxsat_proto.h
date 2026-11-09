// Layers above the demodulator for the wxsat links, in both directions so the test signal is built with the code that checks it.
//
// Frame formats, sync words, offsets, the GAC scrambler and the instrument layouts (AVHRR GAC, HIRS, MSU-MR on HRPT, MSU-GS, MWR) are taken from
// SatDump's plugins noaa_metop_support, meteor_support, elektro_arktika_support and aws_support.
// Copyright (c) Aang23 and contributors, GPL-3.0. Written for OnAir; see THIRD_PARTY.md.
#pragma once
#include "wxsat_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2::wxsat {

constexpr int kGacBits = 33270, kGacBytes = 4159, kGacWords = 3327, kGacImageWord = 1182, kGacWidth = 409;
constexpr uint32_t kGacSync = 0xA116FD71u;
constexpr int kDsbBytes = 104;
constexpr uint16_t kDsbSync = 0xEDE2;
constexpr int kCadu = 1024;
constexpr uint64_t kMsuSync = 0x0218a7a392dd9abfULL;
constexpr int kMsuMrFrame = 11850, kMsuMrWidth = 1572, kMsuMrChannels = 6;
constexpr int kMsuGsIrFrame = 1820, kMsuGsVisFrame = 15210, kMsuGsIrWidth = 183 * 18, kMsuGsIrChannels = 7, kMsuGsIrLines = 3400;
constexpr int kMsuGsVisFullWidth = 6004, kMsuGsVisLines = 17200, kMsuGsVisDecim = 4, kMsuGsVisWidth = kMsuGsVisFullWidth / kMsuGsVisDecim;
constexpr int kAwsCadu = 1279, kAwsCoded = 1275, kAwsVcdu = 1115, kAwsInterleave = 5, kMwrWidth = 145, kMwrChannels = 19, kMwrLineBytes = 7599;
constexpr int kAwsScid = 0x0C2;     // what the test signal sends
constexpr int kHirsWidth = 56, kHirsChannels = 20;

// The GAC scrambler: 1023 bits a period (the last one zero, as SatDump's table has it)
int gacPn(int i);
// Scramble / descramble a whole frame from bit 60 on
void gacScramble(uint8_t* frame);
void unpack10(const uint8_t* in, int words, uint16_t* w);
void pack10(const uint16_t* w, int words, uint8_t* out);

// Bits -> fixed length frames with a sync word of up to 64 bits, either polarity. The frame handed out starts with the sync word
// and has frameBits bits, MSB first, zero padded to a byte (GAC).
class Deframer {
public:
    std::function<void(const uint8_t* frame)> onFrame;
    void configure(uint64_t sync, int syncBits, int frameBits, int tolAcq, int tolTrack, bool allowInverted = true);
    void reset();
    void push(int bit);
    bool locked() const { return lock_; }
    bool inverted() const { return inv_; }
    uint64_t hits = 0;
private:
    uint64_t sync_ = 0, mask_ = 0, sr_ = 0;
    int syncBits_ = 0, frameBits_ = 0, tolAcq_ = 0, tolTrack_ = 0;
    bool allowInv_ = true;
    bool lock_ = false, inv_ = false;
    int pos_ = 0, misses_ = 0;
    std::vector<uint8_t> frame_;
    void start();
};

// TIP minor frame -> HIRS
struct HirsDecoder {
    int line = 0, last = -1, auxCounter = 0;
    bool haveAny = false;
    std::vector<uint16_t> cur = std::vector<uint16_t>((size_t)kHirsChannels * kHirsWidth, 0);
    // returns true when a finished line went to out (channel major)
    bool push(const uint8_t tip[kDsbBytes], std::vector<uint16_t>& out, int& outLine);
};
void encodeHirsTip(uint8_t tip[kDsbBytes], int scid, int minor, int element, const uint16_t words[kHirsChannels]);
uint16_t wxTestPixel(int c, int x, int y);       // 10-bit test picture

// NOAA minor frames: the TIP blocks in them. hrpt: only on frames whose number is 1; gac: blocks 1-5 of every frame
void noaaTipBlocks(const uint16_t* words, bool gac, const std::function<void(const uint8_t*)>& out);
int noaaScid(const uint16_t* words);
const char* noaaSatName(int scid);

// GAC frame
void gacDecodeLine(const uint16_t* words, std::vector<uint16_t>& px);   // px: 5 x 409 channel major
void gacBuildFrame(int line, int scid, uint8_t out[kGacBytes]);

// Meteor HRPT CADU <-> MSU-MR stream: 948 payload bytes a CADU
constexpr int kMeteorPayload = 948;
void meteorPayload(const uint8_t cadu[kCadu], uint8_t out[kMeteorPayload]);
void meteorBuildCadu(const uint8_t payload[kMeteorPayload], uint32_t counter, uint8_t cadu[kCadu]);
void msuMrDecodeFrame(const uint8_t* f, std::vector<uint16_t>& px);     // 6 x 1572
void msuMrBuildFrame(int line, std::vector<uint8_t>& f);

// RDAS CADU: 1000 payload bytes from offset 24, the virtual channel in byte 5 or 11
constexpr int kRdasPayload = 1000;
int rdasVcid(const uint8_t cadu[kCadu]);
void rdasBuildCadu(int vcid, uint32_t counter, const uint8_t payload[kRdasPayload], uint8_t cadu[kCadu]);
struct MsuGsIr { int counter = 0, segment = 0; bool valid = false; std::vector<uint16_t> seg = std::vector<uint16_t>(kMsuGsIrChannels * 183); };
void msuGsIrDecode(const uint8_t* f, MsuGsIr& out);
void msuGsIrBuild(int counter, int segment, std::vector<uint8_t>& f);
int msuGsVisDecode(const uint8_t* f, std::vector<uint16_t>& a, std::vector<uint16_t>& b);   // the counter, -1 bad; a, b 6004 each
void msuGsVisBuild(int counter, std::vector<uint8_t>& f);

// AWS: RS(255,223) x 5
int awsDecodeCadu(const uint8_t coded[kAwsCoded], uint8_t vcdu[kAwsVcdu]);   // errors corrected, -1 failed
void awsEncodeCadu(const uint8_t vcdu[kAwsVcdu], uint8_t cadu[kAwsCadu]);
// VCDU -> space packets (1109 data bytes a frame, first header pointer in the first two)
struct AwsPacket { int apid = 0, seqFlag = 0; std::vector<uint8_t> data; };
class AwsDemux {
public:
    std::function<void(const AwsPacket&)> onPacket;
    void reset() { buf_.clear(); have_ = false; lastCounter_ = -1; }
    void push(const uint8_t vcdu[kAwsVcdu]);
    uint64_t packets = 0, dropped = 0;
private:
    void take(const uint8_t* p, size_t n);
    std::vector<uint8_t> buf_;
    bool have_ = false;
    int64_t lastCounter_ = -1;
};
struct MwrDecoder {
    std::vector<uint8_t> wip;
    int lines = 0;
    bool push(const AwsPacket& p, std::vector<uint16_t>& px);  // true: a finished line, 19 x 145 channel major
};
// one MWR scan cut into space packets (flags 01, 00, 10) as bytes of the packet stream
void mwrBuildPackets(int line, int vcid, int& seq, std::vector<uint8_t>& stream);
void awsBuildVcdu(int vcid, uint32_t counter, std::vector<uint8_t>& stream, std::vector<uint8_t>& starts, uint8_t vcdu[kAwsVcdu]);

} // namespace dect2::wxsat
