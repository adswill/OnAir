// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the P25 frame layer, meant for DECT2_SANITIZE=ON: NID, Reed-Solomon, Golay, trellis, TSBK,
// link control, header / encryption sync words and the IMBE interleave must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/p25/p25_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace P = dect2::p25;

static void one(const uint8_t* d, size_t n) {
    if (n < 4) return;
    auto at = [&](size_t i) { return d[i % n]; };
    uint8_t bits[96];
    for (int i = 0; i < 96; i++) bits[i] = at((size_t)i) & 1;
    int nac, duid, errs;
    P::nidDecode(bits, nac, duid, &errs);
    P::nidEncode(at(0) << 8 | at(1), at(2), bits);
    uint8_t sym[64];
    for (int i = 0; i < 64; i++) sym[i] = at((size_t)i) & 63;
    const int rn = 1 + at(0) % 63, rk = at(1) % (rn + 1);
    P::rsEncode(sym, rn, rk);
    P::rsDecode(sym, rn, rk, &errs);
    P::hamming10Decode((uint16_t)(at(0) << 8 | at(1)), &errs);
    P::golay24Decode(at(0) << 16 | at(1) << 8 | at(2), &errs);
    P::golay18Decode(at(0) << 8 | at(1), &errs);
    P::crcCcitt(bits, (int)(at(2) % 97));
    float lv[98];
    for (int i = 0; i < 98; i++) lv[i] = ((float)at((size_t)i) - 128.f) / 24.f;
    uint8_t out[98], b96[96];
    float metric;
    P::trellisDecode(lv, b96, &metric);
    P::trellisEncode(bits, out);
    P::Tsbk t;
    P::tsbkParse(bits, t);
    P::tsbkBits(t, bits);
    uint8_t hex[20];
    for (int i = 0; i < 20; i++) hex[i] = at((size_t)i) & 63;
    P::Lc l;
    P::lcParse(hex, l);
    P::lcHexbits(l, hex);
    P::Crypto c;
    P::esParse(hex, c); P::hduParse(hex, c); P::esHexbits(c, hex); P::hduHexbits(c, hex);
    uint8_t dib[72];
    for (int i = 0; i < 72; i++) dib[i] = at((size_t)i) & 3;
    char fr[8][23];
    P::imbeDeinterleave(dib, fr);
    uint8_t flat[144];
    P::imbeFlatten(fr, flat);
    P::imbeUnflatten(flat, fr);
    P::imbeInterleave(fr, dib);
    P::channelText((int)(at(0) << 8 | at(1)));
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 50000;
    std::mt19937 g(777);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(4 + g() % 120);
        for (auto& x : s) x = (uint8_t)(g() % 3 ? g() : (g() & 1));
        one(s.data(), s.size());
    }
    printf("fuzz_p25_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
