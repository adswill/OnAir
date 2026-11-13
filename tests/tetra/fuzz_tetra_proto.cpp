// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the TETRA network information layer, meant for DECT2_SANITIZE=ON: SYNC, SYSINFO, the
// signalling block walk, texts and carrier maths must never crash or hang on any bytes (also bytes above 1 and every length). Also a libFuzzer target with
// -DONAIR_LIBFUZZER.
#include "dect2/tetra/tetra_pdu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace T = dect2::tetra;

static void one(const uint8_t* d, size_t n) {
    if (n < 4) return;
    std::vector<uint8_t> bits(d, d + n), bin(n);
    for (size_t i = 0; i < n; i++) bin[i] = d[i] & 1;
    for (const auto* v : {&bits, &bin}) {
        T::Sync s; T::parseSync(*v, s);
        T::Sysinfo si; T::parseSysinfo(*v, si);
        T::parseSecurity(si);
        T::MacBlock m; T::parseMacBlock(*v, m);
    }
    T::serviceText((int)(d[0] << 8 | d[1]));
    T::networkTimeText((uint64_t)d[0] << 40 ^ (uint64_t)d[1] << 24 ^ d[2] << 8 ^ d[3]);
    T::dlCarrierHz((int)d[0] - 100, (int)d[1] * 60, (int)d[2] - 50);
    T::ulCarrierHz((int)d[0] - 100, (int)d[1] * 60, (int)d[2] - 50, (int)d[3] - 100, d[0] & 1);
    T::dlUsageText(T::aachDlUsage((uint16_t)(d[0] << 8 | d[1]), d[2] & 1));
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* e = getenv("ONAIR_FUZZ_ITERS");
    const long iters = e ? atol(e) : 30000;
    std::mt19937 g(1357);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> v(g() % 400);
        const int mode = (int)(g() % 3);
        for (auto& x : v) x = mode == 0 ? (uint8_t)g() : (uint8_t)(g() & 1);
        one(v.data(), v.size());
    }
    printf("fuzz_tetra_proto: %ld inputs ok\n", iters);
    return 0;
}
#endif
