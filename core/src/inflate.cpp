#include "dect2/inflate.h"
#include <cstring>

namespace dect2 {

namespace {

struct Bits {
    const uint8_t* d; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool err = false;
    Bits(const uint8_t* data, size_t size) : d(data), n(size) {}
    uint32_t get(int k) {
        while (cnt < k) {
            if (pos >= n) { err = true; return 0; }
            buf |= (uint32_t)d[pos++] << cnt;
            cnt += 8;
        }
        uint32_t v = buf & ((1u << k) - 1);
        buf >>= k; cnt -= k;
        return v;
    }
};

// canonical Huffman code, decoded bit by bit with counts per length (as in zlib's puff)
struct Huff {
    uint16_t count[16];
    uint16_t symbol[320];
};

bool build(Huff& h, const uint8_t* lens, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[lens[i]]++;
    if (h.count[0] == n) return true;   // no codes: allowed for unused distance trees
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h.count[len]; if (left < 0) return false; }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++) if (lens[i]) h.symbol[offs[lens[i]]++] = (uint16_t)i;
    return true;
}

int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)b.get(1);
        if (b.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, std::vector<uint8_t>& out, size_t maxOut, const Huff& lit, const Huff& dist) {
    for (;;) {
        int sym = decode(b, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + (int)b.get(kLenExtra[sym]);
            int ds = decode(b, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
            if (b.err || d > out.size() || out.size() + len > maxOut) return false;
            size_t from = out.size() - d;
            for (int i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

} // namespace

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut, size_t* consumed) {
    Bits b(data, size);
    out.clear();
    int last;
    do {
        last = (int)b.get(1);
        int type = (int)b.get(2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;   // discard the rest of the byte
            if (b.pos + 4 > b.n) return false;
            unsigned len = data[b.pos] | (data[b.pos + 1] << 8), nlen = data[b.pos + 2] | (data[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen || b.pos + len > b.n || out.size() + len > maxOut) return false;
            out.insert(out.end(), data + b.pos, data + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            static Huff lit, dist;
            static bool init = false;
            if (!init) {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                build(lit, l, 288);
                uint8_t d[30];
                for (int i = 0; i < 30; i++) d[i] = 5;
                build(dist, d, 30);
                init = true;
            }
            if (!codes(b, out, maxOut, lit, dist)) return false;
        } else if (type == 2) {
            int nlen = (int)b.get(5) + 257, ndist = (int)b.get(5) + 1, ncode = (int)b.get(4) + 4;
            if (b.err || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)b.get(3);
            Huff lencode;
            if (!build(lencode, lengths, 19)) return false;
            int idx = 0;
            uint8_t ll[320] = {0};
            while (idx < nlen + ndist) {
                int sym = decode(b, lencode);
                if (sym < 0) return false;
                if (sym < 16) ll[idx++] = (uint8_t)sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (idx == 0) return false; val = ll[idx - 1]; rep = 3 + (int)b.get(2); }
                    else if (sym == 17) rep = 3 + (int)b.get(3);
                    else rep = 11 + (int)b.get(7);
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) ll[idx++] = (uint8_t)val;
                }
            }
            Huff lit, dist;
            if (ll[256] == 0 || !build(lit, ll, nlen) || !build(dist, ll + nlen, ndist)) return false;
            if (!codes(b, out, maxOut, lit, dist)) return false;
        } else return false;
    } while (!last);
    if (consumed) *consumed = b.pos - (b.cnt / 8);
    return true;
}

uint32_t crc32Ieee(const uint8_t* d, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool gunzip(const uint8_t* d, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    if (n < 18 || d[0] != 0x1F || d[1] != 0x8B || d[2] != 8) return false;
    const int flg = d[3];
    size_t pos = 10;
    if (flg & 4) { if (pos + 2 > n) return false; pos += 2 + (d[pos] | (d[pos + 1] << 8)); }
    if (flg & 8) { while (pos < n && d[pos]) pos++; pos++; }
    if (flg & 16) { while (pos < n && d[pos]) pos++; pos++; }
    if (flg & 2) pos += 2;
    if (pos + 8 > n) return false;
    size_t used = 0;
    if (!inflateRaw(d + pos, n - pos, out, maxOut, &used)) return false;
    pos += used;
    if (pos + 8 > n) return false;
    uint32_t crc = d[pos] | (d[pos + 1] << 8) | (d[pos + 2] << 16) | ((uint32_t)d[pos + 3] << 24);
    uint32_t isz = d[pos + 4] | (d[pos + 5] << 8) | (d[pos + 6] << 16) | ((uint32_t)d[pos + 7] << 24);
    return crc == crc32Ieee(out.data(), out.size()) && isz == (uint32_t)out.size();
}

} // namespace dect2
