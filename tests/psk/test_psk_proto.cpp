// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK31 protocol pieces: the varicode table (no "00" inside a code, every code different, round trip of all 128 characters, a bad code),
// the K = 5 convolutional code and its Viterbi decoder (clean, with soft noise, with hard bit errors), the QPSK phase mapping.
#include "dect2/psk/psk_varicode.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::string decodeBits(const std::vector<uint8_t>& b, int* bad = nullptr) {
    psk::VaricodeRx rx;
    std::string s;
    for (uint8_t v : b) { const int c = rx.bit(v); if (c >= 0) s += (char)c; else if (c == -2 && bad) (*bad)++; }
    return s;
}

int main() {
    std::set<std::string> codes;
    for (int c = 0; c < 128; c++) {
        const std::string v = psk::varicode(c);
        CHECK(!v.empty() && v.front() == '1' && v.back() == '1' && v.find("00") == std::string::npos, "code %d %s", c, v.c_str());
        codes.insert(v);
    }
    CHECK(codes.size() == 128, "codes not unique");
    CHECK(std::string(psk::varicode(' ')) == "1" && std::string(psk::varicode('e')) == "11", "space / e");
    std::string all;
    for (int c = 1; c < 128; c++) all += (char)c;
    std::vector<uint8_t> bits(6, 0);
    const auto tb = psk::varicodeBits(all);
    bits.insert(bits.end(), tb.begin(), tb.end());
    CHECK(decodeBits(bits) == all, "round trip of every character");
    // a code that is not in the table: thirteen ones
    std::vector<uint8_t> junk = {0, 0};
    for (int i = 0; i < 13; i++) junk.push_back(1);
    junk.push_back(0); junk.push_back(0);
    const auto ok = psk::varicodeBits("OK");
    junk.insert(junk.end(), ok.begin(), ok.end());
    int bad = 0;
    CHECK(decodeBits(junk, &bad) == "OK" && bad == 1, "bad code skipped (%d)", bad);

    // QPSK mapping is its own inverse and covers all four
    for (int s = 0; s < 4; s++) CHECK(psk::qpskSymbol(psk::qpskTurns(s)) == s, "qpsk map %d", s);

    // convolutional code + Viterbi
    std::mt19937 rng(5);
    for (int trial = 0; trial < 3; trial++) {
        const std::string msg = "CQ CQ DE LY2ABC LY2ABC KO24 PSE K 0123456789";
        std::vector<uint8_t> in(20, 0);
        const auto mb = psk::varicodeBits(msg);
        in.insert(in.end(), mb.begin(), mb.end());
        in.insert(in.end(), 60, 0);
        psk::ConvEncoder enc;
        psk::Viterbi vit;
        dect2::normal_distribution<float> nd(0.f, trial == 0 ? 0.f : 0.55f);
        std::vector<uint8_t> out;
        int flipped = 0;
        for (size_t i = 0; i < in.size(); i++) {
            const int sym = enc.encode(in[i]);
            float s0 = (sym & 1) ? 1.f : -1.f, s1 = (sym & 2) ? 1.f : -1.f;
            s0 += nd(rng); s1 += nd(rng);
            if (trial == 2 && i % 23 == 7) { s0 = -s0; flipped++; }   // hard errors, spread out
            const int b = vit.decode(s0, s1);
            if (b >= 0) out.push_back((uint8_t)b);
        }
        const std::string got = decodeBits(out);
        CHECK(got == msg, "viterbi trial %d: \"%s\"", trial, got.c_str());
        if (trial == 2) CHECK(flipped > 10, "flipped %d", flipped);
    }
    printf(fails ? "psk proto: %d FAILED\n" : "psk proto: all passed\n", fails);
    return fails ? 1 : 0;
}
