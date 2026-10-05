// ATSC 3.0 cell multiplexing of a subframe (A/322 7.2.6): the data cells of all symbols form one list indexed from 0; dummy values fill
// everything first, then the cells of each PLP overwrite their positions (non-dispersed PLPs contiguous, dispersed PLPs in subslices).
#pragma once
#include "atsc3_subframe.h"
#include <vector>

namespace dect2 {
namespace atsc3 {

struct PlpAlloc {
    int start = 0;            // L1D_plp_start: index of the first data cell
    int size = 0;             // L1D_plp_size: number of cells
    bool dispersed = false;   // L1D_plp_type = 1
    int numSubslices = 0;     // L1D_plp_num_subslices (dispersed only)
    int subsliceInterval = 0; // L1D_plp_subslice_interval
};

// Index in the subframe's list of the k-th cell of the PLP.
int plpCellIndex(const PlpAlloc& a, int k);

// Total number of active data cells of the subframe (before any carrier or symbol is mapped), `prefix` counting the cells at the start
// that come from the last Preamble symbol (first subframe only).
long subframeTotalCells(const SubframeParams& s, int prefix = 0);

// All active data cells of the subframe: dummy values with the PLP cells overwritten. `prefix` cells belong to the Preamble's last symbol.
std::vector<cf32> multiplexSubframe(const SubframeParams& s, int prefix, const std::vector<std::pair<PlpAlloc, std::vector<cf32>>>& plps);
std::vector<cf32> extractPlpCells(const std::vector<cf32>& active, const PlpAlloc& a);

// The symbols of the subframe as samples (guard + useful part each, back to back); `active` as made by multiplexSubframe (without the prefix cells).
std::vector<cf32> modulateSubframe(const SubframeParams& s, const std::vector<cf32>& activeAfterPrefix);
// The reverse: returns the active cells of all symbols in order (the prefix is not part of them) and the noise estimate per cell where asked.
bool demodulateSubframe(const SubframeParams& s, const cf32* samples, size_t n, std::vector<cf32>& active, std::vector<float>* noiseVar = nullptr);

} // namespace atsc3
} // namespace dect2
