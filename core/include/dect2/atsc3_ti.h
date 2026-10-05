// ATSC 3.0 time interleaving (A/322 section 7.1) for the PLP cells: the Convolutional Time Interleaver (CTI mode) and the cell interleaver
// and twisted block interleaver of the Hybrid Time Interleaver (HTI mode, intra-subframe). The convolutional delay line of inter-subframe
// HTI is not implemented.
#pragma once
#include "ring.h"
#include <deque>
#include <vector>

namespace dect2 {
namespace atsc3 {

// L1D_plp_CTI_depth (Table 9.24) to the number of rows; `extended` is L1D_plp_TI_extended_interleaving (QPSK only)
int ctiRows(int depthCode, bool extended);

// Convolutional Time Interleaver: Nrows delay lines, line k holds k cells. The initial content is a PRBS-generated sequence of cells made with
// `constellation` (eta bits per cell) as A/322 7.1.4.2 describes, so the first output cells after start-up are the initial state.
class CtiInterleaver {
public:
    CtiInterleaver(int rows, int bitsPerCell, const std::vector<cf32>& constellation, int startRow = 0);
    int rows() const { return rows_; }
    cf32 push(cf32 in);   // one cell in, one cell out
    std::vector<cf32> process(const std::vector<cf32>& in);
private:
    int rows_, row_;
    std::vector<std::deque<cf32>> lines_;
};

// The matching de-interleaver: line k holds (rows - 1 - k) cells, so that the total delay of every cell is rows * (rows - 1) cells.
class CtiDeinterleaver {
public:
    CtiDeinterleaver(int rows, int startRow = 0);
    cf32 push(cf32 in);
    std::vector<cf32> process(const std::vector<cf32>& in);
    int latency() const { return rows_ * (rows_ - 1); }   // cells before the output is the real signal
private:
    int rows_, row_;
    std::vector<std::deque<cf32>> lines_;
};

// Hybrid Time Interleaver, intra-subframe. A TI block holds `nFec` FEC blocks of `cellsPerBlock` cells; nFecMax is L1D_plp_HTI_num_fec_blocks_max
// divided by the number of TI blocks (the number of columns of the twisted block interleaver).
std::vector<int> htiCellPermutation(int cellsPerBlock, int blockIndex);      // L_r(q) for FEC block r of a TI block
std::vector<cf32> htiCellInterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec);
std::vector<cf32> htiCellDeinterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec);
std::vector<int> htiTwistedReadOrder(int cellsPerBlock, int nFec, int nFecMax);   // memory position read at output index i (virtual cells skipped)
std::vector<cf32> htiBlockInterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec, int nFecMax);
std::vector<cf32> htiBlockDeinterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec, int nFecMax);
// Both steps (cell interleaver optional): FEC blocks of one TI block in, interleaved cells out and back.
std::vector<cf32> htiInterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec, int nFecMax, bool cellInterleaver);
std::vector<cf32> htiDeinterleave(const std::vector<cf32>& tiBlock, int cellsPerBlock, int nFec, int nFecMax, bool cellInterleaver);

// Number of FEC blocks in TI block s of an interleaving frame with `nFecIf` FEC blocks and `nTi` TI blocks (A/322 7.1.5.1).
int htiBlocksInTiBlock(int nFecIf, int nTi, int s);

} // namespace atsc3
} // namespace dect2
