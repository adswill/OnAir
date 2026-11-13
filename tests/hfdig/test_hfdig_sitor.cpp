// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR decoder (hfdig_sitor.h): the FEC-B word table round trip, a generated FEC-B character sequence with letters and figures
// shifts, invalid words, and the FEC-A time-diversity acceptance. Pure logic, no audio, silent.
#include "dect2/hfdig/hfdig_sitor.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

// ITA2 5-bit codes used by the test message (letters shift unless noted).
enum : uint8_t { L_E = 1, L_SP = 4, L_D = 9, L_C = 14, L_Q = 23, L_T = 16, L_LTRS = 31, L_FIGS = 27 };
const uint8_t kFive = 16;   // '5' in the figures shift

int main() {
    // 1. Every code has a four-of-seven word, the words are distinct, and they decode back.
    {
        std::vector<int> seen(128, 0);
        for (uint8_t c = 0; c < 32; c++) {
            uint8_t w = sitorFecBEncode(c);
            int ones = 0;
            for (int b = 0; b < 7; b++) ones += (w >> b) & 1;
            CHECK(ones == 4, "code %d word %d has %d marks", c, w, ones);
            CHECK(!seen[w], "word %d used twice", w);
            seen[w] = 1;
            CHECK(sitorFecBDecode(w) == c, "round trip code %d", c);
        }
    }

    // 2. Invalid words (three or five marks, out of range) are not characters.
    CHECK(sitorFecBDecode(0x7F) == -1, "seven marks accepted");
    CHECK(sitorFecBDecode(0x01) == -1, "one mark accepted");
    CHECK(sitorFecBDecode(200) == -1, "out of range accepted");

    // 3. FEC-B sequence "CQ DE" then FIGS '5' then LTRS 'T': 8 characters, no errors.
    {
        const uint8_t codes[] = {L_C, L_Q, L_SP, L_D, L_E, L_SP, L_FIGS, kFive, L_LTRS, L_T};
        SitorDecoder dec(SitorDecoder::Mode::FecB);
        for (uint8_t c : codes) dec.push(sitorFecBEncode(c));
        CHECK(dec.text() == "CQ DE 5T", "FEC-B text \"%s\"", dec.text().c_str());
        CHECK(dec.chars() == 8, "FEC-B chars %u", dec.chars());
        CHECK(dec.errors() == 0, "FEC-B errors %u", dec.errors());
    }

    // 4. A corrupted word in FEC-B is counted, not printed.
    {
        SitorDecoder dec(SitorDecoder::Mode::FecB);
        dec.push(sitorFecBEncode(L_C));
        dec.push(0x7F);
        dec.push(sitorFecBEncode(L_Q));
        CHECK(dec.text() == "CQ", "FEC-B with error \"%s\"", dec.text().c_str());
        CHECK(dec.errors() == 1, "FEC-B error count %u", dec.errors());
    }

    // 5. FEC-A (lag 3): each character sent once per period of three is printed when the copy arrives; a lone copy is not.
    {
        SitorDecoder dec(SitorDecoder::Mode::FecA, 3);
        const uint8_t a = sitorFecBEncode(L_C), b = sitorFecBEncode(L_Q), c = sitorFecBEncode(L_D);
        dec.push(a); dec.push(b); dec.push(c);
        CHECK(dec.text().empty(), "FEC-A printed before the repeat: \"%s\"", dec.text().c_str());
        dec.push(a); dec.push(b); dec.push(c);
        CHECK(dec.text() == "CQD", "FEC-A text \"%s\"", dec.text().c_str());
    }
    {
        SitorDecoder dec(SitorDecoder::Mode::FecA, 3);
        dec.push(sitorFecBEncode(L_C));
        dec.push(0x7F);
        dec.push(sitorFecBEncode(L_Q));
        dec.push(0x7F);
        dec.push(sitorFecBEncode(L_D));
        dec.push(0x7F);
        CHECK(dec.text().empty(), "FEC-A printed a lone copy: \"%s\"", dec.text().c_str());
    }

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
