// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the SAME layer, meant for DECT2_SANITIZE=ON: parse, trim, locate, time text and the
// county reader must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/same/same_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
namespace S = dect2::same;

static void one(const std::string& s) {
    S::trimMessage(s);
    for (int c = 0; c < 2; c++) {
        const S::Message m = S::parse(s, (S::Country)c);
        for (const auto& l : m.locs) if (l.text.empty() && l.code.size() == 6) abort();
        S::locate(s, (S::Country)c);
    }
    if (s.size() >= 4) {
        int v[4];
        memcpy(v, s.data(), 4);
        S::issueText(1970 + (uint8_t)s[0] * 4, v[0] % 400, (uint8_t)s[1] % 30, (uint8_t)s[2] % 70);
        S::purgeText(v[0]);
    }
    std::string err;
    S::loadCountyCsv(s, &err);
    S::clearCounties();
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(std::string((const char*)d, n)); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 200000;
    std::mt19937 g(777);
    const std::string seed = "ZCZC-WXR-TOR-029001-029003+0030-1231415-KEAX/NWS-";
    static const char alpha[] = "ZCNW-+0123456789/ ABCXYZ,\"\r\n\x01\xff";
    for (long i = 0; i < iters; i++) {
        std::string s = (g() % 3) ? seed : std::string();
        const int k = 1 + (int)(g() % 6);
        for (int j = 0; j < k; j++) {
            switch (g() % 5) {
            case 0: if (!s.empty()) s[g() % s.size()] = (char)(g() & 0xFF); break;
            case 1: if (!s.empty()) s.erase(g() % s.size(), 1 + g() % 4); break;
            case 2: s.insert(g() % (s.size() + 1), 1, alpha[g() % (sizeof alpha - 1)]); break;
            case 3: s.resize(g() % (s.size() + 1)); break;
            default: for (int q = (int)(g() % 40); q > 0; q--) s += alpha[g() % (sizeof alpha - 1)]; break;
            }
        }
        one(s);
    }
    printf("fuzz_same_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
