// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the error correction layer, meant for DECT2_SANITIZE=ON: the Reed-Solomon decoders and the BCH
// decoder must never crash or hang on any bytes (the BCH bits are also fed values above 1 and every length from 0), the encoders on any message length.
// Buffers are exact-size heap blocks so a sanitizer sees a read or write past the end. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/fec/fec_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace F = dect2::fecp;

static F::Bch& bch(bool sh, int t) {
    static F::Bch* c[2][13] = {};
    if (!c[sh][t]) c[sh][t] = new F::Bch(sh, t);
    return *c[sh][t];
}

static void one(const uint8_t* d, size_t n) {
    auto exact = [&](size_t len) { uint8_t* p = (uint8_t*)malloc(len ? len : 1); for (size_t i = 0; i < len; i++) p[i] = n ? d[i % n] : 0; return p; };
    uint8_t* w = exact(204); F::rs204Decode(w); free(w);
    w = exact(207); F::rs207Decode(w); free(w);
    w = exact(120); F::rs120Decode(w); free(w);
    uint8_t* m = exact(188); uint8_t* o = (uint8_t*)malloc(204); F::rs204Encode(m, o); free(m); free(o);
    m = exact(187); o = (uint8_t*)malloc(207); F::rs207Encode(m, o); free(m); free(o);
    m = exact(110); o = (uint8_t*)malloc(120); F::rs120Encode(m, o); free(m); free(o);
    {   // Viterbi on an exact-size block: any bytes, any step count the data allows
        const size_t steps = n / 2;
        int8_t* sv = (int8_t*)malloc(2 * steps + 1);
        memcpy(sv, d, 2 * steps);
        std::vector<uint8_t> bits; long met;
        F::viterbiRange(sv, steps, bits, &met);
        free(sv);
    }
    if (n >= 6) {   // LDPC tables from random bytes: k and n small multiples of 360 plus a few bytes of slack, groups of random addresses (negatives and big ones included)
        const int k = (d[0] % 4) * 360, nn = k + (d[1] % 5) * 360 + (d[2] & 1 ? d[3] : 0);
        std::vector<std::vector<int>> rows(1 + d[4] % 5);
        size_t pos = 5;
        for (auto& r : rows) for (int j = 0; j < 1 + d[5] % 7 && pos + 1 < n; j++, pos += 2) r.push_back((int)(int16_t)(d[pos] | (d[pos + 1] << 8)));
        F::LdpcTables t; F::ldpcBuild(k, nn, rows, t);
    }
    if (n < 3) return;
    const bool sh = d[0] & 1; const int t = 1 + d[1] % 12;
    std::vector<uint8_t> bits(d + 2, d + n);
    for (auto& b : bits) b &= 1;                          // the codes work on bits 0 / 1
    std::vector<uint8_t> e = bits;
    bch(sh, t).encode(e, (int)(bits.size() / 2));
    bch(sh, t).decode(bits);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 100000;
    std::mt19937 g(2468);
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b(g() % 3 == 0 ? g() % 40 : g() % 3000);
        for (auto& x : b) x = (uint8_t)g();
        one(b.data(), b.size());
    }
    printf("fuzz_fec_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
