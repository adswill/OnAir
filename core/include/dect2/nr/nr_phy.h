// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells: the physical layer of the SS/PBCH block (3GPP TS 38.211 / 38.212 / 38.213 / 38.104), written from the specifications.
// Only what every cell broadcasts to all phones is handled: PSS and SSS (the physical cell id), the PBCH DMRS (SSB index, half frame) and
// the PBCH itself (polar code, CRC24C) carrying the MIB. Nothing addressed to a phone is decoded, and nothing is decrypted.
#pragma once
#include "dect2/cell/cell_fec.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::nr {

using cell::Bits;

// ---------------------------------------------------------------------------------------------------------------- polar code (38.212 5.3.1, 5.4.1)
struct PolarCode {
    int K = 0, E = 0, N = 0, n = 0;
    bool iil = true;                 // input bit interleaving (downlink: on)
    std::vector<uint8_t> frozen;     // N: 1 = frozen
    std::vector<int> info;           // the K information positions, ascending
    std::vector<int> J;              // sub-block interleaver: y(n) = d(J(n))
    std::vector<int> pil;            // input interleaver: c'(k) = c(pil(k)) (identity when !iil)
};
PolarCode polarCode(int K, int E, int nMax = 9, bool iil = true);
Bits polarEncode(const PolarCode& pc, const Bits& c);              // K bits (CRC included) -> N bits d
Bits polarRateMatch(const PolarCode& pc, const Bits& d);           // N -> E bits (repetition, puncturing or shortening)
std::vector<float> polarRateRecover(const PolarCode& pc, const float* e);   // E LLRs -> N LLRs (positive = 0)
// CRC-aided successive-cancellation list decoder: the candidates, best path metric first, each K bits (c, de-interleaved). The first one
// whose `check` passes is returned in `out`.
bool polarDecode(const PolarCode& pc, const std::vector<float>& llr, int L, Bits& out, bool (*check)(const Bits&, void*), void* ctx);

// ---------------------------------------------------------------------------------------------------------------- sequences
void pss(int nid2, float d[127]);                    // 38.211 7.4.2.2
void sss(int nid1, int nid2, float d[127]);          // 38.211 7.4.2.3
void pbchDmrs(int pci, int issbBar, cf32 r[144]);    // 38.211 7.4.1.4.1, issbBar 0..7

// SS/PBCH block grid: symbol l 0..3, subcarrier k 0..239 (38.211 Table 7.4.3.1-1)
struct Re { int l, k; };
std::vector<Re> pbchDataRes(int pci);   // 432, in mapping order
std::vector<Re> pbchDmrsRes(int pci);   // 144, in mapping order

// ---------------------------------------------------------------------------------------------------------------- MIB and the PBCH payload
struct Mib {
    int sfn = 0;                 // 10 bits (6 in the MIB, 4 in the PBCH payload)
    bool scs30 = false;          // subCarrierSpacingCommon: false scs15or60, true scs30or120
    int kSsb = 0;                // ssb-SubcarrierOffset with its MSB from the payload: 0..31 (FR1: >= 24 means no CORESET#0)
    int dmrsTypeAPos = 2;        // 2 or 3
    int coreset0 = 0, ss0 = 0;   // pdcch-ConfigSIB1: controlResourceSetZero, searchSpaceZero
    bool barred = false;         // cellBarred
    bool intraFreqResel = true;  // intraFreqReselection allowed
    bool hrf = false;            // half frame bit
    int ssbIdx = 0;              // SSB index (beam)
    int lmax = 8;                // 4 or 8 (FR1)
};
Bits mibBits(const Mib& m);                       // BCCH-BCH-Message, 24 bits
bool mibParse(const Bits& a24, Mib& m);
// PBCH transport block (38.212 7.1.1 - 7.1.3): 32 payload bits interleaved and scrambled, then CRC24C: K = 56 bits
Bits pbchPayload(const Mib& m, int pci);
bool pbchUnpack(const Bits& c56, int pci, int lmax, Mib& m);   // CRC checked by the caller
// The whole chain: the 864 scrambled PBCH bits of an SSB (38.211 7.3.3.1 scrambling included)
Bits pbchEncode(const Mib& m, int pci);
// Decode 864 soft values (positive = 0) for SSB DMRS index issbBar; tries Lmax 8 and 4. Returns true with the MIB.
bool pbchDecode(const float* llr864, int pci, int issbBar, Mib& m, int listSize = 8);

// ---------------------------------------------------------------------------------------------------------------- bands and the sync raster (38.104)
struct NrBand { int band; double dlLowMhz, dlHighMhz; int scsKhz; int gscnLo, gscnHi, gscnStep; bool tdd; const char* region; };
const std::vector<NrBand>& nrBands();
const NrBand* nrBandInfo(int band);
double gscnToHz(int gscn);
int hzToGscn(double hz);                          // the nearest GSCN (any range); -1 out of range
int nrBandOfGscn(int gscn, int scsKhz = 0);       // the first band in the table holding it (0 none)
int lmaxFor(int band, double hz);                 // the usual Lmax of a band (4 or 8)
// CORESET#0 of the MIB (38.213 Tables 13-1 to 13-4): RBs, symbols, RB offset from the SSB; false when the table row is reserved
bool coreset0Info(int ssbScsKhz, int pdcchScsKhz, int index, int& rbs, int& syms, int& offsetRb);
// The first OFDM symbol of SSB i within the half frame (38.213 4.1, Case A / C), -1 when i >= lmax
int ssbFirstSymbol(int i, int lmax);

// ---------------------------------------------------------------------------------------------------------------- timing
// OFDM at rate N * scs (N = 256 here): symbol length and cyclic prefix of symbol l of a half frame (38.211 5.3.1)
struct Numerology {
    int scsKhz = 30, nfft = 256;
    double rate() const { return 1000.0 * scsKhz * nfft; }
    int symsPerHalf() const { return 70 * (scsKhz / 15); }
    int cpLen(int l) const;           // l within the half frame
    int symStart(int l) const;        // first sample of its cyclic prefix
    int halfLen() const { return (int)(rate() * 0.005 + 0.5); }
};

// ---------------------------------------------------------------------------------------------------------------- test signal
struct GenCell {
    int pci = 0;
    int scsKhz = 30;
    int lmax = 8;
    uint32_t ssbMask = 0xFF;     // the SSBs sent in each burst
    std::vector<float> beamDb;   // per SSB index (empty: all 0 dB)
    int periodMs = 20;           // SSB burst period
    Mib mib;                     // sfn / hrf / ssbIdx are set per burst
    bool load = true;            // fill the SSB symbols around the SSB and the other symbols with random QPSK (other channels)
    uint32_t seed = 1;
};
// nHalf half frames from the SFN in mib.sfn at rate 256 * scs, rms about 0.2 on the SSB symbols
std::vector<cf32> generate(const GenCell& g, int nHalf);

// ---------------------------------------------------------------------------------------------------------------- receiver
struct Beam { int ssbIdx = 0; float rsrpDb = -200; };
struct CellResult {
    int pci = -1, nid1 = 0, nid2 = 0, scsKhz = 30;
    double cfoHz = 0;            // against the candidate frequency
    float pssDb = 0, sssSig = 0;
    bool hasMib = false;
    Mib mib;
    int issbBar = 0;
    float rsrpDb = -200, rsrqDb = -99, sinrDb = -99;   // of the strongest beam, dBFS per RE / dB
    std::vector<Beam> beams;     // every SSB of the burst that was found (only when the MIB gave the index)
    int pbchOk = 0, pbchTried = 0;
};
struct SearchOpts { double maxCfoHz = 30000; int scsKhz = 30; float minPssDb = 10.0f; int maxCells = 6; };
// Search a block (rate fs, the candidate SS_REF at offsetHz from 0 Hz) for SSBs of one subcarrier spacing and decode their PBCH
std::vector<CellResult> searchDecode(const cf32* x, size_t n, double fs, double offsetHz, const SearchOpts& o);

} // namespace dect2::nr
