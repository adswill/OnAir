// ATSC 1.0 (A/53, 8-VSB): constants, framing and the channel-coding chain (randomiser, Reed-Solomon (207,187), convolutional byte
// interleaver, 12-way interleaved trellis code). The transmit side is used by the test signal generator, the receive side by the
// receiver: Viterbi decoding of a whole data field, de-interleaving, Reed-Solomon correction and de-randomising.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc {

constexpr double kSymbolRate = 4.5e6 / 286.0 * 684.0;   // 10.762238 Msymbols/s
constexpr int kSegSyms = 832;        // symbols per data segment, 4 of them the segment sync
constexpr int kFieldSegs = 313;      // field sync segment + 312 data segments
constexpr int kDataSegs = 312;
constexpr int kSegBytes = 207;       // bytes per data segment after Reed-Solomon coding
constexpr int kTsBytes = 188;
constexpr int kFieldSyms = kFieldSegs * kSegSyms;
constexpr int kNumEnc = 12;          // interleaved trellis encoders
constexpr int kInterleave = 52;      // byte interleaver branches
constexpr int kDelaySegs = 52;       // end-to-end delay of interleaver + de-interleaver, in segments

extern const uint8_t kPn511[511];
extern const uint8_t kPn63[63];

inline float levelOf(int sym) { return (float)(2 * sym - 7); }   // symbol 0..7 -> -7..+7

// The 832 symbols (as 0..7) of a field sync segment. field2 toggles the middle PN63. `prev12` are the last 12 symbols of the
// preceding data segment (copied into the final 12 positions).
void fieldSyncSymbols(bool field2, const uint8_t prev12[12], uint8_t out[kSegSyms]);

// ---- energy dispersal (the LFSR is reset at the first data segment of every field)
class Randomizer {
public:
    Randomizer() { reset(); }
    void reset() { state_ = 0x018f; }
    void apply(uint8_t* data, size_t n);   // XOR with the sequence (same operation to scramble and to descramble)
private:
    unsigned state_;
};

// ---- Reed-Solomon (207,187), GF(256) x^8+x^4+x^3+x^2+1, roots alpha^0..alpha^19 (a (255,235) code shortened by 48)
void rsEncode(const uint8_t in187[187], uint8_t out207[207]);
int rsDecode(uint8_t blk207[207]);   // corrects in place; returns the number of corrected bytes or -1

// ---- convolutional byte interleaver, 52 branches of 4-byte steps. The pair interleaver + de-interleaver (with the
// alignment delay of 156 bytes) delays the stream by exactly 52 segments.
class ByteInterleaver {
public:
    explicit ByteInterleaver(bool inverse);
    void reset();                          // flush all FIFOs and restart the commutator
    void syncCommutator() { comm_ = 0; }   // at the first data segment of every field
    void process(const uint8_t* in, uint8_t* out, size_t n);
private:
    struct Fifo { std::vector<uint8_t> v; size_t pos = 0; uint8_t stuff(uint8_t b) { if (v.empty()) return b; const uint8_t r = v[pos]; v[pos] = b; if (++pos >= v.size()) pos = 0; return r; } };
    bool inverse_;
    int comm_ = 0;
    std::vector<Fifo> fifo_;
    Fifo align_;
};

// ---- trellis code. One encoder: 2 bits in, 3 bits (an 8-level symbol 0..7) out, 8 states (including the pre-coder bit).
struct TrellisTables {
    uint8_t next[32];
    uint8_t out[32];
};
const TrellisTables& trellis();

// Where each trellis symbol of a 12-segment block comes from: for every encoder the positions of its symbols inside the block
// (counting all 832 symbols of each segment) and the byte index / bit shift of the dibit it carries.
struct TrellisMap {
    int symPos[kNumEnc][828 * 1];      // symbol position inside the 12-segment block (9984 symbols)
    int byteIdx[kNumEnc][828];         // byte index inside the block's 12 * 207 bytes
    int shift[kNumEnc][828];           // 6, 4, 2 or 0
};
const TrellisMap& trellisMap();

// ---- one data field (transmit): 312 transport packets (188 bytes, sync byte first) -> 313 * 832 symbols (0..7), field sync first.
class FieldEncoder {
public:
    FieldEncoder();
    // `field2` selects the polarity of the field sync pattern
    void encode(const uint8_t* ts312x188, bool field2, uint8_t* symbols /* kFieldSyms */);
private:
    ByteInterleaver il_{false};
    uint8_t encState_[kNumEnc];
    uint8_t last12_[12];
};

// ---- one data field (receive): soft symbol levels in -> transport packets out
struct FieldStats {
    int segments = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0;
    int bytesCorrected = 0;
    float viterbiMetric = 0;    // average squared distance per symbol to the decoded path
};

class FieldDecoder {
public:
    FieldDecoder();
    void reset();   // after losing field sync: flush the interleaver state
    // `levels`: kFieldSyms soft values around -7..+7 (the field sync segment is skipped). Writes up to 312 packets (188 bytes,
    // 0x47 first) to `out`; the transport_error_indicator is set on uncorrectable packets. Returns the number of packets written
    // (0 while the de-interleaver is still filling after reset()).
    // `symbolsOut` (optional, kFieldSyms entries): the symbols (0..7) of the decoded trellis path for the data segments; positions of
    // sync segments are left untouched.
    int decode(const float* levels, uint8_t* out, FieldStats* st, uint8_t* symbolsOut = nullptr);
private:
    ByteInterleaver dil_{true};
    Randomizer rnd_;
    int warm_ = kDelaySegs;
    int outIdx_ = 0;
};

} // namespace atsc
} // namespace dect2
