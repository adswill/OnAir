// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE: the physical layer of the downlink broadcast channels (3GPP TS 36.211 / 36.212 / 36.213), written from the
// specifications. Only what a cell broadcasts to everyone is handled: synchronisation signals, reference signals, the MIB on the PBCH,
// PCFICH, and PDCCH / PDSCH for SI-RNTI (system information). Paging (P-RNTI) and every UE-specific channel are never decoded: a DCI is
// accepted only when its CRC matches SI-RNTI.
#pragma once
#include "cell_fec.h"
#include "dect2/common/ring.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::cell {

constexpr int kMaxRb = 110;
constexpr uint16_t kSiRnti = 0xFFFF;

struct LteCellCfg {
    int pci = 0;                 // physical cell id 0 .. 503 (3 * NID1 + NID2)
    bool extCp = false;          // extended cyclic prefix
    bool tdd = false;            // frame structure type 2
    int nRb = 6;                 // 6, 15, 25, 50, 75, 100
    int ports = 1;               // cell-specific reference signal ports: 1, 2, 4
    int phichDur = 0;            // 0 normal, 1 extended
    int phichRes = 0;            // Ng: 0 = 1/6, 1 = 1/2, 2 = 1, 3 = 2
    int mibTail = 0;             // MIB bits 14 - 23 as sent: schedulingInfoSIB1-BR-r13 (5 bits, LTE-M), systemInfoUnchanged-BR-r15, 4 spare
};

int lteNfft(int nRb);                         // FFT size used here: 128 for 6 RBs ... 2048 for 75 and 100 RBs
inline double lteRate(int nfft) { return 15000.0 * nfft; }
inline int lteSymbPerSlot(bool ext) { return ext ? 6 : 7; }
int lteSymStart(int lsf, bool ext, int nfft);  // first sample (of the cyclic prefix) of symbol lsf of a subframe
int lteCpLen(int lsf, bool ext, int nfft);
inline int lteSubframeLen(int nfft) { return 15 * nfft; }
int lteBin(int k, int nRb, int nfft);          // FFT bin of subcarrier k (0 .. 12 nRb - 1)
int lteBwIndexToRb(int i);                     // MIB dl-Bandwidth 0..5 -> 6 .. 100
int lteRbToBwIndex(int nRb);

void ltePss(int nid2, cf32 d[62]);
void lteSss(int nid1, int nid2, int subframe, float d[62]);   // subframe 0 or 5
// Cell-specific reference signal r(m'), m' = 0 .. 2*110-1, for slot ns and symbol l of the slot
void lteCrsSeq(int pci, int ns, int l, bool ext, cf32 r[2 * kMaxRb]);
// Is RE (k, symbol lsf of the subframe) a reference signal of antenna port p? (subframe sf)
bool lteCrsAt(const LteCellCfg& c, int sf, int lsf, int k, int p);
bool lteCrsAny(const LteCellCfg& c, int sf, int lsf, int k, int ports);   // of any of the first `ports` ports

// A resource element group: its 4 REs (subcarrier k) in symbol l
struct LteReg { int l = 0; int k0 = 0; int k[4] = {}; };
// The control region of a subframe: PCFICH REGs, PHICH REGs, and the PDCCH REG of each quadruplet z(i) (after the permutation and the cyclic
// shift of 36.211 6.8.5 are undone)
struct LteControl {
    int cfi = 1;
    std::vector<LteReg> pcfich;      // 4
    std::vector<LteReg> phich;       // 3 per mapping unit
    std::vector<LteReg> pdcchQuad;   // quadruplet i of the PDCCH bit stream -> its REG
    int nCce = 0;
};
LteControl lteControl(const LteCellCfg& c, int cfi, int subframe);
int lteCtrlSyms(int cfi, int nRb);               // OFDM symbols of the control region: CFI, one more for 10 RBs and less
int ltePhichGroups(const LteCellCfg& c);
extern const uint8_t kCfiCode[3][32];

// DCI formats for SI-RNTI
int lteDci1ASize(int nRb);
int lteDci1CSize(int nRb);
struct LteDciSi {
    int format = 0;              // 0 = format 1A, 1 = format 1C
    bool distributed = false;
    int gap = 0;                 // 0 gap 1, 1 gap 2
    int rbStart = 0, lCrb = 0;   // VRBs
    int mcs = 0;                 // 1A: MCS (= I_TBS); 1C: I_TBS
    int rv = 0;                  // 1A only
    int nPrb1A = 2;              // 1A: the TBS table column (2 or 3)
    int tbs = 0;
};
bool lteDciParse(const Bits& payload, int nRb, int format, LteDciSi& d);
Bits lteDciBuild(const LteDciSi& d, int nRb);   // payload bits (without CRC)
int lteTbs(int itbs, int nPrb);                  // Table 7.1.7.2.1-1, nPrb 1..10 (the columns needed here)
int lteTbs1C(int itbs);                          // Table 7.1.7.2.3-1
bool lteRiv(int riv, int n, int& start, int& len);
int lteRivMake(int start, int len, int n);
// PRBs of slot 0 and slot 1 of a DCI's allocation
void ltePrbs(const LteDciSi& d, int nRb, std::vector<int>& slot0, std::vector<int>& slot1);
// PDSCH REs (symbol, subcarrier) in mapping order for an allocation (PSS/SSS/PBCH and reference signals excluded)
std::vector<std::pair<int, int>> ltePdschRes(const LteCellCfg& c, int sf, int cfi, const std::vector<int>& prb0, const std::vector<int>& prb1);
int ltePbchBits(bool extCp);                     // 1920 or 1728

// ---------------------------------------------------------------------------------------------------------------- test signal
// One eNodeB carrier, as the tests and the built-in test signal use it. Nothing is transmitted.
struct LteSib { int sfnMod = 0; int period = 2; int subframe = 5; Bits bits; bool c1 = false; };   // a message on SI-RNTI: sent when sfn % period == sfnMod in that subframe
struct LteGenCfg {
    LteCellCfg cell;
    int cfi = 2;
    int sfn0 = 0;                // SFN of the first frame
    std::vector<LteSib> sibs;    // SI messages (the payload bits of BCCH-DL-SCH)
    bool pagingDci = true;       // also send a P-RNTI DCI in subframe 9 (the receiver must ignore it)
    bool loadPdsch = true;       // fill the rest of the data region with random QPSK (other users)
    uint32_t seed = 1;
    std::vector<std::array<float, 4>> portTaps;   // per port: up to 2 taps (delay samples at 30.72 Msps, gain dB) as {d1, g1, d2, g2}; empty = flat
};
// Time samples at lteRate(lteNfft(nRb)) for nFrames frames (sum of the ports through their channels), rms about 0.2
std::vector<cf32> lteGenerate(const LteGenCfg& g, int nFrames);

// ---------------------------------------------------------------------------------------------------------------- receiver
struct LteFound {                // a cell found by the synchronisation signals
    int pci = -1;
    bool extCp = false, tdd = false;
    double cfoHz = 0;            // carrier offset against the candidate centre
    double frameStart = 0;       // input sample (at the input rate) where a radio frame starts (subframe 0)
    double clockPpm = 0;         // the sample clock error measured from the PSS spacing
    float pssDb = 0;             // detection strength (peak to mean)
    float sssScore = 0;          // SSS match (0..1)
    float sssSig = 0;            // SSS significance: the match against the spread of all the other identities
};
struct LteMibInfo { bool ok = false; int nRb = 0, ports = 0, phichDur = 0, phichRes = 0, sfn = 0; int mibTail = 0; double frameStart = 0; };
// LTE-M (eMTC, Rel-13): the MIB's schedulingInfoSIB1-BR-r13 (TS 36.331 6.2.2) says whether the cell sends SystemInformationBlockType1-BR for
// BL / CE devices: 0 not sent, 1 - 18 sent (TS 36.213 Table 7.1.6-1 repetitions, Table 7.1.7.2.7-1 transport block size), 19 - 31 reserved.
inline int lteMibSib1Br(int mibTail) { return (mibTail >> 5) & 31; }
inline bool lteMibSiUnchangedBr(int mibTail) { return ((mibTail >> 4) & 1) != 0; }
inline int lteSib1BrReps(int v) { return v >= 1 && v <= 18 ? (v - 1) % 3 == 0 ? 4 : (v - 1) % 3 == 1 ? 8 : 16 : 0; }
inline int lteSib1BrTbs(int v) { static const int t[6] = {208, 256, 328, 504, 712, 936}; return v >= 1 && v <= 18 ? t[(v - 1) / 3] : 0; }
struct LteMeas { float rsrpDb = -200, rsrqDb = -99, sinrDb = -99, rssiDb = -200; };
struct LteSiMsg { int sfn = 0, subframe = 0; Bits bits; int format = 0; };
struct LteCellResult {
    LteFound found;
    LteMibInfo mib;
    LteMeas meas;
    std::vector<LteSiMsg> si;    // SI-RNTI transport blocks decoded (CRC ok)
    int cfiSeen = 0;
    int dciSeen = 0, dciFail = 0, tbFail = 0;
};

// Search a block (at rate fs, the candidate carrier at fs offset `offsetHz` from 0 Hz) for LTE cells and decode what they broadcast.
// maxCfoHz: the carrier offset searched (+-). wantSi: decode SIBs (needs the whole carrier inside the block).
struct LteSearchOpts { double maxCfoHz = 25000; bool wantSi = true; int maxCells = 12; float minPssDb = 9.0f; };
std::vector<LteCellResult> lteSearchDecode(const cf32* x, size_t n, double fs, double offsetHz, const LteSearchOpts& o);
// a quick PSS-only look (20 ms) at each offset: the offsets where some PSS stands minDb over the correlation mean, with its strength
struct LtePssProbe { double offsetHz = 0; float db = 0; };
std::vector<LtePssProbe> ltePssProbe(const cf32* x, size_t n, double fs, const std::vector<double>& offsetsHz, double maxCfoHz = 25000, float minDb = 9.0f);

} // namespace dect2::cell
