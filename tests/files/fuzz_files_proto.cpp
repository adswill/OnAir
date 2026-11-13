// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the file format layer, meant for DECT2_SANITIZE=ON: every parser must never crash or hang on any bytes, at
// any length (the input is copied to an exactly sized heap block). Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/files/files_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace F = dect2::filesp;

static void one(const uint8_t* d, size_t n) {
    uint8_t* p = n ? (uint8_t*)malloc(n) : nullptr;
    if (n) memcpy(p, d, n);
    const std::string text((const char*)p, n);
    F::csvParse(text);
    F::csvQuote(text);
    F::splitList(text, text.empty() ? ',' : text[0]);
    F::sigmfParse(text);
    F::tleScan(text);
    if (n >= 2) F::tleParse(text.substr(0, n / 2), text.substr(n / 2), text.substr(0, n / 3));
    F::wavFmt(p, n, n ? p[0] : 0);
    F::wavCenterFromName(text);
    std::vector<uint8_t> v(p, p + n);
    std::vector<F::Col> cols;
    F::record(v, cols, true);
    std::string sq;
    F::tilesSchemaOk(text, sq);
    F::indexCovers(text, sq);
    F::isRaster(v);
    for (int kind : {13, 5, 6, 10, 2}) {
        uint8_t type; unsigned cells; uint32_t right;
        const size_t base = n > 0 ? p[0] % 3 * 50 : 0;
        if (F::pageHeader(v, base, type, cells, right)) for (size_t i = 0; i <= cells && i < 40; i++) { F::CellInfo c; F::cell(v, kind, base, i, cells, c); }
    }
    if (n >= 100) F::dbHeader(p, n);
    F::containerCheck(text, n >= 8 ? *(const uint64_t*)p : 0);
    free(p);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 100000;
    std::mt19937 g(8642);
    const std::string seeds[] = {
        "{\"global\":{\"core:datatype\":\"cf32_le\",\"core:sample_rate\":2.4e6,\"core:description\":\"a\\\"b\\u0041\"},\"captures\":[{\"core:sample_start\":0,\"core:frequency\":1e8}],\"annotations\":[{}]}",
        "ISS\n1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927\n2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537\n",
        "city,freq_mhz\nA,\"b,\"\"c\",3\n\n",
    };
    for (long it = 0; it < iters; it++) {
        std::string s = seeds[g() % 3];
        const int k = (int)(g() % 6);
        for (int i = 0; i < k && !s.empty(); i++) {
            switch (g() % 5) {
            case 0: s[g() % s.size()] = (char)g(); break;
            case 1: s.resize(g() % (s.size() + 1)); break;
            case 2: s.insert(s.begin() + g() % (s.size() + 1), (char)g()); break;
            case 3: s.erase(s.begin() + g() % s.size()); break;
            default: for (auto& c : s) if (g() % 8 == 0) c = (char)g();
            }
        }
        one((const uint8_t*)s.data(), s.size());
        if (g() % 3 == 0) { std::vector<uint8_t> r(g() % 700); for (auto& x : r) x = (uint8_t)g(); one(r.data(), r.size()); }
    }
    printf("fuzz_files_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
