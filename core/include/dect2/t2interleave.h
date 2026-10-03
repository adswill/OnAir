// DVB-T2 frequency interleaver (ETSI EN 302 755 clause 8.3.?): per-symbol permutation of the data cells.
#pragma once
#include <vector>

namespace dect2 {

// Interleaver address sequence H for a symbol with `nCells` data cells (C_P2, C_DATA or N_FC).
// Interleaved cell j of the symbol came from original cell H[j]. `oddSymbol`: symbol index parity within the frame.
void freqInterleaverSeq(int fftCode, int nCells, bool oddSymbol, std::vector<int>& H);

} // namespace dect2
