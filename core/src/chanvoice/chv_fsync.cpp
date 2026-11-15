// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kenwood FleetSync burst: see chv_fsync.h for the format and the source of its description.
#include "dect2/chanvoice/chv_fsync.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace chv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr uint16_t kSync = 0xA23E;
constexpr int kLead = 4;                 // bits between the sync word and the first data word
constexpr int kFs1Bits = kLead + 64;     // 68
constexpr int kFs2Bits = kLead + 4 * 64; // 260
constexpr uint16_t kPoly = 0x6815;
const uint16_t kParity[8] = {0x4045, 0x2067, 0x1076, 0x083b, 0x0458, 0x022c, 0x0116, 0x008b};
const uint16_t kRepair[15] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x17, 0x2e, 0x5c, 0xb8, 0x67, 0xce, 0x8b};

int pop(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

uint32_t pack(const uint8_t* b, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (uint32_t)(b[i] & 1); return v; }

// One 15 bit code word: the syndrome is the 8 parity checks over the word. Zero: fine; equal to one of kRepair: that bit is wrong.
bool repair15(uint16_t v, uint16_t& out) {
    unsigned s = 0;
    for (int i = 0; i < 8; i++) if (pop((uint64_t)(kParity[i] & v)) & 1) s |= 1u << i;
    if (s == 0) { out = v; return true; }
    for (int i = 0; i < 15; i++) if (s == kRepair[i]) { out = (uint16_t)(v ^ (1u << (14 - i))); return true; }
    return false;
}

// A code word carrying `nib` in bits 11..8: bits 6..0 are free (the data bits, with the other check bits a function of them); the 128 choices are searched.
bool encode15(unsigned nib, uint16_t& out) {
    for (unsigned d = 0; d < 128; d++) {
        unsigned par = 0;   // bit (14 - i) of the word is the check whose syndrome is the unit vector kRepair[i] (i = 0..7)
        for (int k = 0; k < 7; k++) if (d & (1u << k)) par ^= kRepair[14 - k];
        // the word: data in bits 6..0, check bit for syndrome bit i is bit (14 - i)
        uint16_t w = (uint16_t)d;
        for (int i = 0; i < 8; i++) if (par & (1u << i)) w |= (uint16_t)(1u << (14 - i));
        uint16_t chk;
        if (!repair15(w, chk) || chk != w) continue;   // (never expected)
        if (((w >> 8) & 0xF) == nib) { out = w; return true; }
    }
    return false;
}

void put(std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((x >> i) & 1)); }
} // namespace

uint16_t fsyncCrc(uint32_t w1, uint32_t w2) {
    uint16_t parity = 0, sr = 0;
    for (int bit = 0; bit < 48; bit++) {
        const uint16_t cur = bit < 32 ? (uint16_t)((w1 >> (31 - bit)) & 1) : (uint16_t)((w2 >> (31 - (bit - 32))) & 1);
        if (cur) parity ^= 1;
        if (cur ^ ((sr >> 15) & 1)) sr ^= kPoly;
        sr = (uint16_t)(sr << 1);
    }
    for (int bit = 48; bit < 63; bit++) if ((w2 >> (31 - (bit - 32))) & 1) parity ^= 1;
    sr ^= 0x0002;
    return (uint16_t)((unsigned)sr + parity);
}

static void fields(uint32_t w1, uint32_t w2, FsyncMessage& m, bool fs2) {
    m.fleet = (int)((w1 >> 8) & 0xFF) + 99;
    m.unit = (int)((w1 & 0xFF) << 4) + (int)((w2 >> 28) & 0xF) + 999;
    m.fs2 = fs2; m.word1 = w1; m.word2 = w2;
}

bool fsyncDecodeBits(const uint8_t* b, size_t n, FsyncMessage& m) {
    if (n < (size_t)kFs1Bits) return false;
    uint32_t w1 = pack(b + kLead, 32), w2 = pack(b + kLead + 32, 32);
    if (fsyncCrc(w1, w2) == (uint16_t)(w2 & 0xFFFF)) { fields(w1, w2, m, false); return true; }
    if (n < (size_t)kFs2Bits) return false;
    uint32_t nib[2][8] = {};
    for (int blk = 0; blk < 4; blk++) {
        const uint8_t* p = b + kLead + blk * 64;
        const uint32_t bw1 = pack(p, 32), bw2 = pack(p + 32, 32);
        const uint16_t h[4] = {(uint16_t)(bw1 & 0xFFFF), (uint16_t)(bw1 >> 16), (uint16_t)(bw2 & 0xFFFF), (uint16_t)(bw2 >> 16)};
        for (int k = 0; k < 4; k++) {
            uint16_t c;
            if (!repair15(h[k], c)) return false;
            nib[blk / 2][(blk % 2) * 4 + k] = (c >> 8) & 0xF;
        }
    }
    uint32_t a = 0, c = 0;
    for (int i = 0; i < 8; i++) { a = (a << 4) | nib[0][i]; c = (c << 4) | nib[1][i]; }
    if (fsyncCrc(a, c) != (uint16_t)(c & 0xFFFF)) return false;
    fields(a, c, m, true);
    return true;
}

std::string fsyncText(const FsyncMessage& m) {
    char b[96];
    snprintf(b, sizeof b, "%s fleet %d unit %d", m.fs2 ? "FleetSync II" : "FleetSync", m.fleet, m.unit);
    return b;
}

std::vector<uint8_t> fsyncPayloadBits(int fleet, int unit, bool fs2, uint16_t head, uint16_t mid) {
    const unsigned f = (unsigned)std::max(0, std::min(255, fleet - 99)), u = (unsigned)std::max(0, std::min(4095, unit - 999));
    const uint32_t w1 = ((uint32_t)head << 16) | (f << 8) | (u >> 4);
    uint32_t w2 = ((u & 0xF) << 28) | ((uint32_t)(mid & 0xFFF) << 16);
    // the block check includes a parity of its own bits: search the 16 bit value that matches itself
    uint32_t chk = 0; bool found = false;
    for (uint32_t c = 0; c < 65536; c++) if (fsyncCrc(w1, w2 | c) == (uint16_t)c) { chk = c; found = true; break; }
    if (!found) chk = 0;   // (not seen for any input tried; the decoder would then reject the frame)
    w2 |= chk;
    std::vector<uint8_t> v;
    put(v, 0, kLead);
    if (!fs2) { put(v, w1, 32); put(v, w2, 32); return v; }
    for (int blk = 0; blk < 4; blk++) {
        const uint32_t w = blk < 2 ? w1 : w2;
        const int sh = (blk % 2) ? 0 : 16;   // block 0 / 2 hold the high 16 bits (4 nibbles), block 1 / 3 the low 16 bits
        uint16_t cw[4];
        for (int k = 0; k < 4; k++) { const unsigned nb = (w >> (sh + 12 - 4 * k)) & 0xF; if (!encode15(nb, cw[k])) cw[k] = 0; }
        put(v, ((uint32_t)cw[1] << 16) | cw[0], 32);
        put(v, ((uint32_t)cw[3] << 16) | cw[2], 32);
    }
    return v;
}

std::vector<float> fsyncEncode(int fleet, int unit, bool fs2, double rate, double amp, int leadBits, bool invert, uint16_t head, uint16_t mid) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < leadBits; i++) bits.push_back((uint8_t)(((leadBits - 1 - i) & 1) ? 1 : 0));   // alternating, ending in 0
    put(bits, kSync, 16);
    const std::vector<uint8_t> pl = fsyncPayloadBits(fleet, unit, fs2, head, mid);
    bits.insert(bits.end(), pl.begin(), pl.end());
    for (int i = 0; i < 4; i++) bits.push_back(1);
    std::vector<float> out;
    const double spb = rate / 1200.0;
    out.reserve((size_t)((double)bits.size() * spb) + 16);
    double ph = 0, t = 0;
    for (size_t i = 0; i < bits.size(); i++) {
        const int bit = invert ? !bits[i] : bits[i];
        const double f = bit ? 1200.0 : 1800.0;
        const size_t end = (size_t)std::llround((double)(i + 1) * spb);
        while ((size_t)std::llround(t) < end) { out.push_back((float)(amp * std::sin(ph))); ph += 2 * kPi * f / rate; t += 1.0; }
    }
    return out;
}

FsyncDecoder::FsyncDecoder(double rate) : rate_(rate), spb_(rate / 1200.0) {
    len_ = std::max(4, (int)std::lround(spb_));
    ring_.assign((size_t)len_ + 8, 0.f);
    c12_.resize((size_t)len_); s12_.resize((size_t)len_); c18_.resize((size_t)len_); s18_.resize((size_t)len_);
    for (int i = 0; i < len_; i++) {
        c12_[(size_t)i] = (float)std::cos(2 * kPi * 1200.0 * i / rate_); s12_[(size_t)i] = (float)std::sin(2 * kPi * 1200.0 * i / rate_);
        c18_[(size_t)i] = (float)std::cos(2 * kPi * 1800.0 * i / rate_); s18_[(size_t)i] = (float)std::sin(2 * kPi * 1800.0 * i / rate_);
    }
    reset();
}

void FsyncDecoder::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    wr_ = 0; n_ = 0; lastEnd_ = -1;
    for (int p = 0; p < kPhases; p++) { ph_[p] = Phase{}; ph_[p].next = spb_ * (p + 1) / kPhases + spb_; }
}

bool FsyncDecoder::busy() const { for (const Phase& p : ph_) if (p.capture) return true; return false; }

void FsyncDecoder::feed(const float* x, size_t n) {
    const uint64_t kMask40 = (1ULL << 40) - 1, kPre = (1ULL << 24) - 1;
    for (size_t i = 0; i < n; i++) {
        ring_[wr_] = x[i];
        wr_ = (wr_ + 1) % ring_.size();
        n_++;
        const double now = (double)n_;
        for (int p = 0; p < kPhases; p++) {
            Phase& P = ph_[p];
            if (now < P.next) continue;
            P.next += spb_;
            double a = 0, b = 0, c = 0, d = 0;
            size_t pos = (wr_ + ring_.size() - (size_t)len_) % ring_.size();
            for (int k = 0; k < len_; k++) {
                const double v = ring_[pos];
                a += v * c12_[(size_t)k]; b += v * s12_[(size_t)k]; c += v * c18_[(size_t)k]; d += v * s18_[(size_t)k];
                if (++pos == ring_.size()) pos = 0;
            }
            const int bit = (a * a + b * b) > (c * c + d * d) ? 1 : 0;   // 1200 Hz = 1
            if (!P.capture) {
                P.reg = ((P.reg << 1) | (uint64_t)bit) & kMask40;
                const uint64_t pre = (P.reg >> 16) & kPre;
                const int dpre = std::min(pop(pre ^ 0xAAAAAAULL), pop(pre ^ (~0xAAAAAAULL & kPre)));
                if (dpre > 3) continue;
                const uint16_t syn = (uint16_t)(P.reg & 0xFFFF);
                if (pop((uint64_t)(syn ^ kSync)) <= 1) { P.capture = true; P.inverted = false; P.got = 0; }
                else if (pop((uint64_t)(syn ^ (uint16_t)~kSync)) <= 1) { P.capture = true; P.inverted = true; P.got = 0; }
            } else {
                P.buf[P.got++] = (uint8_t)(P.inverted ? !bit : bit);
                if (P.got != kFs1Bits && P.got != kFs2Bits) continue;
                FsyncMessage m;
                if (fsyncDecodeBits(P.buf, (size_t)P.got, m)) {
                    m.timeSec = now / rate_;
                    P.capture = false; P.reg = 0;
                    if (lastEnd_ >= 0 && m.timeSec - lastEnd_ < 0.03 && m.word1 == last_.word1 && m.word2 == last_.word2) continue;   // the eight phases read one burst
                    last_ = m; lastEnd_ = m.timeSec; count_++;
                    if (cb_) cb_(m);
                } else if (P.got == kFs2Bits) { crcBad_++; P.capture = false; P.reg = 0; }
            }
        }
    }
}

} // namespace chv
} // namespace dect2
