// gzip compression for Winlink gzip proposals: see wl_gzip.h. Deflate with the fixed Huffman codes of RFC 1951 section 3.2.6.
#include "dect2/winlink/wl_gzip.h"
#include "dect2/common/inflate.h"   // crc32Ieee
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace winlink {

namespace {
constexpr int kWindow = 32768;
constexpr int kMinMatch = 3, kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 128;

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Bits {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) {   // least significant bit first
        acc |= v << n;
        n += bits;
        while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void huff(uint32_t code, int bits) {   // Huffman codes go most significant bit first
        uint32_t r = 0;
        for (int i = 0; i < bits; i++) r |= ((code >> i) & 1u) << (bits - 1 - i);
        put(r, bits);
    }
    void flush() { if (n > 0) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

void litLen(Bits& b, int v) {
    if (v < 144) b.huff(0x30 + (uint32_t)v, 8);
    else if (v < 256) b.huff(0x190 + (uint32_t)(v - 144), 9);
    else if (v < 280) b.huff((uint32_t)(v - 256), 7);
    else b.huff(0xC0 + (uint32_t)(v - 280), 8);
}

void match(Bits& b, int len, int dist) {
    int li = 28;
    while (li > 0 && kLenBase[li] > len) li--;
    litLen(b, 257 + li);
    if (kLenExtra[li]) b.put((uint32_t)(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (di > 0 && kDistBase[di] > dist) di--;
    b.huff((uint32_t)di, 5);
    if (kDistExtra[di]) b.put((uint32_t)(dist - kDistBase[di]), kDistExtra[di]);
}

inline uint32_t hash3(const uint8_t* p) { return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & ((1u << kHashBits) - 1); }
} // namespace

std::vector<uint8_t> deflateFixed(const uint8_t* d, size_t n) {
    Bits b;
    b.put(1, 1);   // BFINAL
    b.put(1, 2);   // BTYPE 01: fixed Huffman codes
    std::vector<int> head((size_t)1 << kHashBits, -1), prev((size_t)kWindow, -1);
    size_t i = 0;
    auto insert = [&](size_t at) {
        if (at + kMinMatch > n) return;
        const uint32_t h = hash3(d + at);
        prev[at % kWindow] = head[h];
        head[h] = (int)at;
    };
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            int cand = head[hash3(d + i)];
            const int maxLen = (int)std::min<size_t>(kMaxMatch, n - i);
            for (int chain = 0; cand >= 0 && chain < kMaxChain; chain++) {
                const int dist = (int)i - cand;
                if (dist > kWindow - 1 || dist <= 0) break;
                if (d[cand + bestLen] == d[i + (size_t)bestLen] || bestLen == 0) {
                    int l = 0;
                    while (l < maxLen && d[(size_t)cand + (size_t)l] == d[i + (size_t)l]) l++;
                    if (l > bestLen) { bestLen = l; bestDist = dist; if (l == maxLen) break; }
                }
                const int nx = prev[(size_t)cand % kWindow];
                if (nx >= cand) break;   // the slot was reused by a newer position
                cand = nx;
            }
        }
        if (bestLen >= kMinMatch) {
            match(b, bestLen, bestDist);
            for (int k = 0; k < bestLen; k++) insert(i + (size_t)k);
            i += (size_t)bestLen;
        } else {
            litLen(b, d[i]);
            insert(i);
            i++;
        }
    }
    litLen(b, 256);   // end of block
    b.flush();
    return b.out;
}

std::vector<uint8_t> gzipCompress(const uint8_t* d, size_t n) {
    std::vector<uint8_t> o = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};   // deflate, no flags, no time, unknown OS
    const std::vector<uint8_t> z = deflateFixed(d, n);
    o.insert(o.end(), z.begin(), z.end());
    const uint32_t crc = crc32Ieee(d, n);
    const uint32_t len = (uint32_t)n;
    for (int k = 0; k < 4; k++) o.push_back((uint8_t)(crc >> (8 * k)));
    for (int k = 0; k < 4; k++) o.push_back((uint8_t)(len >> (8 * k)));
    return o;
}

} // namespace winlink
} // namespace dect2
