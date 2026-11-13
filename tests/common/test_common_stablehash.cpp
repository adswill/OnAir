// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dect2::stable_string_hash (dect2/common/stable_random.h) must give libc++'s std::hash<std::string> value on every platform: test_specx_idmatrix
// seeds each case's offsets and noise from the hash of the case's label, and with libstdc++'s own std::hash (Linux, MinGW) it drew other numbers
// than on macOS and failed there (frequency known top-1 at 10 dB 40.6 %, digital voice top-1 at 20 dB 72.2 %). The expected values below were
// taken from libc++'s std::hash on macOS arm64; on macOS the test also compares against std::hash directly on random strings of 0 to 64 bytes.
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    static const struct { const char* s; unsigned long long h; } kRef[] = {
        {"", 0x9ae16a3b2f90404fULL},
        {"a", 0x2420662cd003acfaULL},
        {"fm", 0x326526f3e9d53fb7ULL},
        {"dmr", 0x1e8b5b61dda16cf5ULL},
        {"dvbt2", 0xa87b7f0098e1c9ebULL},
        {"meshtastic", 0x83791b99654ef3edULL},
        {"contestia", 0xd4186dc17b9b04ceULL},
        {"0123456789abcdef", 0x099d21e99dac3317ULL},
        {"loraaprs-meshcom-x", 0xef2d20d288b49376ULL},
        {"a label of twenty-nine bytes!", 0x4644a729dfe47651ULL},
        {"a much longer label that runs to fifty-five bytes long", 0xddc9361b5c24891aULL},
    };
    for (const auto& r : kRef) {
        const std::string s(r.s);
        const unsigned long long h = dect2::stable_string_hash(s.data(), s.size());
        CHECK(h == r.h, "\"%s\" (%zu bytes): 0x%016llx, libc++ gives 0x%016llx", r.s, s.size(), h, r.h);
    }
    // the whole chain test_specx_idmatrix uses: label -> seed -> mt19937 -> dect2::normal_distribution (libc++'s numbers everywhere)
    static const struct { const char* l; double v; } kDraw[] = {{"dmr", -0.91263140552553057}, {"pocsag", -0.26865040276171537}, {"meshtastic", 1.2303315302934876}};
    for (const auto& d : kDraw) {
        const std::string s(d.l);
        std::mt19937 g(0x9E3779B9u * (uint32_t)(dect2::stable_string_hash(s.data(), s.size()) + 17));
        const double v = dect2::normal_distribution<double>(0, 1)(g);
        CHECK(std::fabs(v - d.v) < 1e-12, "%s: first draw %.17g, macOS gives %.17g", d.l, v, d.v);   // the same draw; log / sqrt of the libm may differ in the last bit
    }
#if defined(_LIBCPP_VERSION) && SIZE_MAX == UINT64_MAX
    std::mt19937 g(1);
    int bad = 0;
    for (int len = 0; len <= 64; len++)
        for (int t = 0; t < 100; t++) {
            std::string s((size_t)len, ' ');
            for (char& c : s) c = (char)(g() & 0xff);
            bad += (unsigned long long)std::hash<std::string>()(s) != dect2::stable_string_hash(s.data(), s.size());
        }
    CHECK(bad == 0, "%d of 6500 random strings hash differently from libc++'s std::hash", bad);
#endif
    printf("common_stablehash: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
