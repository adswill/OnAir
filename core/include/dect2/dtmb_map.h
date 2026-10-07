// DTMB constellation mapping and soft demapping.
#pragma once
#include "dtmb_defs.h"
#include "ring.h"

namespace dect2::dtmb {

// nSym symbols from nSym * bitsPerSymbol bits (one per byte); the first bit of a symbol is label bit 0. Qam4Nr maps like Qam4.
void mapSymbols(Mapping m, const uint8_t* bits, size_t nSym, cf32* out);

// Max-log soft demapping of one symbol. x is on the unit-power constellation scale, var = E|n|^2 of the noise on x.
// Writes bitsPerSymbol(m) LLRs in the transmit order of the bits; positive favours a zero.
void demapSymbol(Mapping m, cf32 x, float var, float* llr);

// The same for a run of symbols with a noise variance each (table driven: the LLR of an axis bit is a function of the received level only)
void demapBlock(Mapping m, const cf32* x, const float* var, size_t n, float* llr);

// Mean squared distance of x[0], x[stride], x[2 stride], ... (n symbols in all) to the nearest constellation point, on the unit power scale. For the
// equalised symbols of a frame this is the decision based noise power: about E|n|^2 while the noise is small against the point spacing, below it when not.
double decisionError(Mapping m, const cf32* x, size_t n, size_t stride);

// A C=3780 body in the frequency domain (the 3780 FFT bins, physical order) -> the 36 system information symbols (in order) and the 3744 data
// symbols (in transmit order): carrier de-interleaving, then the system information positions are taken out.
void splitBody(const cf32* bins, cf32* si36, cf32* data3744);
// The bin of every system information symbol / data symbol (what splitBody reads)
const int16_t* siBins();
const int16_t* dataBins();

} // namespace dect2::dtmb
