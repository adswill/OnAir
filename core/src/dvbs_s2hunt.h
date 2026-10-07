// DVB-S2 frame synchronisation from a stored window of symbols: the PLHEADER is found by correlating the products of neighbouring symbols with
// those of the SOF and of the PLS code (which do not care about the carrier phase or a frequency offset of a few percent of the symbol rate), then
// each candidate is fitted with a maximum likelihood search over the 128 code words, the carrier frequency and the sense of the spectrum, and the
// frames are followed from it by the lengths the MODCODs give. Internal.
#pragma once
#include "dect2/ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {
namespace dvbs {

struct S2HuntResult {
    bool ok = false;
    size_t pos = 0;             // index (in the window) of the first symbol of the header the receiver starts from (the same as firstPos)
    size_t firstPos = 0;        // the first verified header of a data frame that has another header one frame length later
    size_t lastPos = 0;         // the last header verified in the window
    int frameLen = 0;           // symbols per frame of the header at firstPos
    int modcod = 0;
    bool shortFrame = false, pilots = false;
    double phi = 0;             // carrier offset in radians per symbol (to a few thousandths; the receiver refines it from the headers)
    double theta = 0;           // carrier phase at firstPos, radians
    float plsScore = 0;         // correlation with the winning PLS code word, 1 = perfect
    float plsSecond = 0;
    float sofScore = 0;         // correlation with the SOF after the phase correction
    int headers = 0;            // headers found one frame apart from firstPos on (including the one at firstPos)
    float snrDb = 0;            // Es/N0 from the SOF symbols
    int candidates = 0;         // peaks of the SOF correlation that were examined
    bool inverted = false;      // the symbols are complex conjugates of what was sent (spectral inversion): feed conj(z) to the tracker
};

// Window of n symbols (three frames of the longest kind find a header and two more). About 60 complex multiply-adds per symbol plus a fit per candidate.
S2HuntResult s2Hunt(const cf32* z, size_t n);

} // namespace dvbs
} // namespace dect2
