// TD-SCDMA (UTRA TDD 1.28 Mcps, the Chinese 3G system) base stations: the downlink pilot (DwPTS) and the time slot 0 midamble, from the
// 3GPP specifications TS 25.221 (frame and burst structure 5A.1, 5A.2.2, 5A.3.5; the basic midamble codes, Annex AA, with the binary to
// complex rule of 5A.2.3) and TS 25.223 (the SYNC-DL codes, Annex AA.1; their phase quadruples, 8.1.1; the mapping of code groups to cell
// parameters and their cycling with the SFN, Tables 7E and 7F).
//
// What it finds for every cell heard: the SYNC-DL code (= the code group, 0 - 31), the cell parameter (0 - 127: the scrambling code and the
// basic midamble, the cell's physical identity) from the time slot 0 midamble, the frame parity and the start of the P-CCPCH interleaving
// period from the SYNC-DL phase quadruples, the DwPTS level and the carrier offset. Receive only. The broadcast channel (P-CCPCH: the
// operator and the system information) is not decoded.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2::tdscdma {

constexpr double kChipRate = 1.28e6;
constexpr double kFs = 2 * kChipRate;             // working rate: two samples per chip
constexpr int kSubframeChips = 6400;              // 5 ms
constexpr int kTsChips = 864;                     // traffic time slot: 352 data + 144 midamble + 352 data + 16 guard
constexpr int kSyncDlChip = 864 + 32;             // SYNC-DL starts after time slot 0 and the DwPTS guard (TS 25.221 Figure 18I)
constexpr int kMidChip = 352;                     // time slot 0 midamble start

// The complex codes, chip i = 0 ...: SYNC-DL s_i (-j)^(i+1) (TS 25.223 8.1), midamble m_i j^(i+1) (TS 25.221 5A.2.3)
std::vector<cf32> syncDl(int code);               // 64 chips
std::vector<cf32> basicMidamble(int code);        // 128 chips
// The 144 chip midamble of shift k (1 .. K) of a basic code, K = 8 in time slot 0 (TS 25.221 5A.2.3: W = 128 / K)
std::vector<cf32> midamble(int code, int k, int K = 8);
const signed char* scramblingCode(int code);     // 16 chips, +1 / -1
// Cell parameter cycling (TS 25.223 Table 7F): the parameter used in a frame with odd SFN, given the one of even SFN
inline int oddParam(int evenParam) { return evenParam ^ 1; }
// SYNC-DL phase quadruples (TS 25.223 Table 8), degrees: S1 = a P-CCPCH in the next 4 sub-frames, S2 = none
constexpr int kS1[4] = {135, 45, 225, 135}, kS2[4] = {315, 225, 315, 45};

// ---------------------------------------------------------------- test transmitter (nothing is transmitted)
struct TxCell {
    int cellParam = 0;            // initial cell parameter (used in even SFN frames); its code group = cellParam / 4
    int sfn0 = 0;                 // SFN of the first frame
    double cfoHz = 0, amp = 1;
    int chipOffset = 0;           // where the first sub-frame starts in the capture (chips, may be negative)
    bool s2Every = false;         // send S2 (no P-CCPCH) in every second quadruple instead of always S1
};
std::vector<cf32> generate(const std::vector<TxCell>& cells, size_t samples, double snrDb, unsigned seed = 1);

// ---------------------------------------------------------------- the search
struct Found {
    int syncDl = -1;              // SYNC-DL code = code group
    int cellParam = -1;           // the initial cell parameter (even SFN), -1 when the midamble was not identified
    int evenFirst = -1;           // 1: the first whole frame of the block has an even SFN, 0 odd, -1 unknown
    int subframeStart = 0;        // first sub-frame start in the block (samples at kFs)
    int quadS1 = 0, quadS2 = 0;   // phase quadruples recognised
    double cfoHz = 0, levelDbfs = -200, snrDb = 0;
    float midScore = 0;           // how clearly the midamble chose the parameter (best over second best)
    int subframes = 0;
};
struct SearchOpts { int maxCells = 6; double thresh = 6.0; };
std::vector<Found> search(const cf32* x, size_t n, const SearchOpts& o = {}, double* ms = nullptr);

}  // namespace dect2::tdscdma
