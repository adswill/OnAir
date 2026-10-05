// Constellations used for L1-Basic and L1-Detail (A/322 Annex C).
#pragma once
#include "dect2/ring.h"
#include <vector>

namespace dect2 {
namespace atsc3 {

enum class Nuc { Qpsk, Nuc16_8, Nuc64_9, Nuc256_9, Nuc256_13 };

// Points indexed by the bit label (y0 is the most significant bit).
const std::vector<cf32>& signallingConstellation(Nuc kind);

} // namespace atsc3
} // namespace dect2
