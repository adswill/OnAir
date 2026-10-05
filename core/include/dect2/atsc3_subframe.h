// ATSC 3.0 subframe OFDM symbols (A/322 7.2.4, 7.2.6, 7.3 and 8.1): data symbols with scattered pilots, subframe boundary symbols, continual
// and edge pilots, null cells, frequency interleaving, and the modulator and demodulator for one symbol.
#pragma once
#include "atsc3_ofdm.h"
#include <vector>

namespace dect2 {
namespace atsc3 {

struct SubframeParams {
    int fftSize = 8192;
    int cred = 0;                // Cred_coeff 0..4
    int guard = 192;             // guard interval in samples
    int spPattern = 0;           // L1D_scattered_pilot_pattern: 0 SP3_2, 1 SP3_4, 2 SP4_2, ... 15 SP32_4
    int spBoost = 0;             // L1D_scattered_pilot_boost 0..4
    bool freqInterleaver = true;
    bool sbsFirst = false, sbsLast = false;   // the first / last symbol is a subframe boundary symbol
    int numSymbols = 8;          // data and boundary symbols of the subframe
    int sbsNullCells = 0;        // L1D_sbs_null_cells: null cells in each boundary symbol (half at each end)
    int fiOffset = 0;            // number of symbols before the subframe that count for the interleaver (Preamble symbols in the first subframe)
};

int spDx(int pattern);
int spDy(int pattern);
double spAmplitude(const SubframeParams& s);   // Table 9.14, amplitude of the scattered pilots and boundary pilots
int guardFromCode(int code);                   // Table 9.11: L1D_guard_interval -> samples, 0 for reserved
int fftFromCode(int code);                     // Table 9.10

bool isBoundarySymbol(const SubframeParams& s, int l);

// 0 = data, 1 = scattered or boundary pilot, 2 = common continual pilot, 3 = additional continual pilot, 4 = edge pilot
std::vector<uint8_t> subframeCarrierKinds(const SubframeParams& s, int l);
int subframeDataCells(const SubframeParams& s, int l);   // carriers that carry cells (null cells of boundary symbols included)
int subframeActiveCells(const SubframeParams& s, int l); // cells that carry PLP data: the data cells without the null cells of boundary symbols

// Modulates one symbol: `cells` are the cells of all data carriers in order of increasing carrier (null cells included, as zeros).
std::vector<cf32> modulateSubframeSymbol(const SubframeParams& s, int l, const std::vector<cf32>& cells);
// The reverse; the cells come back in the order they were passed to the modulator.
bool demodulateSubframeSymbol(const SubframeParams& s, int l, const cf32* x, std::vector<cf32>& cells, float* noiseVar = nullptr);

} // namespace atsc3
} // namespace dect2
