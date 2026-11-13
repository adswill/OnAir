// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/chanvoice/chv_mdc.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace chv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr uint8_t kSync[5] = {0x07, 0x09, 0x2A, 0x44, 0x6F};

// the sync as the differential decisions: x = b xor previous b, the bit before the sync (the last of 0x55) is 1
uint64_t syncPattern() {
    uint64_t p = 0;
    int prev = 1;
    for (int i = 0; i < 40; i++) {
        const int b = (kSync[i / 8] >> (7 - i % 8)) & 1;
        p = (p << 1) | (uint64_t)(b ^ prev);
        prev = b;
    }
    return p;
}
constexpr uint64_t kMask40 = (1ull << 40) - 1;
int pop(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

// the 14 byte block as 112 on-air bits: 7 data bytes, 7 parity bytes, interleaved
void blockBits(const uint8_t d7[7], uint8_t out[112]) {
    uint8_t data[14];
    for (int i = 0; i < 7; i++) data[i] = d7[i];
    int csr[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; i++) {
        data[i + 7] = 0;
        for (int j = 0; j < 8; j++) {
            for (int k = 6; k > 0; k--) csr[k] = csr[k - 1];
            csr[0] = (data[i] >> j) & 1;
            data[i + 7] |= (uint8_t)(((csr[0] + csr[2] + csr[5] + csr[6]) & 1) << j);
        }
    }
    for (int n = 0; n < 112; n++) {
        const int bit = (data[n / 8] >> (n % 8)) & 1;       // bytes are sent least significant bit first
        out[16 * (n % 7) + n / 7] = (uint8_t)bit;           // interleaved over 7 rows of 16
    }
}

void buildData(uint8_t op, uint8_t arg, uint16_t id, uint8_t d7[7]) {
    d7[0] = op; d7[1] = arg; d7[2] = (uint8_t)(id >> 8); d7[3] = (uint8_t)id;
    const uint16_t c = mdcCrc(d7, 4);
    d7[4] = (uint8_t)(c & 0xFF); d7[5] = (uint8_t)(c >> 8); d7[6] = 0;
}
} // namespace

uint16_t mdcCrc(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

std::string mdcOpText(const MdcPacket& p) {
    char b[96];
    if (p.op == 0x01) snprintf(b, sizeof b, "PTT ID%s", p.arg == 0x80 ? " (start of transmission)" : p.arg == 0x00 ? " (end of transmission)" : "");
    else snprintf(b, sizeof b, "op 0x%02X arg 0x%02X%s", p.op, p.arg, p.dbl ? " (double packet)" : "");
    return b;
}

std::vector<float> mdcEncode(const MdcPacket& p, double rate, double amp, int leadBytes) {
    std::vector<uint8_t> bits;   // the b stream
    for (int i = 0; i < leadBytes; i++) for (int k = 7; k >= 0; k--) bits.push_back((uint8_t)((0x55 >> k) & 1));
    for (int i = 0; i < 5; i++) for (int k = 7; k >= 0; k--) bits.push_back((uint8_t)((kSync[i] >> k) & 1));
    uint8_t d7[7], blk[112];
    buildData(p.op, p.arg, p.unitId, d7);
    blockBits(d7, blk);
    bits.insert(bits.end(), blk, blk + 112);
    if (p.dbl) {
        uint8_t e7[7];
        e7[0] = p.extra[0]; e7[1] = p.extra[1]; e7[2] = p.extra[2]; e7[3] = p.extra[3];
        const uint16_t c = mdcCrc(e7, 4);
        e7[4] = (uint8_t)(c & 0xFF); e7[5] = (uint8_t)(c >> 8); e7[6] = 0;
        blockBits(e7, blk);
        bits.insert(bits.end(), blk, blk + 112);
    }
    std::vector<float> out;
    const double spb = rate / 1200.0;
    out.reserve((size_t)((double)bits.size() * spb) + 16);
    double ph = 0; int prev = 0;
    double t = 0;
    for (size_t i = 0; i < bits.size(); i++) {
        const double f = bits[i] != prev ? 1800.0 : 1200.0;
        prev = bits[i];
        const size_t end = (size_t)std::llround((double)(i + 1) * spb);
        while ((size_t)std::llround(t) < end) {
            out.push_back((float)(amp * std::sin(ph)));
            ph += 2 * kPi * f / rate;
            t += 1.0;
        }
    }
    return out;
}

MdcDecoder::MdcDecoder(double rate) : rate_(rate), spb_(rate / 1200.0) {
    len_ = std::max(4, (int)std::lround(spb_));
    ring_.assign((size_t)len_ + 8, 0.f);
    c12_.resize((size_t)len_); s12_.resize((size_t)len_); c18_.resize((size_t)len_); s18_.resize((size_t)len_);
    for (int i = 0; i < len_; i++) {
        c12_[(size_t)i] = (float)std::cos(2 * kPi * 1200.0 * i / rate_); s12_[(size_t)i] = (float)std::sin(2 * kPi * 1200.0 * i / rate_);
        c18_[(size_t)i] = (float)std::cos(2 * kPi * 1800.0 * i / rate_); s18_[(size_t)i] = (float)std::sin(2 * kPi * 1800.0 * i / rate_);
    }
    reset();
}

void MdcDecoder::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    wr_ = 0; n_ = 0; lastEnd_ = -1;
    for (int p = 0; p < kPhases; p++) { ph_[p] = Phase{}; ph_[p].next = spb_ * (p + 1) / kPhases + spb_; }
}

bool MdcDecoder::block(const uint8_t* x, uint8_t d[14]) {
    // (the differential reconstruction is done by the caller; x are bits b of the on-air order)
    uint8_t lb[112];
    for (int n = 0; n < 112; n++) lb[n] = x[16 * (n % 7) + n / 7];
    for (int i = 0; i < 14; i++) { d[i] = 0; for (int j = 0; j < 8; j++) if (lb[i * 8 + j]) d[i] |= (uint8_t)(1 << j); }
    return mdcCrc(d, 4) == (uint16_t)(d[4] | (d[5] << 8));
}

void MdcDecoder::emit(const MdcPacket& p) {
    // the eight sampling phases decode the same burst: one report
    if (lastEnd_ >= 0 && p.timeSec - lastEnd_ < 0.05 && p.op == last_.op && p.arg == last_.arg && p.unitId == last_.unitId && p.dbl == last_.dbl) return;
    last_ = p; lastEnd_ = p.timeSec;
    count_++;
    if (cb_) cb_(p);
}

void MdcDecoder::feed(const float* x, size_t n) {
    static const uint64_t pat = syncPattern();
    for (size_t i = 0; i < n; i++) {
        ring_[wr_] = x[i];
        wr_ = (wr_ + 1) % ring_.size();
        n_++;
        const double now = (double)n_;
        for (int p = 0; p < kPhases; p++) {
            Phase& P = ph_[p];
            if (now < P.next) continue;
            P.next += spb_;
            // energies of the window of the last len_ samples at 1200 and 1800 Hz
            double a = 0, b = 0, c = 0, d = 0;
            size_t pos = (wr_ + ring_.size() - (size_t)len_) % ring_.size();
            for (int k = 0; k < len_; k++) {
                const double v = ring_[pos];
                a += v * c12_[(size_t)k]; b += v * s12_[(size_t)k]; c += v * c18_[(size_t)k]; d += v * s18_[(size_t)k];
                if (++pos == ring_.size()) pos = 0;
            }
            const int xbit = (c * c + d * d) > (a * a + b * b) ? 1 : 0;
            P.reg = (P.reg << 1) | (uint64_t)xbit;
            const double t = now / rate_;
            if (P.state == 0) {
                if (pop((P.reg ^ pat) & kMask40) <= 3) { P.state = 1; P.got = 0; }
            } else {
                P.blk[P.got++] = (uint8_t)xbit;
                if (P.got == 112) {
                    // differential to bits: the last sync bit is 1
                    uint8_t bits[112]; int prev = P.state == 1 ? 1 : (int)P.first.extra[0];   // (extra[0] is reused as the carried bit for the second block)
                    for (int k = 0; k < 112; k++) { prev ^= P.blk[k]; bits[k] = (uint8_t)prev; }
                    uint8_t dd[14];
                    const bool ok = block(bits, dd);
                    if (ok && P.state == 1) {
                        MdcPacket pk; pk.op = dd[0]; pk.arg = dd[1]; pk.unitId = (uint16_t)((dd[2] << 8) | dd[3]); pk.timeSec = t;
                        if (pk.op == 0x35 || pk.op == 0x55) { pk.dbl = true; P.first = pk; P.first.extra[0] = (uint8_t)prev; P.state = 2; P.got = 0; }
                        else { emit(pk); P.state = 0; P.reg = 0; }
                    } else if (ok && P.state == 2) {
                        MdcPacket pk = P.first; pk.extra[0] = dd[0]; pk.extra[1] = dd[1]; pk.extra[2] = dd[2]; pk.extra[3] = dd[3]; pk.timeSec = t;
                        emit(pk); P.state = 0; P.reg = 0;
                    } else {
                        if (P.state == 1) crcBad_++;
                        else { P.first.extra[0] = 0; emit(P.first); }   // the second block was lost: the first is still a good packet
                        P.state = 0; P.reg = 0;
                    }
                }
            }
        }
    }
}

} // namespace chv
} // namespace dect2
