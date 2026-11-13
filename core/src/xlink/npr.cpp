// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NPR sync word (see npr.h). Modem parameters read from the Si4463 configuration arrays of the NPR-70 firmware (GPL-3, F4HDK): MODEM_DATA_RATE / MODEM_TX_NCO_MODE give the symbol rate,
// MODEM_FREQ_DEV the outer deviation (half the symbol rate), PREAMBLE_CONFIG 0x31 the standard preamble, SYNC_CONFIG 0x0A a 3-byte sync word in 4FSK; the mapping 00 -3, 01 -1, 11 +1, 10 +3 is
// MODEM_FSK4_MAP 0 of the EZRadioPRO API documentation.
#include "dect2/xlink/npr.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2::xlink::npr {

using dect2::math::kPi;
const double kSymbolRates[5] = {50e3, 100e3, 180e3, 300e3, 500e3};

namespace {
const uint8_t kHash[16] = {0x33, 0x36, 0x39, 0x3C, 0x63, 0x66, 0x69, 0x6C, 0x93, 0x96, 0x99, 0x9C, 0xC3, 0xC6, 0xC9, 0xCC};
constexpr int kSps = 4;
int levelOf(int bitsPair) { switch (bitsPair & 3) { case 0: return -3; case 1: return -1; case 3: return 1; default: return 3; } }

// 12 symbols of the sync word in levels, -3 .. 3
void syncLevels(uint32_t w, int* lv) {
    for (int i = 0; i < 12; i++) lv[i] = levelOf((int)((w >> (22 - 2 * i)) & 3));
}
} // namespace

uint32_t syncWord(int net) { return 0x45u << 16 | 0xD2u << 8 | kHash[net & 15]; }
int networkOfHash(uint8_t h) { for (int i = 0; i < 16; i++) if (kHash[i] == h) return i; return -1; }

void SyncFinder::feed(const float* d, size_t n) {
    static const std::vector<std::vector<float>> patterns = [] {
        std::vector<std::vector<float>> p(16);
        for (int net = 0; net < 16; net++) {
            int lv[12];
            syncLevels(syncWord(net), lv);
            p[(size_t)net].resize(12);
            for (int i = 0; i < 12; i++) p[(size_t)net][(size_t)i] = (float)lv[i];
        }
        return p;
    }();
    h_.insert(h_.end(), d, d + n);
    n_ += (int64_t)n;
    constexpr int kPre = 16 * 4;                // preamble samples before the sync to check (4 samples per symbol * 16 symbols)
    constexpr int kSyncLen = 12 * kSps;
    size_t i = 0;
    const size_t minLen = (size_t)(kPre + kSyncLen + 8);
    for (; i + minLen <= h_.size(); i++) {
        const float* sy = h_.data() + i + (size_t)kPre;
        // symbol values with removed mean; take the middle two samples of each symbol
        float v[12], m = 0, en = 0;
        for (int s = 0; s < 12; s++) { v[s] = sy[s * kSps + 1] + sy[s * kSps + 2]; m += v[s]; }
        m /= 12;
        for (int s = 0; s < 12; s++) { v[s] -= m; en += v[s] * v[s]; }
        if (en < 1e-9f) continue;
        int bestN = -1; float bestC = 0;
        for (int net = 0; net < 16; net++) {
            float c = 0, ep = 0;
            const auto& p = patterns[(size_t)net];
            float pm = 0; for (int s = 0; s < 12; s++) pm += p[(size_t)s]; pm /= 12;
            for (int s = 0; s < 12; s++) { c += v[s] * (p[(size_t)s] - pm); ep += (p[(size_t)s] - pm) * (p[(size_t)s] - pm); }
            const float nc = c / std::sqrt(en * ep);
            if (nc > bestC) { bestC = nc; bestN = net; }
        }
        const int64_t at = base_ + (int64_t)i + kPre;
        if (pend_.network >= 0 && at > pend_.atSample + 8) { if (cb_) cb_(pend_); last_ = pend_.atSample; pend_ = Hit{}; }
        if (bestC < 0.93f || bestN < 0 || at - last_ < 12 * kSps) continue;
        float mp = 0;
        for (int s = 0; s < 12; s++) mp += sy[-kSps * (12 - s) + 1] + sy[-kSps * (12 - s) + 2];
        mp /= 12;
        int alt = 0, sized = 0;
        const float want = (float)kPi / 2;                // the outer deviation is half the symbol rate: two phase steps of 4 samples per symbol make pi / 2
        for (int s = 0; s < 12; s++) {
            const float x = sy[-kSps * (12 - s) + 1] + sy[-kSps * (12 - s) + 2] - mp;
            if ((s & 1) ? x > 0 : x < 0) alt++;
            if (std::fabs(x) > 0.55f * want && std::fabs(x) < 1.45f * want) sized++;
        }
        if ((alt != 0 && alt != 12) || sized < 11) continue;
        if (bestC > pend_.metric) {
            pend_.rateIndex = rate_; pend_.network = bestN; pend_.metric = bestC; pend_.atSample = at;
            pend_.offsetHz = (double)mp / 2 / (2 * kPi) * (kSymbolRates[rate_] * kSps);
        }
    }
    if (i > 0) { base_ += (int64_t)i; h_.erase(h_.begin(), h_.begin() + (ptrdiff_t)i); }
}

std::vector<cf32> txFrame(int rateIndex, int network, const std::vector<uint8_t>& payload, double sps) {
    (void)rateIndex;
    const int isps = (int)sps;
    std::vector<int> levels;
    for (int i = 0; i < 40; i++) levels.push_back((i & 1) ? -3 : 3);            // preamble, 2-level on the outer deviation
    const uint32_t w = syncWord(network);
    for (int i = 0; i < 12; i++) levels.push_back(levelOf((int)((w >> (22 - 2 * i)) & 3)));
    for (uint8_t b : payload) for (int i = 6; i >= 0; i -= 2) levels.push_back(levelOf((b >> i) & 3));
    // Gaussian pulse (BT 0.5) on the deviation, peak deviation (outer) = symbol rate / 2 (phase step = 2 pi f / fs)
    const double dev = 0.5;     // of the symbol rate
    std::vector<double> g((size_t)(4 * isps + 1));
    double s2 = 0;
    for (size_t i = 0; i < g.size(); i++) {
        const double t = ((double)i - 2.0 * isps) / isps;
        const double a = 0.5;
        const double sig = std::sqrt(std::log(2.0)) / (2 * kPi * a);
        g[i] = std::exp(-t * t / (2 * sig * sig));
        s2 += g[i];
    }
    for (double& v : g) v /= s2;
    std::vector<double> freq(levels.size() * (size_t)isps + g.size(), 0);
    for (size_t k = 0; k < levels.size(); k++) for (size_t j = 0; j < g.size(); j++) for (int r = 0; r < isps; r++) freq[k * (size_t)isps + (size_t)r + j] += levels[k] / 3.0 * dev * g[j];
    std::vector<cf32> out(freq.size());
    double ph = 0;
    for (size_t i = 0; i < freq.size(); i++) { ph += 2 * kPi * freq[i] / sps; out[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    return out;
}

} // namespace dect2::xlink::npr
