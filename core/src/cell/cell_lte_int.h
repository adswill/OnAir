// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE: helpers shared by the test signal and the receiver (not part of the public interface).
#pragma once
#include "dect2/cell/cell_lte.h"
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2::cell {

// One subframe of one antenna port (or of the received signal): nSym x nSc
struct LteGrid {
    int nSym = 14, nSc = 72;
    std::vector<cf32> v;
    void init(int ns, int nsc) { nSym = ns; nSc = nsc; v.assign((size_t)ns * (size_t)nsc, cf32(0, 0)); }
    cf32& at(int l, int k) { return v[(size_t)l * (size_t)nSc + (size_t)k]; }
    const cf32& at(int l, int k) const { return v[(size_t)l * (size_t)nSc + (size_t)k]; }
};

// QPSK (36.211 7.1.2)
inline cf32 qpsk(uint8_t b0, uint8_t b1) { const float s = dect2::math::k1OverSqrt2f; return cf32(b0 ? -s : s, b1 ? -s : s); }
// Transmit diversity (6.3.3.3 / 6.3.4.3): symbols -> per-port symbols (ports 1, 2, 4); the count is padded to a multiple of the ports
void lteTxdEncode(const std::vector<cf32>& d, int ports, std::vector<std::vector<cf32>>& y);
// Its inverse on received REs: r[j] with channel h[p][j] (per RE), noise variance nv; out: 2 LLRs per symbol (in order)
void lteTxdDecode(const cf32* r, const std::vector<const cf32*>& h, size_t n, int ports, float nv, std::vector<float>& llr);

// OFDM: one subframe grid (nSym x 12 nRb) to time samples at nfft (appends 15*nfft samples)
void lteOfdmMod(const LteGrid& g, int nRb, bool ext, int nfft, std::vector<cf32>& out);
// and back: x points at the subframe's first sample; Y scaled so a subcarrier's amplitude is that of the tone in x
void lteOfdmDemod(const cf32* x, int nRb, bool ext, int nfft, LteGrid& g);

// Channel estimate per port from the reference signals of one subframe (symbols of the whole subframe), and the noise variance
struct LteChan { std::vector<LteGrid> h; float noiseVar = 1e-3f; float rsrp = 0, rssi = 0; };
// wide > 1: the estimate is the mean of +-wide pilots in frequency and of the subframe's pilot symbols in time (a weak cell: less noise on
// the channel, for a channel that changes little over the band and the millisecond). lsfLo..lsfHi: only the pilots of these symbols
// (the PBCH symbols, where nothing unknown lies once the decoded cells are cancelled)
void lteChanEst(const LteGrid& y, const LteCellCfg& c, int sf, int nRbGrid, int rbOffset, int ports, LteChan& out, int wide = 0, int lsfLo = 0, int lsfHi = 99);
// Rebuild one port-summed subframe of a known cell from its channel and add it to y times scale (-1: subtract): the reference signals of every port, and in
// subframe 0 the PBCH of frame sfn (its MIB rebuilt with bandwidth mibNRb; 0 = none); used to take a decoded cell out from under a weaker one at the same timing
// symbols sent with transmit diversity on a list of REs (a decoded PDCCH or PDSCH), also rebuilt and taken out
struct LteTxPart { std::vector<std::pair<int, int>> res; std::vector<cf32> d; };
void lteCancelKnown(LteGrid& y, const LteChan& ch, const LteCellCfg& c, int sf, int sfn, int mibNRb, float scale = -1.f, const std::vector<LteTxPart>* extra = nullptr);

} // namespace dect2::cell
