// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone tables and the DCS word: the Golay (23,12) code word of 023 against the published pattern, every code, rotations, polarity, the lists.
#include "dect2/tones/tones_proto.h"
#include <cmath>
#include <cstdio>

using namespace dect2::tones;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static uint32_t rotl(uint32_t w, int r) { return r ? (((w << r) | (w >> (23 - r))) & 0x7FFFFF) : w; }

int main() {
    // CTCSS list: 50 tones ascending, 12 extended, lookups
    int ext = 0;
    for (int i = 0; i < kCtcssN; i++) {
        if (i) CHECK(ctcssHz(i) > ctcssHz(i - 1), "ctcss %d not ascending", i);
        if (ctcssExtended(i)) ext++;
        CHECK(ctcssIndex(ctcssHz(i) + 0.4) == i, "ctcss lookup %d", i);
    }
    CHECK(ext == 12, "12 extended tones, got %d", ext);
    CHECK(ctcssIndex(100.0) >= 0 && ctcssHz(ctcssIndex(100.0)) == 100.0, "100.0 Hz");
    CHECK(ctcssIndex(140.0) < 0, "140 Hz is no tone");
    // DCS: the word of 023 is 0x763813 (check bits 11101100011, marker 100, code 000 010 011)
    CHECK(dcsWord(23) == 0x763813u, "023 word %06x", dcsWord(23));
    int code = 0;
    CHECK(dcsValid(0x763813u, code) && code == 23, "023 valid");
    CHECK(!dcsValid(0x763813u ^ 1u, code), "one bit error is not valid");
    // every code: encode, validate, decode at every rotation
    int std83 = 0;
    for (int c = 0; c < 512; c++) {
        const int w = ((c >> 6) & 7) * 100 + ((c >> 3) & 7) * 10 + (c & 7);
        const uint32_t word = dcsWord(w);
        int back = -1;
        CHECK(dcsValid(word, back) && back == w, "code %03d", w);
        for (int r = 0; r < 23; r += 5) {
            DcsHit h;
            CHECK(dcsDecode(rotl(word, r), h), "decode %03d rot %d", w, r);
            if (dcsStandard(w) && !h.inverted && h.code != w) CHECK(false, "decode %03d rot %d gave %03d", w, r, h.code);
        }
        // inverted waveform: reads as this code with the inverted flag, or as another standard code's normal word (the aliases)
        DcsHit h;
        CHECK(dcsDecode(~word & 0x7FFFFF, h), "inverted %03d", w);
        if (dcsStandard(w)) {
            std83++;
            const bool direct = h.code == w && h.inverted;
            const bool viaAlias = h.aliasCode == w && h.aliasInverted;
            CHECK(direct || viaAlias || (!h.inverted), "inverted %03d read as %03d%s alias %d", w, h.code, h.inverted ? "I" : "N", h.aliasCode);
        }
    }
    CHECK(std83 == 83, "83 standard codes, got %d", std83);
    DcsHit h;
    CHECK(dcsDecode(dcsWord(23), h) && h.code == 23 && !h.inverted && h.standard, "023N decode");
    CHECK(h.aliasCode == 47 && h.aliasInverted, "023 reads as 047I too: %d", h.aliasCode);
    CHECK(dcsName(23, false) == "D023N" && dcsName(754, true) == "D754I", "names");
    // the sequential plans
    const auto& pool = seqPool();
    CHECK(pool.size() > 30 && pool.size() < 60, "pool size %zu", pool.size());
    for (size_t i = 1; i < pool.size(); i++) CHECK(pool[i] > pool[i - 1], "pool ascending");
    CHECK(kSeqSetN == 6, "six plans");
    // SELCAL: 16 tones, letters without I, N, O
    CHECK(selcalLetter(0) == 'A' && selcalLetter(8) == 'J' && selcalLetter(12) == 'P' && selcalLetter(15) == 'S', "selcal letters");
    for (int i = 1; i < kSelcalN; i++) CHECK(selcalHz(i) > selcalHz(i - 1), "selcal ascending");
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("tones proto: ok\n");
    return 0;
}
