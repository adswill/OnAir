// Generated tables of A/322 for the data path (see atsc3_data_tables.cpp).
#pragma once
namespace dect2 {
namespace atsc3 {
// Position vector of the non-uniform constellation with the given bits per cell (4, 6, 8: first quadrant as re, im pairs;
// 10, 12: one dimension) for code rate rate15 / 15.
const float* nucVectors(int bitsPerCell, int rate15);
// Group-wise bit interleaver permutation pi(j) of A/322 Annex B (nInner / 360 entries); nullptr if the combination does not exist.
const unsigned short* groupPermutation(int nInner, int bitsPerCell, int rate15);
} // namespace atsc3
} // namespace dect2
