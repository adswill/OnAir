// FY-3 (Fengyun-3 polar orbit) AHRPT and MPT, the layers above the demodulator (both directions: the test signal is built with the
// same code it is checked by).
//
// Channel coding, as the FY-3 downlinks have it and as the open decoders (SatDump's fengyun3 plug-in, GPL-3) apply it: the two QPSK rails
// each carry their own CCSDS convolutional code (K = 7, r = 1/2, G1 = 171, G2 = 133 octal); the decoded rail bits (x, y) go through a
// differential decoder that makes the whole chain immune to the QPSK phase ambiguity (see DiffDecoder); the bits that come out are
// CADUs of 1024 bytes (sync word 0x1ACFFC1D, CCSDS randomiser, Reed-Solomon (255,223) interleave 4): meteor_proto.h's layers.
//
// Instruments ride on the virtual channels of the VCDUs (AOS, 892 bytes: 6 bytes header, 4 bytes more, 882 bytes of data):
//   VIRR (FY-3A / B / C, virtual channel 5): frames of 26050 bytes on a 60-bit sync word, 10 channels x 2048 pixels of 10 bits, a time stamp.
//   MERSI (virtual channel 3, MPT of FY-3A / B / C; AHRPT of FY-3D and later): a bit stream of a head frame (48-bit sync 0x55AA55AA55AA, time
//     stamp, calibration) and scan lines (28-bit sync): 250 m channels of 8192 pixels (40 lines in a scan), 1 km channels of 2048 (10 lines),
//     12 bits a pixel. The other instruments (MWRI, MWHS, MWTS, GAS, ...) are named and counted.
#pragma once
#include "dect2/fy3/fy3_tel.h"
#include "dect2/hrpt/hrpt_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2::fy3 {

// --- satellites ----------------------------------------------------------------------------------------------------------------------
struct Sat { const char* name; int scid; int norad; };
const Sat* sats(int& n);                  // FY-3A .. FY-3H
int satIndexByScid(int scid);             // -1 unknown

// --- instruments -----------------------------------------------------------------------------------------------------------------------
enum Inst { InstNone = 0, Virr, Mersi1, Mersi2, MersiLL, Mersi3, MersiRM, Mwri, Mwri2, MwriRM, Mwhs, Mwts, Erm, Gas, Waai, Xeuvi, WindRad, Pmr, InstCount };
const char* instName(int inst);           // "VIRR", "MERSI-II", ...
bool instIsImage(int inst);               // a picture is built for it
// The instrument a virtual channel carries on a satellite (scid), InstNone when not known. vcid 12 carries several (by APID).
int instForVcid(int scid, int vcid, int product);
int instChannels(int inst);               // channels of the picture
int instWidth(int inst);                  // pixels of a row of the picture
const char* instChannelName(int inst, int ch);   // "1 (0.47 um)" ...

// --- differential coding and the rail codes --------------------------------------------------------------------------------------------
// Decoder (the receiver's side): the rail bits (x, y) of this step give the data pair (hi, lo). The bit order of the output stream is hi first.
struct DiffDecoder {
    int xp = 0, yp = 0;
    void push(int x, int y, int& hi, int& lo) {
        if ((x ^ y) == 1) { hi = y ^ yp; lo = x ^ xp; } else { hi = x ^ xp; lo = y ^ yp; }
        xp = x; yp = y;
    }
    void reset() { xp = yp = 0; }
};
// Encoder (the test signal's side): the data pair (hi, lo) gives the rail bits (x, y) that DiffDecoder turns back into it.
struct DiffEncoder {
    int xp = 0, yp = 0;
    void push(int hi, int lo, int& x, int& y) {
        if (((xp ^ yp ^ hi ^ lo) & 1) == 0) { x = xp ^ hi; y = yp ^ lo; } else { y = yp ^ hi; x = xp ^ lo; }
        xp = x; yp = y;
    }
};

// One rail's K = 7 soft decoder. Same code and the same arithmetic as hrpt::Viterbi, and the sum of the path metric gains kept, so the
// receiver can tell which pairing / polarity of the symbols is the right one (a wrong one decodes noise: its metric stays far lower).
class RailViterbi {
public:
    RailViterbi() { reset(); }
    void reset();
    void push(int a, int b);                            // one pair of soft code symbols (> 0: a 1)
    std::vector<uint8_t>& out() { return out_; }        // decided bits, oldest first, about 96 late
    void flush();
    double score() const { return (double)acc_ + (double)best(); }   // total correlation of the best path with everything pushed
    int steps() const { return stepsTotal_; }
private:
    int best() const;
    void traceback(int emit, bool all);
    alignas(16) int16_t m_[64];
    uint8_t dec_[160][64];
    int n_ = 0, sinceNorm_ = 0, stepsTotal_ = 0;
    int64_t acc_ = 0;
    std::vector<uint8_t> out_;
};

// The 8 symmetries of a soft pair (rotations by 90 degrees and mirrored), see hrpt_rx.cpp xform
inline void pairXform(int t, int a, int b, int& oa, int& ob) {
    if (t & 4) b = -b;
    switch (t & 3) {
    case 0: oa = a; ob = b; break;
    case 1: oa = -b; ob = a; break;
    case 2: oa = -a; ob = -b; break;
    default: oa = b; ob = -a; break;
    }
}

// --- frames -----------------------------------------------------------------------------------------------------------------------------
struct Cadu {
    int scid = 0, vcid = 0, fhp = 0x7FF;
    uint32_t counter = 0;
    uint8_t data[882];
    int rsErrors = 0;
};
// The 1020 bytes after the sync word: derandomise, Reed-Solomon. false: a codeword could not be corrected.
bool decodeCadu(const uint8_t coded[1020], Cadu& out);
// The AOS header of an already decoded 892 byte VCDU (the first-header-pointer field is not checked: instrument virtual channels carry bit streams)
void parseVcdu(const uint8_t v[892], Cadu& out);
void encodeVcdu(const Cadu& c, uint8_t cadu[1024]);   // ASM, RS, randomised

// --- VIRR ---------------------------------------------------------------------------------------------------------------------------------
constexpr int kVirrFrameBytes = 26050, kVirrChannels = 10, kVirrWidth = 2048;
constexpr int kVirrWordBit = 436 * 8 + 2;      // first pixel word, in bits from the first bit of the sync word
constexpr double kVirrLineMs = 1000.0 / 6.0;
constexpr uint64_t kVirrSync = 0b101000010001011011111101011100011001110110000011110010010101ull;   // the 60-bit sync word
struct VirrLine {
    uint32_t days = 0, ms = 0;
    std::vector<uint16_t> px;                 // kVirrChannels * kVirrWidth, channel major, 10 bit
    VirrLine() : px((size_t)kVirrChannels * kVirrWidth, 0) {}
};
bool decodeVirrFrame(const uint8_t* f, size_t n, VirrLine& out);
void encodeVirrFrame(const VirrLine& l, uint8_t* f);   // kVirrFrameBytes bytes

// --- a bit stream cut into frames (a sync word, then a fixed length) ---------------------------------------------------------------------
class FrameSync {
public:
    FrameSync(uint64_t sync, int syncBits, int frameBits) : sync_(sync), syncBits_(syncBits), frameBits_(frameBits), mask_(syncBits >= 64 ? ~0ull : (1ull << syncBits) - 1) {}
    // Feeds bytes; each complete frame (the sync word's bits first, frameBits long) goes to the callback
    void push(const uint8_t* d, size_t n, const std::function<void(const std::vector<uint8_t>&)>& cb);
    void gap(size_t bytes);                   // lost data: zeros stand in for it, so a frame in progress stays the right length
    void reset() { in_ = false; shift_ = 0; cur_.clear(); nbits_ = 0; }
private:
    uint64_t sync_;
    int syncBits_, frameBits_;
    uint64_t mask_, shift_ = 0;
    bool in_ = false;
    std::vector<uint8_t> cur_;
    int nbits_ = 0;
    uint8_t acc_ = 0;
};

// --- MERSI ----------------------------------------------------------------------------------------------------------------------------------
struct MersiCfg {
    int ch250, ch1000, width250;
    int headBits, scan250Bits, scan1000Bits;
    int imgByte, imgBit;                      // pixel data starts at this byte / bit of a scan frame (after its sync word)
    double msScale; bool time3g;
};
const MersiCfg& mersiCfg(int inst);           // Mersi1, Mersi2, MersiLL, Mersi3, MersiRM
// The picture of an instrument is on the 1 km grid: the 250 m channels are averaged 4 x 4. A row group (a scan, 10 rows) arrives complete.
struct MersiSegment {
    int number = 0;                           // counted from the first scan heard
    double time = 0;                          // seconds since 1970 of the head frame (the on-board clock; 0 when unreadable)
    int lines = 0;                            // lines of the 40 + 10 that arrived
    std::vector<uint8_t> px;                  // 10 rows x (ch250 + ch1000) channels x 2048, row major; 8 bit
};
class MersiReader {
public:
    explicit MersiReader(int inst);
    void push(const uint8_t* d, size_t n);    // payload bytes of the virtual channel (882 a VCDU)
    void gap(size_t bytes);
    void flush();                             // emit the segment in progress
    std::function<void(const MersiSegment&)> onSegment;
    int channels() const { return cfg_.ch250 + cfg_.ch1000; }
    uint64_t segments = 0, linesTotal = 0;
private:
    void frameDone();
    void processHead();
    void processScan();
    void emit();
    MersiCfg cfg_;
    uint64_t shHead_ = 0;
    uint32_t shScan_ = 0;
    std::vector<uint8_t> cur_;
    int curBits_ = 0, curSize_ = -1;
    uint8_t byteAcc_ = 0;
    bool inFrame_ = false, wasHead_ = false;
    double lastTime_ = 0, firstTime_ = 0;
    bool haveFirst_ = false;
    int segNo_ = -1, lastEmitted_ = -1, counterSeg_ = -1;
    bool segOpen_ = false;
    std::vector<uint32_t> acc_;               // 250 m channels summed: [ch][10][2048]
    std::vector<uint8_t> cnt_;
    std::vector<uint8_t> km_;                 // 1 km channels: [ch][10][2048]
    int linesIn_ = 0;
    double segTime_ = 0;
};
// Bits appended MSB first (the test signals' instrument streams are not byte aligned)
struct BitWriter {
    std::vector<uint8_t> bytes;
    int nbit = 0;                             // bits in the byte being filled
    uint8_t acc = 0;
    void put(uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) { acc = (uint8_t)(acc << 1 | (v >> i & 1)); if (++nbit == 8) { bytes.push_back(acc); nbit = 0; acc = 0; } } }
    size_t size() const { return bytes.size(); }
};
// A scan for the test signal: a head frame and the lines of every channel (pixel(ch, x, y) is the 12-bit value at the 250 m or the 1 km grid
// position of that channel; y counted in lines of that grid from the top of the picture).
void encodeMersiSegment(int inst, int segment, double time, const std::function<uint16_t(int ch, int x, int y)>& pixel, BitWriter& out);

// --- test pictures ----------------------------------------------------------------------------------------------------------------------------
uint16_t testPixel(int channel, int x, int y);   // 10 bit

} // namespace dect2::fy3
