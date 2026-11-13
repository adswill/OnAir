// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the pager layer, meant for DECT2_SANITIZE=ON: code words, text readers, the POCSAG
// parser and the FLEX phase decoder must never crash or hang on any bits. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/pager/pager_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace P = dect2::pager;

static void one(const uint8_t* d, size_t n) {
    std::vector<uint8_t> bits(d, d + n);
    P::pocsagLooksAlpha(bits);
    P::alphaFromBits(bits);
    P::numericFromBits(bits, false);
    P::numericFromBits(bits, true);
    P::PocsagParser pp;
    std::vector<dect2::PagerMessage> out;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, d + i, 4);
        P::correctWord(w); P::rev21(w); P::pocsagClassify(w);
        pp.word(w, (int)((i / 4) & 15), out);
    }
    pp.flush(out);
    int c, f;
    if (n >= 4) { uint32_t w; memcpy(&w, d, 4); P::flexFiwCheck(w, c, f); P::flexModeForCode((uint16_t)w, (int)(w >> 16) % 6); P::flexFiwData((int)w, (int)(w >> 16)); }
    std::vector<uint8_t> full(2816, 0);
    memcpy(full.data(), d, n < full.size() ? n : full.size());
    P::FlexPhaseStat st;
    P::flexDecodePhase(full.data(), out, st);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 200000;
    std::mt19937 g(555);
    std::vector<dect2::pager::FlexPage> pages(3);
    pages[0].cap = 100; pages[0].type = dect2::kPagerAlpha; pages[0].text = "Hello fuzzer";
    pages[1].cap = 200; pages[1].type = dect2::kPagerNumeric; pages[1].text = "12345 678";
    pages[2].cap = 300; pages[2].type = dect2::kPagerTone;
    const std::vector<uint8_t> seed = dect2::pager::flexPhaseBits(pages);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> b = (g() % 3) ? seed : std::vector<uint8_t>();
        if (b.empty()) { b.resize(1 + g() % 3000); for (auto& x : b) x = (uint8_t)(g() & 1); }
        const int k = 1 + (int)(g() % 40);
        for (int j = 0; j < k; j++) b[g() % b.size()] ^= (uint8_t)(1 + (g() % 255));   // bytes above 1 too: only bit 0 counts
        if (g() % 8 == 0) b.resize(g() % (b.size() + 1));
        one(b.data(), b.size());
    }
    printf("fuzz_pager_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
