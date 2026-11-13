// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder bit stages: bytes and bits, line codes (NRZ, NRZI, Manchester, differential Manchester, Miller, 4b5b, 8b10b), the
// scramblers, the block interleaver, HDLC bit stuffing and the generic CRC.
#include "dect2/modebuilder/modebuilder.h"
#include <algorithm>
#include <array>

namespace dect2::modebuilder {
namespace stage {

std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes, bool lsb) {
    std::vector<uint8_t> b;
    b.reserve(bytes.size() * 8);
    for (uint8_t v : bytes)
        for (int k = 0; k < 8; k++) b.push_back((uint8_t)((v >> (lsb ? k : 7 - k)) & 1));
    return b;
}
std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits, bool lsb) {
    std::vector<uint8_t> out(bits.size() / 8);
    for (size_t i = 0; i < out.size(); i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) if (bits[i * 8 + (size_t)k] & 1) v |= (uint8_t)(1u << (lsb ? k : 7 - k));
        out[i] = v;
    }
    return out;
}

namespace {
// 4B5B (FDDI / 100BASE-TX) data codes, nibble 0..F, the leftmost bit sent first
const uint8_t k4b5b[16] = {0x1E, 0x09, 0x14, 0x15, 0x0A, 0x0B, 0x0E, 0x0F, 0x12, 0x13, 0x16, 0x17, 0x1A, 0x1B, 0x1C, 0x1D};
// 8b/10b (IEEE 802.3 clause 36) data codes: 5b/6b for RD- and RD+, written abcdei with a as the highest bit
const uint8_t k6bMinus[32] = {0x27, 0x1D, 0x2D, 0x31, 0x35, 0x29, 0x19, 0x38, 0x39, 0x25, 0x15, 0x34, 0x0D, 0x2C, 0x1C, 0x17,
                              0x1B, 0x23, 0x13, 0x32, 0x0B, 0x2A, 0x1A, 0x3A, 0x33, 0x26, 0x16, 0x36, 0x0E, 0x2E, 0x1E, 0x2B};
const uint8_t k6bPlus[32] = {0x18, 0x22, 0x12, 0x31, 0x0A, 0x29, 0x19, 0x07, 0x06, 0x25, 0x15, 0x34, 0x0D, 0x2C, 0x1C, 0x28,
                             0x24, 0x23, 0x13, 0x32, 0x0B, 0x2A, 0x1A, 0x05, 0x0C, 0x26, 0x16, 0x09, 0x0E, 0x11, 0x21, 0x14};
// 3b/4b, fghj with f as the highest bit; index 8 is the alternate A7
const uint8_t k4bMinus[9] = {0x0B, 0x09, 0x05, 0x0C, 0x0D, 0x0A, 0x06, 0x0E, 0x07};
const uint8_t k4bPlus[9] = {0x04, 0x09, 0x05, 0x03, 0x02, 0x0A, 0x06, 0x01, 0x08};

int ones(unsigned v) { int c = 0; while (v) { c += (int)(v & 1); v >>= 1; } return c; }

struct Rev8b10b {
    std::array<int8_t, 64> six;
    std::array<int8_t, 16> four;
    std::array<int8_t, 32> five;
    Rev8b10b() {
        six.fill(-1); four.fill(-1); five.fill(-1);
        for (int i = 0; i < 32; i++) { six[k6bMinus[i]] = (int8_t)i; six[k6bPlus[i]] = (int8_t)i; }
        for (int i = 0; i < 9; i++) { four[k4bMinus[i]] = (int8_t)(i & 7); four[k4bPlus[i]] = (int8_t)(i & 7); }
        four[0x07] = 7; four[0x08] = 7;
        for (int i = 0; i < 16; i++) five[k4b5b[i]] = (int8_t)i;
    }
};
const Rev8b10b& rev() { static const Rev8b10b r; return r; }

void push(std::vector<uint8_t>& out, unsigned code, int n) { for (int k = n - 1; k >= 0; k--) out.push_back((uint8_t)((code >> k) & 1)); }
unsigned take(const std::vector<uint8_t>& c, size_t at, int n) { unsigned v = 0; for (int k = 0; k < n; k++) v = (v << 1) | (c[at + (size_t)k] & 1u); return v; }
}   // namespace

std::vector<uint8_t> lineEncode(const std::vector<uint8_t>& bits, Line l, int toggleOn) {
    std::vector<uint8_t> o;
    switch (l) {
    case Line::NRZ: return bits;
    case Line::NRZI: {
        int lv = 0;
        for (uint8_t b : bits) { if ((b & 1) == toggleOn) lv ^= 1; o.push_back((uint8_t)lv); }
        return o;
    }
    case Line::Manchester:
    case Line::ManchesterThomas: {
        const bool ieee = l == Line::Manchester;
        for (uint8_t b : bits) { const int x = (b & 1) ^ (ieee ? 0 : 1); o.push_back((uint8_t)(x ? 0 : 1)); o.push_back((uint8_t)(x ? 1 : 0)); }
        return o;
    }
    case Line::DiffManchester: {
        int lv = 0;
        for (uint8_t b : bits) { if (!(b & 1)) lv ^= 1; o.push_back((uint8_t)lv); lv ^= 1; o.push_back((uint8_t)lv); }
        return o;
    }
    case Line::Miller: {
        int lv = 0, prev = 1;
        for (uint8_t b : bits) {
            if (b & 1) { o.push_back((uint8_t)lv); lv ^= 1; o.push_back((uint8_t)lv); }
            else { if (!prev) lv ^= 1; o.push_back((uint8_t)lv); o.push_back((uint8_t)lv); }
            prev = b & 1;
        }
        return o;
    }
    case Line::B4B5: {
        for (size_t i = 0; i < bits.size(); i += 4) {
            unsigned nib = 0;
            for (size_t k = 0; k < 4; k++) nib = (nib << 1) | (i + k < bits.size() ? (bits[i + k] & 1u) : 0u);
            push(o, k4b5b[nib], 5);
        }
        return o;
    }
    case Line::B8B10: {
        int rd = -1;
        for (size_t i = 0; i < bits.size(); i += 8) {
            unsigned v = 0;   // the first bit is A (the least significant)
            for (size_t k = 0; k < 8; k++) if (i + k < bits.size() && (bits[i + k] & 1)) v |= 1u << k;
            const unsigned x = v & 31, y = v >> 5;
            const unsigned six = rd < 0 ? k6bMinus[x] : k6bPlus[x];
            push(o, six, 6);
            if (ones(six) != 3) rd = -rd;
            unsigned idx = y;
            if (y == 7 && ((rd < 0 && (x == 17 || x == 18 || x == 20)) || (rd > 0 && (x == 11 || x == 13 || x == 14)))) idx = 8;
            const unsigned four = rd < 0 ? k4bMinus[idx] : k4bPlus[idx];
            push(o, four, 4);
            if (ones(four) != 2) rd = -rd;
        }
        return o;
    }
    }
    return bits;
}

int linePhases(Line l) {
    switch (l) {
    case Line::NRZ: case Line::NRZI: return 1;
    case Line::B4B5: return 5;
    case Line::B8B10: return 10;
    default: return 2;
    }
}
double lineExpansion(Line l) {
    switch (l) {
    case Line::NRZ: case Line::NRZI: return 1;
    case Line::B4B5: return 1.25;
    case Line::B8B10: return 1.25;
    default: return 2;
    }
}

std::vector<uint8_t> lineDecode(const std::vector<uint8_t>& c, Line l, int toggleOn, int phase, std::vector<int>* pos, int* errors) {
    std::vector<uint8_t> o;
    if (pos) pos->clear();
    int err = 0;
    const size_t ph = (size_t)std::max(0, phase);
    auto at = [&](size_t i) { if (pos) pos->push_back((int)i); };
    switch (l) {
    case Line::NRZ:
        for (size_t i = ph; i < c.size(); i++) { o.push_back(c[i] & 1); at(i); }
        break;
    case Line::NRZI: {
        int prev = ph > 0 ? (c[ph - 1] & 1) : 0;
        for (size_t i = ph; i < c.size(); i++) { const int cur = c[i] & 1; o.push_back((uint8_t)(cur != prev ? toggleOn : 1 - toggleOn)); prev = cur; at(i); }
        break;
    }
    case Line::Manchester:
    case Line::ManchesterThomas: {
        const int inv = l == Line::Manchester ? 0 : 1;
        for (size_t i = ph; i + 1 < c.size(); i += 2) {
            const int a = c[i] & 1, b = c[i + 1] & 1;
            if (a == b) err++;
            o.push_back((uint8_t)(b ^ inv));
            at(i);
        }
        break;
    }
    case Line::DiffManchester: {
        for (size_t i = ph; i + 1 < c.size(); i += 2) {
            const int p = i > 0 ? (c[i - 1] & 1) : 0, a = c[i] & 1, b = c[i + 1] & 1;
            if (a == b) err++;
            o.push_back((uint8_t)(a == p ? 1 : 0));
            at(i);
        }
        break;
    }
    case Line::Miller:
        for (size_t i = ph; i + 1 < c.size(); i += 2) { o.push_back((uint8_t)((c[i] & 1) != (c[i + 1] & 1))); at(i); }
        break;
    case Line::B4B5:
        for (size_t i = ph; i + 5 <= c.size(); i += 5) {
            const int v = rev().five[take(c, i, 5)];
            if (v < 0) err++;
            const unsigned nib = v < 0 ? 0u : (unsigned)v;
            for (int k = 3; k >= 0; k--) { o.push_back((uint8_t)((nib >> k) & 1)); at(i); }
        }
        break;
    case Line::B8B10:
        for (size_t i = ph; i + 10 <= c.size(); i += 10) {
            const int x = rev().six[take(c, i, 6)], y = rev().four[take(c, i + 6, 4)];
            if (x < 0 || y < 0) err++;
            const unsigned v = (unsigned)std::max(0, x) | ((unsigned)std::max(0, y) << 5);
            for (int k = 0; k < 8; k++) { o.push_back((uint8_t)((v >> k) & 1)); at(i); }
        }
        break;
    }
    if (errors) *errors = err;
    return o;
}

void scramble(std::vector<uint8_t>& bits, const Definition& d, bool encode) {
    if (d.scrambler == Scrambler::None || d.taps.empty()) return;
    const int len = *std::max_element(d.taps.begin(), d.taps.end());
    const uint64_t mask = len >= 64 ? ~0ull : (1ull << len) - 1;
    uint64_t r = d.seed & mask;
    for (uint8_t& b : bits) {
        unsigned s = 0;
        for (int t : d.taps) s ^= (unsigned)((r >> (t - 1)) & 1);
        const unsigned in = b & 1u;
        const unsigned out = in ^ s;
        if (d.scrambler == Scrambler::Additive) r = ((r << 1) | s) & mask;
        else r = ((r << 1) | (encode ? out : in)) & mask;   // self-synchronising: the register holds the scrambled bits
        b = (uint8_t)out;
    }
}

void interleave(std::vector<uint8_t>& bits, int rows, int cols, bool forward) {
    if (rows <= 0 || cols <= 0) return;
    const size_t blk = (size_t)rows * (size_t)cols;
    std::vector<uint8_t> t(blk);
    for (size_t s = 0; s + blk <= bits.size(); s += blk) {
        for (size_t r = 0; r < (size_t)rows; r++)
            for (size_t c = 0; c < (size_t)cols; c++) {
                if (forward) t[c * (size_t)rows + r] = bits[s + r * (size_t)cols + c];
                else t[r * (size_t)cols + c] = bits[s + c * (size_t)rows + r];
            }
        std::copy(t.begin(), t.end(), bits.begin() + (std::ptrdiff_t)s);
    }
}

std::vector<uint8_t> hdlcStuff(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> o;
    int run = 0;
    for (uint8_t b : bits) {
        o.push_back(b & 1);
        if (b & 1) { if (++run == 5) { o.push_back(0); run = 0; } }
        else run = 0;
    }
    return o;
}

}   // namespace stage

uint32_t crcCompute(const CrcModel& m, const uint8_t* d, size_t n) {
    const int w = std::clamp(m.width, 1, 32);
    const uint64_t mask = (w >= 32) ? 0xFFFFFFFFull : ((1ull << w) - 1);
    uint64_t crc = m.init & mask;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        if (m.refin) { uint8_t r = 0; for (int k = 0; k < 8; k++) if (b & (1 << k)) r |= (uint8_t)(0x80 >> k); b = r; }
        for (int k = 7; k >= 0; k--) {
            const uint64_t bit = (b >> k) & 1u;
            const uint64_t top = (crc >> (w - 1)) & 1u;
            crc = (crc << 1) & mask;
            if (top ^ bit) crc ^= m.poly & mask;
        }
    }
    if (m.refout) { uint64_t r = 0; for (int k = 0; k < w; k++) if (crc & (1ull << k)) r |= 1ull << (w - 1 - k); crc = r; }
    return (uint32_t)((crc ^ m.xorout) & mask);
}

}   // namespace dect2::modebuilder
