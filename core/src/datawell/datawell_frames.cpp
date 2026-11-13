// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bit stream analysis for the Datawell buoy HF link (datawell_frames.h).
#include "dect2/datawell/datawell_frames.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::datawell {

namespace {
const CrcSpec kSpecs[] = {
    {"CRC-16/CCITT-FALSE", 16, 0x1021, 0xFFFF, 0x0000, false, false},
    {"CRC-16/XMODEM", 16, 0x1021, 0x0000, 0x0000, false, false},
    {"CRC-16/KERMIT", 16, 0x1021, 0x0000, 0x0000, true, true},
    {"CRC-16/X-25", 16, 0x1021, 0xFFFF, 0xFFFF, true, true},
    {"CRC-16/AUG-CCITT", 16, 0x1021, 0x1D0F, 0x0000, false, false},
    {"CRC-16/ARC", 16, 0x8005, 0x0000, 0x0000, true, true},
    {"CRC-16/MODBUS", 16, 0x8005, 0xFFFF, 0x0000, true, true},
    {"CRC-16/BUYPASS", 16, 0x8005, 0x0000, 0x0000, false, false},
    {"CRC-16/DNP", 16, 0x3D65, 0x0000, 0xFFFF, true, true},
    {"CRC-32/ISO-HDLC", 32, 0x04C11DB7, 0xFFFFFFFF, 0xFFFFFFFF, true, true},
    // 8 bit checks: weak, one in 256 frames passes by chance
    {"CRC-8", 8, 0x07, 0x00, 0x00, false, false},
    {"CRC-8/MAXIM", 8, 0x31, 0x00, 0x00, true, true},
    {"sum of bytes (8 bit)", 8, 0xFFFFFFFFu, 0, 0, false, false},
    {"xor of bytes (8 bit)", 8, 0xFFFFFFFEu, 0, 0, false, false},
};
constexpr int kNSpecs = (int)(sizeof kSpecs / sizeof kSpecs[0]);

uint32_t reflect(uint32_t v, int w) { uint32_t r = 0; for (int i = 0; i < w; i++) if (v & (1u << i)) r |= 1u << (w - 1 - i); return r; }

std::vector<uint8_t> pack(const uint8_t* bits, size_t n, bool lsb, bool inv) {
    std::vector<uint8_t> b((n + 7) / 8, 0);
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = (uint8_t)((bits[i] ? 1 : 0) ^ (inv ? 1 : 0));
        if (v) b[i / 8] |= (uint8_t)(lsb ? (1u << (i % 8)) : (0x80u >> (i % 8)));
    }
    return b;
}
}

const CrcSpec* crcSpecs(int& count) { count = kNSpecs; return kSpecs; }

uint32_t crcCompute(const CrcSpec& s, const uint8_t* d, size_t n) {
    if (s.poly == 0xFFFFFFFFu) { uint32_t a = 0; for (size_t i = 0; i < n; i++) a += d[i]; return a & 0xFF; }
    if (s.poly == 0xFFFFFFFEu) { uint32_t a = 0; for (size_t i = 0; i < n; i++) a ^= d[i]; return a & 0xFF; }
    const int w = s.width;
    const uint32_t mask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1), top = 1u << (w - 1);
    uint32_t reg = s.init & mask;
    for (size_t i = 0; i < n; i++) {
        const uint32_t b = s.refin ? reflect(d[i], 8) : d[i];
        reg ^= b << (w - 8);
        for (int k = 0; k < 8; k++) reg = (reg & top) ? ((reg << 1) ^ s.poly) : (reg << 1);
        reg &= mask;
    }
    if (s.refout) reg = reflect(reg, w);
    return (reg ^ s.xorout) & mask;
}

Period findPeriod(const std::vector<uint8_t>& bits, int minLag, int maxLag) {
    Period best;
    const int n = (int)bits.size();
    if (n < 4 * minLag + 64) return best;
    int ones = 0;
    for (uint8_t b : bits) ones += b ? 1 : 0;
    if (ones < n / 10 || ones > n - n / 10) return best;   // a constant stream repeats at every lag: nothing to learn
    maxLag = std::min(maxLag, n / 5);
    // For a lag L the stream is cut into K periods; each column (position inside the period) should be a coin toss. The statistic
    // sum over columns of (2 ones - K)^2 / K is chi-squared with L degrees of freedom for random bits (mean L, variance 2L); columns that
    // carry the same bit in every period (a sync pattern, a constant field) push it up. It needs far fewer periods than comparing bit i
    // with bit i + L.
    std::vector<int> col;
    for (int lag = minLag; lag <= maxLag; lag++) {
        const int K = n / lag;
        col.assign((size_t)lag, 0);
        for (int i = 0; i < K; i++) { const uint8_t* p = bits.data() + (size_t)i * lag; for (int c = 0; c < lag; c++) col[(size_t)c] += p[c] ? 1 : 0; }
        double s = 0;
        for (int c = 0; c < lag; c++) { const double d = 2.0 * col[(size_t)c] - K; s += d * d / K; }
        const double z = (s - lag) / std::sqrt(2.0 * lag);
        if (z > best.z) { best.z = z; best.lag = lag; }
    }
    return best;
}

std::string bitsToHex(const uint8_t* bits, size_t n, bool lsbFirst, bool invert) {
    const std::vector<uint8_t> b = pack(bits, n, lsbFirst, invert);
    std::string s;
    char t[4];
    for (uint8_t v : b) { snprintf(t, sizeof t, "%02X", v); s += t; }
    return s;
}

Sync findSync(const std::vector<uint8_t>& bits, int lag) {
    Sync r;
    if (lag < 16) return r;
    const int K = (int)bits.size() / lag;
    if (K < 5) return r;
    std::vector<uint8_t> col((size_t)lag, 0), val((size_t)lag, 0);
    for (int c = 0; c < lag; c++) {
        int ones = 0;
        for (int i = 0; i < K; i++) ones += bits[(size_t)i * lag + c] ? 1 : 0;
        const int top = std::max(ones, K - ones);
        col[(size_t)c] = top >= K - K / 12 ? 1 : 0;   // one dissenting period in twelve is a symbol error, not a data column
        val[(size_t)c] = ones * 2 > K ? 1 : 0;
    }
    int bestLen = 0, bestStart = 0;
    for (int s = 0; s < lag; s++) {
        if (col[(size_t)s] && col[(size_t)((s + lag - 1) % lag)]) continue;   // not the start of a run
        int len = 0;
        while (len < lag && col[(size_t)((s + len) % lag)]) len++;
        if (len > bestLen) { bestLen = len; bestStart = s; }
    }
    if (bestLen < 8 || bestLen > lag / 2) return r;
    r.found = true; r.start = bestStart; r.len = bestLen;
    for (int i = 0; i < bestLen; i++) r.bits.push_back(val[(size_t)((bestStart + i) % lag)]);
    r.hex = bitsToHex(r.bits.data(), r.bits.size(), false, false);
    return r;
}

namespace {
bool passes(const std::vector<uint8_t>& bytes, const CrcSpec& s, int skip, bool little) {
    const int w = s.width / 8;
    if ((int)bytes.size() < skip + w + 1) return false;
    const size_t dn = bytes.size() - (size_t)w - (size_t)skip;
    const uint32_t c = crcCompute(s, bytes.data() + skip, dn);
    uint32_t got = 0;
    for (int i = 0; i < w; i++) {
        const uint32_t v = bytes[bytes.size() - (size_t)w + (size_t)i];
        if (little) got |= v << (8 * i); else got = (got << 8) | v;
    }
    return got == c;
}
}

CrcHit bestCrc(const std::vector<std::vector<uint8_t>>& frames, int syncBits) {
    CrcHit best;
    if (frames.size() < 3) return best;
    const int total = (int)frames.size();
    for (int inv = 0; inv < 2; inv++)
        for (int lsb = 0; lsb < 2; lsb++) {
            std::vector<std::vector<uint8_t>> bytes;
            for (const auto& f : frames) bytes.push_back(pack(f.data(), f.size() - f.size() % 8, lsb != 0, inv != 0));
            for (int si = 0; si < kNSpecs; si++) {
                const CrcSpec& s = kSpecs[si];
                for (int skipSync = 0; skipSync < 2; skipSync++) {
                    if (skipSync && (syncBits < 8 || syncBits % 8)) continue;
                    const int skip = skipSync ? syncBits / 8 : 0;
                    for (int little = 0; little < (s.width > 8 ? 2 : 1); little++) {
                        int p = 0;
                        for (const auto& b : bytes) p += passes(b, s, skip, little != 0) ? 1 : 0;
                        const bool strong = s.width >= 16;
                        const bool enough = strong ? (p >= 3 && p * 2 >= total) : (p == total && total >= 8);
                        if (!enough) continue;
                        if (p > best.pass || (p == best.pass && strong && best.specIdx >= 0 && kSpecs[best.specIdx].width < 16)) {
                            best.name = s.name; best.pass = p; best.total = total; best.inverted = inv != 0; best.lsbFirst = lsb != 0;
                            best.crcLittle = little != 0; best.skipBytes = skip; best.specIdx = si;
                        }
                    }
                }
            }
        }
    return best;
}

bool frameCrcOk(const std::vector<uint8_t>& frameBits, const CrcHit& h) {
    if (h.specIdx < 0) return false;
    const std::vector<uint8_t> b = pack(frameBits.data(), frameBits.size() - frameBits.size() % 8, h.lsbFirst, h.inverted);
    return passes(b, kSpecs[h.specIdx], h.skipBytes, h.crcLittle);
}

} // namespace dect2::datawell
