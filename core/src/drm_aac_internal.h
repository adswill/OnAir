// DRM audio: AAC tables and the frame model shared by drm_aac.cpp (decoder side) and drm_aacenc.cpp (the test signal's encoder). Private.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 { namespace drm { namespace aac {

// Tables of drm_aactab.cpp: Huffman codes (value, length in bits) of the scale factors (121 symbols) and of the spectral codebooks 1 to 11, and the scale
// factor band offsets of the 960 transform (long windows: 47 and 43 entries for 24 and 12 kHz; the eight short windows: 16 entries)
extern const uint32_t kScaleCode[121];
extern const uint8_t kScaleBits[121];
extern const uint16_t kSpecCode1[81], kSpecCode2[81], kSpecCode3[81], kSpecCode4[81], kSpecCode5[81], kSpecCode6[81], kSpecCode7[64], kSpecCode8[64], kSpecCode9[169], kSpecCode10[169], kSpecCode11[289];
extern const uint8_t kSpecBits1[81], kSpecBits2[81], kSpecBits3[81], kSpecBits4[81], kSpecBits5[81], kSpecBits6[81], kSpecBits7[64], kSpecBits8[64], kSpecBits9[169], kSpecBits10[169], kSpecBits11[289];
extern const uint16_t kSwbOffset96024[47], kSwbOffset96016[43], kSwbOffset12024[16], kSwbOffset12016[16];

constexpr int kFrame = 960;

// Codeword reordering (HCR): the longest code word of each codebook (index = codebook, 16 to 31 are the virtual codebooks of VCB11), and the sorted list of
// code words. A code word is a pair or a quad of spectral values: its codebook and the position of its first value in the spectrum (groups of short windows
// one after the other).
extern const int kMaxCwLen[32];
struct HcrCw { int cb, sp; };
void hcrSortedList(int numGroups, const int* groupLen, int maxSfb, const int (*sfbCb)[64], const uint16_t* swb, int frameLines, std::vector<HcrCw>& out);

// Spectral codebook cb (1 to 11): tuple size, signed or not, largest value, number of symbols
struct CbInfo { int dim; bool isSigned; int lav; int size; const uint16_t* code; const uint8_t* bits; };
const CbInfo& cbInfo(int cb);
// Values of symbol `idx` of codebook cb (first value most significant), and the other way round
void cbValues(int cb, int idx, int* v);
int cbIndex(int cb, const int* v);          // v within the codebook's range (for cb 11 the escape marker 16 stands for |v| >= 16)

// Prefix code reader: finds the symbol of the next code word in a bit sequence of 0/1 values. Returns the symbol, -1 when the bits run out, -2 when no code matches.
class PrefixTable {
public:
    void build(const uint32_t* code, const uint8_t* bits, int n);
    void build16(const uint16_t* code, const uint8_t* bits, int n);
    int find(const uint8_t* b, int n, int* used) const;
private:
    struct Node { int child[2] = {-1, -1}; int sym = -1; };
    std::vector<Node> nodes_;
};
const PrefixTable& scaleTable();
const PrefixTable& specTable(int cb);

// The scale factor band offsets for a core rate (12000 or 24000): numSfb + 1 entries
const uint16_t* swbLong(int rateHz, int& numSfb);
const uint16_t* swbShort(int rateHz, int& numSfb);

}}} // namespace dect2::drm::aac
