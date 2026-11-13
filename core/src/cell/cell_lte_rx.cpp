// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE receiver: cell search (PSS / SSS), the MIB on the PBCH, and the system information on SI-RNTI (PCFICH, the common
// search space of the PDCCH, PDSCH). 3GPP TS 36.211 / 36.212 / 36.213. A DCI is accepted only when its CRC matches SI-RNTI: paging
// (P-RNTI), random access and every UE-specific channel are never decoded.
// (The search itself is in cell_lte_search.cpp, the synchronisation helpers in cell_lte_sync.cpp.)
#include "cell_lte_sync.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace dect2::cell {
using namespace lte_rx;

void lteCancelKnown(LteGrid& y, const LteChan& ch, const LteCellCfg& c, int sf, int sfn, int mibNRb, float scale, const std::vector<LteTxPart>* extra) {
    const int N = lteSymbPerSlot(c.extCp), nsym = 2 * N, nsc = 12 * c.nRb;
    if (y.nSym != nsym || y.nSc != nsc || (int)ch.h.size() < c.ports) return;
    std::vector<LteGrid> port((size_t)c.ports);
    for (auto& p : port) p.init(nsym, nsc);
    addCrs(port, c, sf);
    if (sf == 0 && mibNRb > 0 && lteRbToBwIndex(mibNRb) >= 0) {
        Bits mib;
        auto put = [&](int v, int n) { for (int i = n - 1; i >= 0; i--) mib.push_back((uint8_t)((v >> i) & 1)); };
        put(lteRbToBwIndex(mibNRb), 3); put(c.phichDur, 1); put(c.phichRes, 2); put((sfn >> 2) & 0xff, 8); put(c.mibTail & 0x3ff, 10);
        appendCrc(mib, kCrc16, 16, mibMask(c.ports));
        const int M = ltePbchBits(c.extCp), per = M / 4, seg = (sfn & 1023) % 4;
        Bits e = convRateMatch(convEncode(mib), (size_t)M);
        const Bits cs = gold((uint32_t)c.pci, (size_t)M);
        std::vector<cf32> d;
        for (int i = seg * per; i + 1 < (seg + 1) * per; i += 2) d.push_back(qpsk(e[(size_t)i] ^ cs[(size_t)i], e[(size_t)i + 1] ^ cs[(size_t)i + 1]));
        std::vector<std::vector<cf32>> yv;
        lteTxdEncode(d, c.ports, yv);
        size_t j = 0;
        for (int l = N; l < N + 4; l++) for (int kk = 0; kk < 72; kk++) {
            const int k = 6 * c.nRb - 36 + kk;
            const bool rs = (l - N <= 1) || (c.extCp && l - N == 3);
            if (rs && (k % 3) == (c.pci % 6) % 3) continue;
            for (int p = 0; p < c.ports; p++) if (j < yv[(size_t)p].size()) port[(size_t)p].at(l, k) = yv[(size_t)p][j];
            j++;
        }
    }
    if (extra) for (const LteTxPart& pt : *extra) {
        std::vector<std::vector<cf32>> yv;
        lteTxdEncode(pt.d, c.ports, yv);
        for (int p = 0; p < c.ports; p++)
            for (size_t i = 0; i < pt.res.size() && i < yv[(size_t)p].size(); i++) {
                const auto& q = pt.res[i];
                if (q.first >= 0 && q.first < nsym && q.second >= 0 && q.second < nsc) port[(size_t)p].at(q.first, q.second) = yv[(size_t)p][i];
            }
    }
    for (int l = 0; l < nsym; l++) for (int k = 0; k < nsc; k++) {
        cf32 v(0, 0);
        for (int p = 0; p < c.ports; p++) v += ch.h[(size_t)p].at(l, k) * port[(size_t)p].at(l, k);
        y.at(l, k) += scale * v;
    }
}




std::vector<LtePssProbe> ltePssProbe(const cf32* x, size_t n, double fs, const std::vector<double>& offsetsHz, double maxCfoHz, float minDb) {
    std::vector<LtePssProbe> out;
    if (!x || fs < kSyncRate * 0.99 || offsetsHz.empty()) return out;
    const size_t nSync = (size_t)(4 * kHalf + kNfft6 + 16);
    const size_t len = (size_t)std::ceil((double)nSync * fs / kSyncRate) + 64;
    if (n < len) return out;
    // one FFT of the 20 ms; each offset takes its own slice of bins (a brick-wall filter of +-0.7 MHz), back to the time domain at a
    // low power-of-two fraction of the rate, then to 1.92 Msps with a short filter. Far cheaper than filtering at the full rate per offset.
    int N = 1; while ((size_t)N < len) N <<= 1;
    int M = 1; while (fs * M / N < 2.4e6) M <<= 1;
    if (M > N) M = N;
    const double fsM = fs * M / N, binHz = fs / N;
    Fft big(N), small(M);
    std::vector<cf32> X((size_t)N, cf32(0, 0));
    for (size_t i = 0; i < len; i++) X[i] = x[i];
    big.forward(X.data());
    const int half = (int)std::floor(0.7e6 / binHz);
    std::vector<cf32> Y((size_t)M);
    static const bool dbg = getenv("CELL_DEBUG") != nullptr;
    for (double off : offsetsHz) {
        const long c = std::lround(off / binHz);   // the slice centre; the rest of the offset is left as a carrier offset
        std::fill(Y.begin(), Y.end(), cf32(0, 0));
        for (int k = -half; k <= half && k < M / 2; k++) {
            const long src = ((c + k) % N + N) % N;
            Y[(size_t)((k + M) % M)] = X[(size_t)src] / (float)N;
        }
        small.inverse(Y.data());
        const double rest = off - c * binHz;
        const std::vector<cf32> y = resampleBlock(Y.data(), (size_t)M, fsM, kSyncRate, rest, 1.4e6, 0, nSync);
        const std::vector<Cand> cd = pssSearch(y, maxCfoHz, minDb, 4, 1);
        if (dbg && !cd.empty()) fprintf(stderr, "[cell] probe %.0f: %.1f dB\n", off, cd[0].db);
        if (!cd.empty()) out.push_back({off, cd[0].db});
    }
    return out;
}

} // namespace dect2::cell
