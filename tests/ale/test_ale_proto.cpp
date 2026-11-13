// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE word and call layer: Golay (every pattern of up to 3 errors corrected, 4 detected), the Gray tones, a word through the vote
// with one copy wrong, the long addresses, an AMD text and an LQA report through the assembler.
#include "dect2/ale/ale_proto.h"
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::ale;
#include "../common/test_util.h"

static void feedCall(Assembler& a, const std::vector<uint32_t>& ws) {
    for (uint32_t w : ws) { uint8_t b[147]; wordBits(w, b); Word x = decodeBits(b); CHECK(x.ok && x.w == w, "word %06X", w); a.add(x); }
}

int main() {
    std::mt19937 rng(3);
    // Golay: all 12-bit values, up to 3 errors at random places; 4 errors never decode to the wrong value silently with weight <= 3
    int bad = 0, det4 = 0;
    for (uint32_t d = 0; d < 4096; d++) {
        const uint32_t c = golayEncode(d);
        for (int e = 0; e <= 3; e++) {
            uint32_t m = 0;
            while (__builtin_popcount(m) < e) m |= 1u << (rng() % 24);
            uint32_t out;
            const int n = golayDecode(c ^ m, out);
            if (n != e || out != d) bad++;
        }
        uint32_t m = 0;
        while (__builtin_popcount(m) < 4) m |= 1u << (rng() % 24);
        uint32_t out;
        if (golayDecode(c ^ m, out) < 0) det4++;
    }
    CHECK(bad == 0, "%d Golay failures", bad);
    CHECK(det4 == 4096, "4 errors detected %d / 4096", det4);
    // tones: Gray, the inverse
    for (int t = 0; t < 8; t++) CHECK(tone(tribit(t)) == t, "tone %d", t);
    CHECK(tribit(0) == 0 && tribit(7) == 4 && tribit(2) == 3, "Gray map");
    // a word, with one of the three copies of every bit wrong in some places: the vote counts them
    const uint32_t w = makeWord(To, 'L', 'Y', '2');
    uint8_t b[147];
    wordBits(w, b);
    for (int i = 0; i < 49; i += 5) b[49 * (i % 3) + i] ^= 1;
    Word x = decodeBits(b);
    CHECK(x.ok && x.w == w && x.type == To && x.text() == "LY2" && x.voteErrs == 10 && x.golayErrs == 0, "vote: ok %d %06X errs %d", x.ok, x.w, x.voteErrs);
    // two copies wrong in one bit: Golay puts it right
    wordBits(w, b);
    b[7] ^= 1; b[49 + 7] ^= 1;
    x = decodeBits(b);
    CHECK(x.ok && x.w == w && x.golayErrs == 1, "golay after vote: %d", x.golayErrs);
    // long addresses: TO, DATA, REP .. padded with @
    auto v = addressWords(To, "DL5ABCDEF1");
    CHECK(v.size() == 4 && (v[1] >> 21) == Data && (v[2] >> 21) == Rep && (v[3] >> 21) == Data, "address words %zu", v.size());
    CHECK(trimAddress("ABC@@") == "ABC" && trimAddress("@?@") == "@?@", "trim");
    // a whole call: scanning TO (long address), LQA, AMD, TIS
    Assembler a;
    for (int i = 0; i < 3; i++) feedCall(a, addressWords(To, "OH2XYZ1"));
    feedCall(a, {lqaWord(3, 21, 2)});
    feedCall(a, amdWords("HELLO WORLD"));
    feedCall(a, addressWords(Tis, "LY2ABC"));
    Call c = a.finish();
    CHECK(c.kind == Call::Individual && c.to.size() == 1 && c.to[0] == "OH2XYZ1" && c.from == "LY2ABC" && c.conclusion == Tis, "call %s -> %s", c.from.c_str(), c.to.empty() ? "" : c.to[0].c_str());
    CHECK(c.amd == "HELLO WORLD", "amd \"%s\"", c.amd.c_str());
    CHECK(c.lqa && c.lqaBer == 3 && c.lqaSinad == 21 && c.lqaMp == 2, "lqa %d %d %d", c.lqaBer, c.lqaSinad, c.lqaMp);
    // a sounding, a group, the allcall
    for (int i = 0; i < 3; i++) feedCall(a, addressWords(Tws, "K1ABC"));
    c = a.finish();
    CHECK(c.kind == Call::Sounding && c.from == "K1ABC" && c.conclusion == Tws, "sounding");
    feedCall(a, addressWords(Thru, "F5XYZ")); feedCall(a, addressWords(Thru, "I2ABC")); feedCall(a, addressWords(Tis, "EA1ABC"));
    c = a.finish();
    CHECK(c.kind == Call::Group && c.thru.size() == 2, "group");
    feedCall(a, addressWords(To, "@?@")); feedCall(a, addressWords(Tws, "EA1ABC"));
    c = a.finish();
    CHECK(c.kind == Call::AllCall, "allcall");
    printf(fails ? "ale proto: %d FAILED\n" : "ale proto: all passed\n", fails);
    return fails ? 1 : 0;
}
