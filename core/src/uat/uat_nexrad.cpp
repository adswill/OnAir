// FIS-B NEXRAD block decoding (see uat_nexrad.h).
#include "dect2/uat/uat_nexrad.h"
#include <cmath>

namespace dect2 {

namespace {

constexpr int kRing = 450;
constexpr double kBlockH = 4.0 / 60.0, kBlockW = 48.0 / 60.0;
constexpr uint32_t kWideFrom = 405000;

void place(UatNexradBlock& b, uint32_t num, int sf, bool south) {
    const double sc = sf == 1 ? 5.0 : sf == 2 ? 9.0 : 1.0;
    const uint32_t bn = num >= kWideFrom ? (num & ~1u) : num;
    b.blockNum = bn;
    const double rawLat = kBlockH * (double)(bn / kRing), rawLon = (double)(bn % kRing) * kBlockW;
    b.lonSize = (bn >= kWideFrom ? 2 * kBlockW : kBlockW) * sc;
    b.latSize = kBlockH * sc;
    b.lonWest = rawLon;
    b.latNorth = south ? -rawLat : rawLat + kBlockH;
}

} // namespace

bool uatNexradDecode(uint16_t product, const uint8_t* d, size_t n, std::vector<UatNexradBlock>& out) {
    if ((product != 63 && product != 64) || n < 3) return false;
    const bool rle = (d[0] & 0x80) != 0, south = (d[0] & 0x40) != 0;
    const int sf = (d[0] >> 4) & 3;
    const uint32_t num = (uint32_t)(d[0] & 0x0f) << 16 | (uint32_t)d[1] << 8 | d[2];
    if (rle) {
        UatNexradBlock b;
        b.productId = product;
        place(b, num, sf, south);
        size_t bin = 0;
        for (size_t i = 3; i < n && bin < 128; i++) {
            const int run = (d[i] >> 3) + 1, val = d[i] & 7;
            for (int k = 0; k < run && bin < 128; k++) b.bins[bin++] = (uint8_t)val;
        }
        if (bin != 128) return false;
        out.push_back(b);
        return true;
    }
    if (n < 4) return false;
    // empty blocks: bitmap
    const int L = d[3] & 15;
    if (4 + (size_t)L > n) return false;
    const uint32_t ringSize = num >= kWideFrom ? 225 : kRing;
    const uint8_t fill = product == 64 ? 1 : 0;
    auto emit = [&](uint32_t bn) {
        UatNexradBlock b; b.productId = product; place(b, bn, sf, south); b.bins.fill(fill); out.push_back(b);
    };
    emit(num);
    // same latitude ring only; wide rings use even block numbers
    const uint32_t step = num >= kWideFrom ? 2 : 1, ring = num / (num >= kWideFrom ? kRing : kRing);
    auto ringBlock = [&](uint32_t off) {
        const uint32_t base = ring * kRing;
        const uint32_t idx = ((num - base) / step + off) % ringSize;
        return base + idx * step;
    };
    for (int j = 0; j < 4; j++) if (d[3] & (0x10 << j)) emit(ringBlock((uint32_t)j + 1));
    for (int i = 0; i < L; i++) {
        const uint8_t m = d[4 + (size_t)i];
        for (int j = 0; j < 8; j++) if (m & (1 << j)) emit(ringBlock((uint32_t)(8 * (i + 1) + j - 3)));
    }
    return true;
}

size_t uatNexradEncodeRle(const UatNexradBlock& b, int scale, bool south, std::vector<uint8_t>& out) {
    out.push_back((uint8_t)(0x80 | (south ? 0x40 : 0) | ((scale & 3) << 4) | ((b.blockNum >> 16) & 15)));
    out.push_back((uint8_t)(b.blockNum >> 8));
    out.push_back((uint8_t)b.blockNum);
    for (size_t i = 0; i < 128;) {
        size_t r = 1;
        while (i + r < 128 && b.bins[i + r] == b.bins[i] && r < 32) r++;
        out.push_back((uint8_t)(((r - 1) << 3) | b.bins[i]));
        i += r;
    }
    return out.size();
}

} // namespace dect2
