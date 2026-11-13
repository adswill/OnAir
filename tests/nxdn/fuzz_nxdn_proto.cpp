// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the NXDN frame layer, meant for DECT2_SANITIZE=ON: LICH, CRC, the coded channels, message
// parse / build and the voice bit order must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/nxdn/nxdn_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace N = dect2::nxdn;

static void one(const uint8_t* d, size_t n) {
    if (n < 2) return;
    // the bytes as bits: parse, rebuild, CRC
    std::vector<uint8_t> bits(d, d + n);
    N::Msg m;
    N::parseMsg(bits.data(), (int)bits.size() - (int)(d[0] % 3), m);
    uint8_t out[160];
    N::buildMsg(m, out, (int)(d[1] % 160));
    N::msgName(m.type); N::callTypeName(m.callType);
    N::crc6(bits.data(), (int)n); N::crc12(bits.data(), (int)n); N::crc16(bits.data(), (int)n);
    N::Lich l;
    N::lichDecode(d[0], l);
    N::lichEncode(l);
    // the bytes as soft values for each channel
    const N::Ch chs[3] = {N::Ch::Sacch, N::Ch::Facch1, N::Ch::Cac};
    for (N::Ch c : chs) {
        std::vector<float> soft((size_t)N::airBits(c));
        for (size_t i = 0; i < soft.size(); i++) soft[i] = ((float)d[i % n] - 128.f) / 16.f * (i & 1 ? 1.f : -1.f);
        uint8_t info[160]; int e = 0;
        N::decode(c, soft.data(), info, &e);
        uint8_t air[300];
        N::encode(c, info, air);
    }
    uint8_t v[72]; char fr[4][24];
    for (int i = 0; i < 72; i++) v[i] = d[i % n];
    N::ambeFrame(v, fr);
    uint8_t pn[200];
    N::pn9(pn, (int)(n % 200));
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 100000;
    std::mt19937 g(777);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(2 + g() % 120);
        for (auto& x : s) x = (uint8_t)(g() % 3 ? (g() & 1) : g());
        if (g() % 2) { s[0] = 0; s[1] = 0; s[2 % s.size()] = 0; }
        one(s.data(), s.size());
    }
    printf("fuzz_nxdn_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
