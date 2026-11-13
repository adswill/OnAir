// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the HFDL layers, meant for DECT2_SANITIZE=ON: the PDU parser, the FCS, the convolutional decoder with
// arbitrary soft values and the mask / station functions must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/hfdl/hfdl_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;

static void one(const uint8_t* d, size_t n) {
    std::vector<HfdlMessage> out;
    hfdlParsePdu(d, n, out);
    if (n >= 2) { hfdlFcs(d, n - 2); hfdlFcsOk(d, n - 2); }
    if (n >= 5) {
        const uint32_t mask = (uint32_t)(d[1] | d[2] << 8 | d[3] << 16);
        hfdlMaskKhz(d[0], mask);
        hfdlStationOnKhz(d[1] * 100 + d[2]);
    }
    static unsigned tick = 0;
    if (++tick % 32 == 0) {   // the full-size decoders are slow: every 32nd input
    const int m1 = n ? d[0] % 10 - 1 : 0;
    const int N = hfdlCodedBits(m1);
    std::vector<float> chan((size_t)N);
    for (size_t i = 0; i < chan.size(); i++) {
        float f; uint32_t u = (uint32_t)(n ? d[i % n] : 0) * 0x01010101u + (uint32_t)i * 2654435761u;
        if (n && d[0] % 3 == 0) { memcpy(&f, &u, 4); } else f = (float)((int)(u >> 20) - 2048) / 100.f;
        chan[i] = f;
    }
    hfdlDecodeSoft(m1, chan);
    }
    std::vector<float> chan(1200);
    for (size_t i = 0; i < chan.size(); i++) chan[i] = n ? (float)((int)d[i % n] - 128) / 16.f : 0.f;
    std::vector<uint8_t> bits(n * 4);
    for (size_t i = 0; i < bits.size(); i++) bits[i] = (uint8_t)(d[i % n] >> (i % 8));
    hfdlConvEncode(bits);
    hfdlViterbi(chan, (int)(n % 600));
    hfdlViterbi(chan, -(int)n);
    hfdlViterbi(chan, 700);
    std::vector<int8_t> s;
    hfdlScramblerSigns((int)(n * 5), s);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 20000;
    std::mt19937 g(5150);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(g() % 300);
        for (auto& b : s) b = (uint8_t)(g() % 3 ? g() : (g() % 2 ? 0x00 : 0xFF));
        if (s.size() >= 4 && g() % 2) { s[0] = (uint8_t)(g() % 2 ? 0x03 | ((g() % 16) << 2) : 0x01 | ((g() % 8) << 4)); }
        if (s.size() >= 66 && g() % 3 == 0) { s[0] &= ~1; const uint16_t f = hfdlFcs(s.data(), 64); s[64] = (uint8_t)f; s[65] = (uint8_t)(f >> 8); }
        one(s.data(), s.size());
    }
    printf("fuzz_hfdl_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
