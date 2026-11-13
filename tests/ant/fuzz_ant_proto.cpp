// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT frame layer under mutation: libFuzzer entry point (-DONAIR_LIBFUZZER) or a deterministic seeded mutation run (ONAIR_FUZZ_ITERS, default 200000).
#include "dect2/ant/ant_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace dect2::ant;

static unsigned long long sink = 0;

static void exercise(const uint8_t* d, size_t n) {
    uint8_t fr[kFrameBytes] = {};
    memcpy(fr, d, n < sizeof fr ? n : sizeof fr);
    Frame f;
    sink += parseFrame(fr, f) + f.crcOk + crc16(d, n);
    uint8_t out[kFrameBytes];
    buildFrame(f, out);
    sink += out[16];
    std::vector<uint8_t> bits;
    frameBits(f, bits);
    sink += bits.size();
    static const int types[] = {120, 121, 122, 123, 124, 11, 17, 15, 119, 16, 25, 20, 18, 19, 31, 40, 35, 34, 0, 127};
    uint8_t pl[kPayload] = {};
    memcpy(pl, d, n < kPayload ? n : kPayload);
    for (int t : types) { const Reading r = decodePayload(t, pl); sink += r.text.size() + r.pageName.size(); }
    sink += decodePayload(n ? d[0] : 0, pl).text.size() + hex(d, n).size();
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { exercise(d, n); return 0; }
#else
int main() {
    const char* e = getenv("ONAIR_FUZZ_ITERS");
    const long iters = e ? atol(e) : 200000;
    std::mt19937 g(777);
    std::vector<std::vector<uint8_t>> seeds;
    for (int t : {120, 121, 11, 124, 25, 17}) for (int pg : {0, 1, 2, 3, 4, 7, 16, 80, 81, 82}) {
        Frame f; f.marker = kAntPlusMarker; f.deviceNumber = (uint16_t)g(); f.deviceType = (uint8_t)t; f.transType = 1;
        for (int i = 0; i < kPayload; i++) f.payload[i] = (uint8_t)g();
        f.payload[0] = (uint8_t)pg;
        uint8_t b[kFrameBytes]; buildFrame(f, b);
        seeds.push_back(std::vector<uint8_t>(b, b + kFrameBytes));
    }
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> v = seeds[g() % seeds.size()];
        const int k = 1 + (int)(g() % 4);
        for (int j = 0; j < k; j++) {
            switch (g() % 4) {
            case 0: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
            case 1: v[g() % v.size()] = (uint8_t)g(); break;
            case 2: if (v.size() > 1) v.pop_back(); break;
            default: v.push_back((uint8_t)g()); break;
            }
        }
        exercise(v.data(), v.size());
    }
    printf("fuzz_ant_proto: %ld iterations ok (%llu)\n", iters, sink);
    return 0;
}
#endif
