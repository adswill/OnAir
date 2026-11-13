// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the ACARS block layer, meant for DECT2_SANITIZE=ON: repair and parse must never crash
// or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/acars/acars_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;

static unsigned long long sink = 0;

static void one(const uint8_t* d, size_t n) {
    std::vector<uint8_t> t(d, d + n);
    AcarsMessage m;
    sink += acarsParseBlock(t.data(), (int)t.size(), m) + m.text.size() + m.decoded.size();
    if (n >= 2) {
        const uint8_t crc[2] = {d[0], d[1]};
        int fixed = 0, perr = 0;
        std::vector<uint8_t> r(d + 2, d + n);
        sink += acarsRepairBlock(r.data(), (int)r.size(), crc, fixed, perr, (int)(d[0] % 5)) + (unsigned)fixed + (unsigned)perr;
    }
    if (n >= 3) sink += acarsDecodeText(std::string((const char*)d, 2), std::string((const char*)d + 2, n - 2)).size();
    sink += acarsCrc(d, n) + acarsSyndrome((int)n - 3, (int)(n % 9));
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 200000;
    std::mt19937 g(99);
    AcarsBlockSpec s;
    s.reg = "N12345"; s.text = "M01AEK0201OMDB1030 hello\r\nworld"; s.label[0] = 'Q'; s.label[1] = 'A';
    const std::vector<uint8_t> frame = acarsBuildFrame(s, 0);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> v(frame.begin() + 3, frame.end() - 1);
        const int k = (int)(g() % 5);
        for (int j = 0; j < k; j++) {
            switch (g() % 4) {
            case 0: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
            case 1: v[g() % v.size()] = (uint8_t)g(); break;
            case 2: v.resize(g() % (v.size() + 1)); if (v.empty()) v.push_back(0); break;
            default: v.insert(v.begin() + (long)(g() % (v.size() + 1)), (uint8_t)g()); break;
            }
        }
        if (g() % 8 == 0) v.assign(g() % 400, (uint8_t)g());
        if (v.empty()) v.push_back(0);
        one(v.data(), v.size());
    }
    printf("fuzz_acars_proto: %ld inputs, no crash (%llu)\n", iters, sink & 1);
    return 0;
}
#endif
