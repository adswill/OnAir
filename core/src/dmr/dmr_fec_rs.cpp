// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR error control codes on top of the Rust crate rust/onair_dmr: the functions of dmr_fec.h;
// dmr_fec.cpp keeps the plain bit helpers (putBits, getBits, bytesToBits, bitsToBytes, popcount32). Bit vectors that are shorter than the code needs are zero padded.
#include "dect2/dmr/dmr_fec.h"
#include "dect2/rs/dmr.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace dmr {

namespace {
// copies the first n bits of a vector into a fixed array, zero padding the rest
template <size_t N> void take(const Bits& b, uint8_t (&a)[N]) {
    memset(a, 0, N);
    memcpy(a, b.data(), std::min(b.size(), N));
}
} // namespace

unsigned golay2008Parity(unsigned info8) { return onair_dmr_golay2008_parity(info8); }
int golay2008Decode(unsigned word20, unsigned& info8) { uint32_t i; const int e = onair_dmr_golay2008_decode(word20, &i); if (e >= 0) info8 = i; return e; }
unsigned qr1676Parity(unsigned info7) { return onair_dmr_qr1676_parity(info7); }
int qr1676Decode(unsigned word16, unsigned& info7) { uint32_t i; const int e = onair_dmr_qr1676_decode(word16, &i); if (e >= 0) info7 = i; return e; }
unsigned hamming743Parity(unsigned info4) { return onair_dmr_hamming743_parity(info4); }
int hamming743Decode(unsigned word7, unsigned& info4) { uint32_t i; const int e = onair_dmr_hamming743_decode(word7, &i); if (e >= 0) info4 = i; return e; }

unsigned hammingParity(HammingKind k, unsigned info) { return onair_dmr_hamming_parity((int)k, info); }
int hammingCorrect(HammingKind k, unsigned& word) { uint32_t w = word; const int e = onair_dmr_hamming_correct((int)k, &w); if (e > 0) word = w; return e; }
int hammingInfoBits(HammingKind k) { return onair_dmr_hamming_info_bits((int)k); }
int hammingParityBits(HammingKind k) { return onair_dmr_hamming_parity_bits((int)k); }

void rs129Parity(const uint8_t msg[9], uint8_t parity[3]) { onair_dmr_rs129_parity(msg, parity); }
int rs129Correct(uint8_t word[12]) { return onair_dmr_rs129_correct(word); }

uint16_t crcCcitt(const uint8_t* d, size_t n) { return onair_dmr_crc_ccitt(d, n); }
uint8_t crc8Short(uint32_t bits28) { return onair_dmr_crc8_short(bits28); }
uint16_t crc9(const uint8_t* d, size_t nbytes, unsigned dbsn) { return onair_dmr_crc9(d, nbytes, dbsn); }
uint32_t crc32Msg(const uint8_t* d, size_t n) { return onair_dmr_crc32_msg(d, n); }
uint8_t checksum5(const uint8_t lc[9]) { return onair_dmr_checksum5(lc); }

void bptc196Encode(const Bits& info96, Bits& tx196) {
    uint8_t in[96];
    take(info96, in);
    tx196.assign(196, 0);
    onair_dmr_bptc196_encode(in, tx196.data());
}
int bptc196Decode(const Bits& tx196, Bits& info96) {
    uint8_t in[196], out[96];
    take(tx196, in);
    const int r = onair_dmr_bptc196_decode(in, out);
    if (r >= 0) info96.assign(out, out + 96);
    return r;
}

void embLcEncode(const uint8_t lc[9], Bits frag[4]) {
    uint8_t out[128];
    onair_dmr_emb_lc_encode(lc, out);
    for (int f = 0; f < 4; f++) frag[f].assign(out + 32 * f, out + 32 * f + 32);
}
int embLcDecode(const Bits frag[4], uint8_t lc[9]) {
    uint8_t in[128] = {};
    for (int f = 0; f < 4; f++) memcpy(in + 32 * f, frag[f].data(), std::min<size_t>(frag[f].size(), 32));
    return onair_dmr_emb_lc_decode(in, lc);
}

void shortLcEncode(uint32_t lc28, Bits piece[4]) {
    uint8_t out[68];
    onair_dmr_short_lc_encode(lc28, out);
    for (int f = 0; f < 4; f++) piece[f].assign(out + 17 * f, out + 17 * f + 17);
}
int shortLcDecode(const Bits piece[4], uint32_t& lc28) {
    uint8_t in[68] = {};
    for (int f = 0; f < 4; f++) memcpy(in + 17 * f, piece[f].data(), std::min<size_t>(piece[f].size(), 17));
    uint32_t v = lc28;
    const int r = onair_dmr_short_lc_decode(in, &v);
    if (r >= 0 || r == -2) lc28 = v;
    return r;
}

void cachEncode(int at, int tc, int lcss, unsigned payload17, Bits& out24) {
    out24.assign(24, 0);
    onair_dmr_cach_encode(at, tc, lcss, payload17, out24.data());
}
int cachDecode(const Bits& in24, int& at, int& tc, int& lcss, unsigned& payload17) {
    uint8_t in[24];
    take(in24, in);
    int32_t a = at, t = tc, l = lcss;
    uint32_t p = 0;
    const int r = onair_dmr_cach_decode(in, &a, &t, &l, &p);
    payload17 = p;
    if (r >= 0) { at = a; tc = t; lcss = l; }
    return r;
}

void trellis34Encode(const uint8_t data[18], Bits& tx196) {
    tx196.assign(196, 0);
    onair_dmr_trellis34_encode(data, tx196.data());
}
int trellis34Decode(const float symbols[98], uint8_t data[18]) { return onair_dmr_trellis34_decode(symbols, data); }

void rate1Encode(const uint8_t data[24], Bits& tx196) {
    tx196.assign(196, 0);
    onair_dmr_rate1_encode(data, tx196.data());
}
void rate1Decode(const Bits& tx196, uint8_t data[24], bool* padOk) {
    uint8_t in[196];
    take(tx196, in);
    const bool ok = onair_dmr_rate1_decode(in, data);
    if (padOk) *padOk = ok;
}

} // namespace dmr
} // namespace dect2
