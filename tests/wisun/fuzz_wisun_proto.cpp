// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the Wi-SUN frame layers, meant for DECT2_SANITIZE=ON: parse() of any bytes, the PHY helpers, the name
// functions and scheduleText must never crash or hang. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/wisun/wisun_proto.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2::wisun;

static void one(const uint8_t* d, size_t n) {
    const Frame f = parse(d, n);
    if (f.ok && f.payloadAt + f.payloadLen > n) abort();
    srcText(f); dstText(f);
    std::vector<uint8_t> v(d, d + n);
    whiten(v.data(), v.size());
    fcsOk(d, n, n & 1);
    crc16(d, n); crc32(d, n);
    if (n >= 2) { const Phr p = parsePhr((uint16_t)(d[0] | d[1] << 8)); buildPhr(p); }
    Schedule s = f.us.present ? f.us : f.bs;
    scheduleText(s);
    if (n >= 3) { uttName(d[0]); uttShort(d[1]); chFunctionName(d[2]); regDomainName(d[0]); }
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 50000;
    std::mt19937 g(1357);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(g() % 3000);
        for (auto& b : s) b = (uint8_t)(g() % 4 ? g() : 0x2A);
        if (s.size() >= 2 && g() % 2) { s[0] = (uint8_t)(g() % 2 ? 0x01 : g()); s[1] = (uint8_t)(0x22 | (g() % 3 ? 0x20 : 0)); }
        one(s.data(), s.size());
    }
    printf("fuzz_wisun_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
