// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cdma2000 / IS-95 test transmitter: pilot (W0), Sync Channel (W32, the real coding chain and Sync Channel Messages), a paging channel and
// other code channels filled with random symbols (W1, W8 - W15), each base station at its own PN offset. Only for the tests and the
// built-in test signal; nothing is transmitted.
#include "dect2/cdma/cdma.h"
#include "dect2/dsp/rrc.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <random>

namespace dect2::cdma {

namespace {
constexpr int kFrameChips = kPnLen, kSymChips = 256, kGroupFrames = 9;   // a message every 3 superframes (9 frames: 8 carry it)

// The Sync Channel modulation symbols (bits) of frames f0 .. f0 + nf - 1 of one cell, the encoder running from frame f0 - 2
std::vector<uint8_t> syncSymbols(const SyncMsg& base, int64_t f0, int nf) {
    std::vector<uint8_t> out;
    ConvEnc enc;
    const int64_t first = f0 - 2;
    for (int64_t f = first; f < f0 + nf; f++) {
        const int64_t g = (f >= 0 ? f : f - (kGroupFrames - 1)) / kGroupFrames, q = f - g * kGroupFrames;
        SyncMsg m = base;
        m.sysTime = (uint64_t)((int64_t)base.sysTime + 3 * g + 7) & ((1ULL << 36) - 1);   // 4 superframes after the message's last one
        const Bits cap = encapsulate(encodeSyncPdu(m));
        uint8_t fb[32] = {0};
        fb[0] = q == 0;
        if (q < 8)
            for (int i = 0; i < 31; i++) { const size_t k = (size_t)(31 * q + i); fb[i + 1] = k < cap.size() ? cap[k] : 0; }
        Bits code;
        for (int i = 0; i < 32; i++) enc.push(fb[i], code);
        if (f < f0) continue;
        uint8_t rep[128], tx[128];
        for (int i = 0; i < 64; i++) rep[2 * i] = rep[2 * i + 1] = code[(size_t)i];
        for (int i = 0; i < 128; i++) tx[i] = rep[interleaveAddr(i, 7, 1)];
        out.insert(out.end(), tx, tx + 128);
    }
    return out;
}
}  // namespace

std::vector<cf32> generate(const std::vector<TxCell>& cells, size_t samples, double snrDb, uint64_t startChip, unsigned seed) {
    const size_t nChips = samples / 2 + 16;
    std::vector<cf32> sum(samples, cf32(0, 0));
    std::mt19937 rng(seed);
    // root raised cosine, roll-off 0.2, 2 samples per chip, +-6 chips
    const int H = 12;
    std::vector<float> h(2 * H + 1);
    double e = 0;
    for (int k = -H; k <= H; k++) { h[(size_t)(k + H)] = (float)dsp::rrc(k / 2.0, 0.2); e += h[(size_t)(k + H)] * h[(size_t)(k + H)]; }
    for (auto& v : h) v = (float)(v / std::sqrt(e / 2));
    const auto& pi = pnI();
    const auto& pq = pnQ();
    for (const TxCell& c : cells) {
        const int64_t off = (int64_t)kOffsetUnit * (c.msg.pilotPn & 511);
        const int64_t l0 = (int64_t)startChip - off;   // the cell's own chip count at the first chip of the capture
        const int64_t f0 = (l0 >= 0 ? l0 : l0 - (kFrameChips - 1)) / kFrameChips;
        const int nf = (int)((nChips + (size_t)(l0 - f0 * kFrameChips)) / kFrameChips) + 2;
        const std::vector<uint8_t> sy = syncSymbols(c.msg, f0, nf);
        const double gp = std::pow(10.0, c.pilotDb / 20), gs = std::pow(10.0, c.syncDb / 20), gg = std::pow(10.0, c.pagingDb / 20);
        const double go = std::pow(10.0, c.otherDb / 20) / std::sqrt(8.0);
        std::vector<cf32> chips(nChips);
        int pag = 1, oth[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        for (size_t j = 0; j < nChips; j++) {
            const int64_t l = l0 + (int64_t)j;
            const int64_t lp = ((l % kFrameChips) + kFrameChips) % kFrameChips;
            const int w = (int)(lp & 63);
            if (w == 0) { pag = (rng() & 1) ? 1 : -1; for (int& v : oth) v = (rng() & 1) ? 1 : -1; }
            const int64_t sym = (l - f0 * kFrameChips) / kSymChips;
            const double ds = sy[(size_t)sym] ? -1 : 1;
            double a = gp + gs * ds * (walshBit(32, w) ? -1 : 1) + gg * pag * (walshBit(1, w) ? -1 : 1);
            for (int k = 0; k < 8; k++) a += go * oth[k] * (walshBit(8 + k, w) ? -1 : 1);
            const float ii = pi[(size_t)lp] ? -1.f : 1.f, qq = pq[(size_t)lp] ? -1.f : 1.f;
            chips[j] = cf32(ii, qq) * (float)(a * c.amp / std::sqrt(2.0));
        }
        // upsample by 2, pulse shape, turn by the cell's carrier offset
        const double wc = 2 * M_PI * c.cfoHz / kFs;
        for (size_t n = 0; n < samples; n++) {
            cf32 acc(0, 0);
            const int64_t base = (int64_t)n;
            for (int k = -H; k <= H; k++) {
                const int64_t m = base - k;   // input sample index m (chips at even m)
                if (m < 0 || (m & 1)) continue;
                const size_t ci = (size_t)(m / 2);
                if (ci < chips.size()) acc += chips[ci] * h[(size_t)(k + H)];
            }
            const double ph = wc * (double)n;
            sum[n] += acc * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    }
    double p = 0;
    for (auto& v : sum) p += std::norm(v);
    p /= (double)std::max<size_t>(1, samples);
    if (snrDb < 200) {
        dect2::normal_distribution<float> g(0.f, (float)std::sqrt(p * std::pow(10.0, -snrDb / 10) / 2));
        for (auto& v : sum) v += cf32(g(rng), g(rng));
    }
    return sum;
}

}  // namespace dect2::cdma
