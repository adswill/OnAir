// DVB-C2 (ETSI EN 302 769 V1.3.1): the parts of the standard shared by the generator and the receiver.
//  - the OFDM numbers (4K FFT, the 8 MHz raster's 64/7 Msample/s, guard intervals 1/64 and 1/128, the C2 frame of L_P preamble symbols
//    and 448 data symbols),
//  - the reference sequences (clause 9.2: the data scrambling PRBS w_k, the pilot PRBS w'_i and the differential pilot reference r_k),
//  - the pilot positions of the preamble (every 6th carrier) and of the data symbols (scattered, continual, edge; clause 9.6),
//  - the Reed-Muller (32,16) headers (clause 7.2.2: the FECFrame header of Data Slice Type 2 and the preamble header of clause 8.2),
//  - the L1 signalling part 2 fields (clause 8.3) and their coding (clause 8.4: the shortened and punctured 16K LDPC that DVB-T2's
//    L1-post uses, from t2l1.h),
//  - the bit-interleaved coded modulation of the PLPs (clause 6: BCH, LDPC, bit interleaver, 16 / 64 / 256-QAM; the LDPC tables are
//    DVB-T2's for 2/3 .. 5/6 and DVB-S2's for 8/9 and 9/10, the code annexes A and B list the same addresses),
//  - the data slice time interleaver (clause 9.4.4) and the 4K frequency interleaver (clause 9.4.5, DVB-T2's 4K address generator).
// Carrier indices are absolute (multiples of the carrier spacing from 0 Hz), as in the standard: the pilots, the scrambling and the
// L1 block structure all depend on the absolute index. 1024-QAM and 4096-QAM, GSE payloads and reserved tones are not implemented.
// Nothing here has met a real DVB-C2 network: generator and receiver were checked against each other only (docs/DVBC2-3.9.md).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/t2/t2fec.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace dvbc2 {

// ---------------------------------------------------------------- OFDM (clause 10, table 38; 8 MHz raster)
constexpr int kFft = 4096;
constexpr double kSampleRate = 64e6 / 7;                     // elementary period T = 7/64 us
constexpr double kCarrierSpacingHz = kSampleRate / kFft;    // 2 232.14 Hz, T_U = 448 us
constexpr int kL1Carriers = 3408;                           // K_L1: carriers of one L1 signalling block (7.61 MHz)
constexpr int kL1DataCells = 2840;                          // N_L1: the data cells of an L1 block (3408 - 568 pilots)
constexpr int kPreamblePilotSpacing = 6;                    // D_P
constexpr int kDataSymbols = 448;                           // L_data (C2_FRAME_LENGTH)
constexpr int kDy = 4;                                      // scattered pilot pattern: symbols per sequence
constexpr int kPreambleHeaderCells = 32;
constexpr int kMaxL1InfoPerBlock = 4759;                    // N_L1part2_max_per_Symbol
constexpr int kMaxL1Bits = 32766;
constexpr float kPilotAmp = 7.f / 3.f;                      // scattered, continual and edge pilots
inline int guardSamples(int gi) { return gi == 1 ? 64 : 32; }   // GUARD_INTERVAL 0 = 1/128, 1 = 1/64
inline int symbolSamples(int gi) { return kFft + guardSamples(gi); }
inline int dx(int gi) { return gi == 1 ? 12 : 24; }             // D_X: separation of the pilot bearing carriers
inline float preamblePilotAmp(int gi) { return gi == 1 ? 4.f / 3.f : 6.f / 5.f; }   // A_PP
inline double symbolSeconds(int gi) { return symbolSamples(gi) / kSampleRate; }

// ---------------------------------------------------------------- reference sequences (clause 9.2)
uint8_t dataScramble(int64_t k);        // w_k: PRBS X^11 + X^2 + 1 (period 2047), k the absolute carrier index
uint8_t pilotPrbs(int i);               // w'_i: PRBS X^10 + X^3 + 1 (period 1023)
// r_k of clause 9.2.3 for a pilot bearing carrier (k multiple of 6): differential within the L1 block that holds k.
// (Walks the block from its start: cache the result per block when calling it often, see ReferenceBlock.)
uint8_t referenceBit(int64_t k);
// r_k for every k = 6 j of one L1 block, block = k div 3408
class ReferenceBlock {
public:
    void set(int64_t block);
    int64_t block() const { return block_; }
    uint8_t bit(int64_t k) const;       // k multiple of 6 inside the block
    float value(int64_t k, float amp) const { return amp * (1.f - 2.f * bit(k)); }   // A (1 - 2 r_k)
private:
    int64_t block_ = -1;
    std::array<uint8_t, kL1Carriers / kPreamblePilotSpacing> r_{};
};
// A (1 - 2 r_k) for any pilot bearing k (uncached)
float pilotValue(int64_t k, float amp);

// ---------------------------------------------------------------- pilots of the data symbols (clause 9.6)
bool isContinualPilot(int64_t k);                            // table 35 (k mod 3408)
inline bool isScatteredPilot(int64_t k, int ld, int gi) {    // clause 9.6.2.1: k mod (D_X D_Y) = D_X (l_d mod D_Y)
    const int d = dx(gi);
    return ((k % (d * kDy)) + d * kDy) % (d * kDy) == d * (ld % kDy);
}
extern const int kContinualPilots[30];

// ---------------------------------------------------------------- Reed-Muller (32,16) headers (clause 7.2.2)
uint32_t rmEncode(uint16_t info);                            // bit i of the result = codeword bit lambda_i
// Soft decoder: llr[i] > 0 means lambda_i = 0. Returns the best information word; `metric` the correlation (32 = perfect).
uint16_t rmDecode(const float* llr, float* metric = nullptr);
// The header's cells: 32 QPSK cells (robust) or 16 16QAM cells (high efficiency mode)
void encodeHeader(uint16_t info, bool hem, std::vector<cf32>& cells);
// From equalised cells (32 or 16) with their noise variance per real dimension; returns the information word, `metric` as rmDecode
uint16_t decodeHeader(const cf32* cells, const float* n0, bool hem, float* metric = nullptr);
inline int headerCells(bool hem) { return hem ? 16 : 32; }
// A cheap test for a header at these cells: the correlation (0 .. 1) of the upper branch bits with the shifted and descrambled lower
// branch, which only a header has. Above about 0.5 it is worth running decodeHeader.
float headerCorrelation(const cf32* cells, bool hem);

// FECFrame header fields (clause 7.2.1), MSB first: PLP_ID 8, PLP_FEC_TYPE 1, PLP_MOD 3, PLP_COD 3, HEADER_COUNTER 1
struct FecFrameHeader {
    int plpId = 0, fecType = 1, mod = 3, cod = 2, counter = 0;
    uint16_t pack() const { return (uint16_t)((plpId & 255) << 8 | (fecType & 1) << 7 | (mod & 7) << 4 | (cod & 7) << 1 | (counter & 1)); }
    static FecFrameHeader unpack(uint16_t v) { FecFrameHeader h; h.plpId = v >> 8; h.fecType = (v >> 7) & 1; h.mod = (v >> 4) & 7; h.cod = (v >> 1) & 7; h.counter = v & 1; return h; }
};
// Preamble header (clause 8.2): L1_INFO_SIZE 14, L1_TI_MODE 2
struct PreambleHeader {
    int l1InfoSize = 0, tiMode = 0;
    uint16_t pack() const { return (uint16_t)((l1InfoSize & 0x3FFF) << 2 | (tiMode & 3)); }
    static PreambleHeader unpack(uint16_t v) { PreambleHeader h; h.l1InfoSize = v >> 2; h.tiMode = v & 3; return h; }
};

// ---------------------------------------------------------------- FEC of the PLPs (clause 6)
// PLP_MOD 1 16QAM, 2 64QAM, 3 256QAM (4 1024QAM and 5 4096QAM are not implemented); PLP_COD 1 2/3, 2 3/4, 3 4/5, 4 5/6, 5 8/9 (16K) / 9/10 (64K)
struct Fec {
    int fecType = 1;    // PLP_FEC_TYPE: 0 = 16 200 bits, 1 = 64 800
    int mod = 3;
    int cod = 2;
};
struct Dims {
    bool ok = false;
    int nLdpc = 0, kLdpc = 0, kBch = 0, t = 0, q = 0;
    int bitsPerCell = 0, cells = 0;   // cells of one XFECFrame (table 16(b))
};
Dims dims(const Fec& f);
bool modCodValid(const Fec& f);       // tables 11(a) and 11(b)
const char* modName(int mod);
const char* codName(int fecType, int cod);
int xfecFrameCells(int fecType, int mod);   // table 16(b): 0 for the reserved values (900 for PLP_MOD 0, the stuffing packets)
// The bit interleaver (clause 6.1.3) with the demultiplexer (6.2.1) folded in: map[p] = codeword bit carried by label bit p
// (p = cell * bitsPerCell + position, position 0 = the MSB of the cell label)
const std::vector<int>& bitMap(const Fec& f);
// One FEC block: `bb` holds kBch bits (one per byte, plain: the BB scrambler is applied here) -> cells of the XFECFrame
void encodeBlock(const Fec& f, const std::vector<uint8_t>& bb, std::vector<cf32>& cells);
// Equalised cells with their noise variance per real dimension -> the kBch bits of the BB frame (descrambled). false: not decodable.
struct BlockResult { bool ok = false; int ldpcIters = 0, bchCorrected = 0; double preBer = 0; };
BlockResult decodeBlock(const Fec& f, const cf32* cells, const float* n0, std::vector<uint8_t>& bb, int maxIter = 50);
double blockSeconds(const Fec& f, int dataCellsPerSymbol, int gi);   // how long one XFECFrame takes in a slice with that many cells per symbol
double netBitrate(const Fec& f, double cellsPerSecond);              // transport stream bit/s (normal mode BB frames)

// ---------------------------------------------------------------- L1 signalling part 2 (clause 8.3)
struct L1Plp {
    int id = 0, bundled = 0, type = 2, payloadType = 3, groupId = 0;
    int start = 0;              // PLP_START (Data Slice Type 1 only): the first complete XFECFrame of the frame, in the slice's data cell stream
    int fecType = 1, mod = 3, cod = 2;   // Data Slice Type 1 only
    int psiSiReprocessing = 0, tsId = 0, onId = 0, reserved1 = 0;
};
struct L1DataSlice {
    int id = 0;
    int tunePos = 0;            // DSLICE_TUNE_POS, in multiples of D_X from START_FREQUENCY
    int offsetLeft = 0, offsetRight = 0;   // signed, multiples of D_X from the tuning position
    int tiDepth = 0;            // 0 none, 1 four symbols, 2 eight, 3 sixteen
    int type = 0;               // 0 Type 1, 1 Type 2
    int fecHeaderType = 0;      // Type 2: 0 robust (QPSK), 1 high efficiency (16QAM)
    int constConf = 0, leftNotch = 0;
    std::vector<L1Plp> plps;
    int reserved2 = 0;
};
struct L1Notch { int start = 0, width = 0, reserved3 = 0; };
struct L1Part2 {
    int networkId = 0, systemId = 0;
    int64_t startFrequency = 0; // 24 bits, absolute carrier index of K_min (multiple of D_X)
    int bandwidth = 0;          // C2_BANDWIDTH: the system spans bandwidth * D_X + 1 carriers
    int guardInterval = 1;      // 0 = 1/128, 1 = 1/64
    int frameLength = kDataSymbols;
    int changeCounter = 0;
    std::vector<L1DataSlice> slices;
    std::vector<L1Notch> notches;
    int reservedTone = 0, ews = 0, version = 1, reserved4 = 0;
    bool crcOk = false;
    int64_t kMin() const { return startFrequency; }
    int64_t kMax() const { return startFrequency + (int64_t)bandwidth * dx(guardInterval); }
    int64_t sliceKMin(const L1DataSlice& s) const { return startFrequency + (int64_t)(s.tunePos + s.offsetLeft) * dx(guardInterval); }
    int64_t sliceKMax(const L1DataSlice& s) const { return startFrequency + (int64_t)(s.tunePos + s.offsetRight) * dx(guardInterval) - 1; }
    int64_t sliceTuneCarrier(const L1DataSlice& s) const { return startFrequency + (int64_t)s.tunePos * dx(guardInterval); }
    int64_t notchKMin(const L1Notch& n) const { return startFrequency + (int64_t)n.start * dx(guardInterval) + 1; }
    int64_t notchKMax(const L1Notch& n) const { return startFrequency + (int64_t)(n.start + n.width) * dx(guardInterval) - 1; }
};
void packL1Part2(const L1Part2& l1, std::vector<uint8_t>& bits);        // the fields only (no padding, no CRC)
bool unpackL1Part2(const std::vector<uint8_t>& bits, L1Part2& l1);      // the fields (the caller has checked the CRC)
std::string l1Summary(const L1Part2& l1);

// The coded L1 of one C2 frame (clause 8.4): the preamble header and the cells of the N_L1part2_FEC_Block blocks after time interleaving,
// one row per preamble symbol (without the header and before the cyclic repetition over the 2 840 cells of the L1 block)
struct L1Coded {
    PreambleHeader header;
    int fecBlocks = 0;              // N_L1part2_FEC_Block
    int preambleSymbols = 0;        // L_P (the TI depth when the L1 is time interleaved)
    int ksig = 0;                   // information bits per block
    int cellsPerBlock = 0;          // N_MOD_per_Block
    std::vector<std::vector<cf32>> rows;   // [preamble symbol][cell]
};
// tiMode: L1_TI_MODE 0 none, 1 best fit (N_FEC_Block symbols), 2 four symbols, 3 eight
bool encodeL1(const L1Part2& l1, int tiMode, L1Coded& out);
// Geometry from the header alone (what a receiver knows before decoding the blocks): false when the header is impossible
bool l1Geometry(const PreambleHeader& h, int& fecBlocks, int& preambleSymbols, int& ksig, int& cellsPerBlock);
// Decoder: rows[r] = the cells after the header of preamble symbol r (cellsPerBlock entries each, averaged over the repetitions), with the
// noise variance per real dimension n0[r][cell]. Returns true when the CRC-32 passes.
struct L1DecodeResult { bool ok = false, blocksOk = false; int ldpcIters = 0; };
L1DecodeResult decodeL1(const PreambleHeader& h, const std::vector<std::vector<cf32>>& rows, const std::vector<std::vector<float>>& n0, L1Part2& out);

// ---------------------------------------------------------------- the preamble (clause 9.3)
// Which carrier of the 3408 of an L1 block carries data cell q (q = 0 .. 2839): k mod 3408 for q, inverse of q = kk - ceil(kk / 6)
int preambleDataCarrier(int q);
inline int preambleCellIndex(int kk) { return kk - (kk + 5) / 6; }   // kk = k mod 3408, not a pilot
// The time interleaver of clause 9.4.4 (also the L1 one of clause 8.5): the write order of `depth` rows x `width` columns.
// order[i] = (row, column) for input cell i; the caller skips the positions that are not data cells.
void tiWriteOrder(int depth, int width, std::vector<std::pair<int, int>>& order);

// ---------------------------------------------------------------- data slice geometry (clause 9.4)
struct SliceCells {
    int64_t kMin = 0, kMax = 0;
    int gi = 1;
    std::vector<std::pair<int64_t, int64_t>> notches;   // [kMin, kMax] of each notch band inside the system
    int64_t sysKMin = 0, sysKMax = 0;                   // the system's edge pilots
    bool isNotch(int64_t k) const { for (auto& n : notches) if (k >= n.first && k <= n.second) return true; return false; }
    bool isEdgePilot(int64_t k) const {
        if (k == sysKMin || k == sysKMax) return true;
        for (auto& n : notches) if (k == n.first - 1 || k == n.second + 1) return true;
        return false;
    }
    bool isPilot(int64_t k, int ld) const { return isEdgePilot(k) || isContinualPilot(k) || isScatteredPilot(k, ld, gi); }
    bool isData(int64_t k, int ld) const { return !isNotch(k) && !isPilot(k, ld); }
    // the data carriers of data symbol ld, in increasing k
    void dataCarriers(int ld, std::vector<int64_t>& out) const;
    int width() const { return (int)(kMax - kMin + 1); }
};

} // namespace dvbc2
} // namespace dect2
