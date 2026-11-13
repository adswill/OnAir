// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES DCS message format: the BCH (31,21) address code against the JMA annex example, error correction, frame bits and report parsing (ASCII and pseudo-binary B).
#include "dect2/goesdcs/goesdcs_proto.h"
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> bitsOf(const std::string& s) {
    std::vector<uint8_t> v;
    for (char c : s) if (c == '0' || c == '1') v.push_back((uint8_t)(c - '0'));
    return v;
}

int main() {
    // the example address of the JMA DCP annex: 21 data bits, then the 10 check bits 0110000000 (b1 first)
    const auto data = bitsOf("000 00 010 0001 000111101");
    uint32_t d21 = 0;
    for (uint8_t b : data) d21 = (d21 << 1) | b;
    CHECK(data.size() == 21, "21 bits");
    CHECK(dcs::bchCheck(d21) == 0x180, "check bits %03X", dcs::bchCheck(d21));   // 0110000000
    const uint32_t w = dcs::addressWord(d21);
    uint32_t cw; int ne;
    CHECK(dcs::addressDecode(w, cw, ne) && cw == w && ne == 0, "clean word");
    int fixed1 = 0, fixed2 = 0, lost3 = 0, total3 = 0;
    for (int i = 0; i < 31; i++) {
        if (dcs::addressDecode(w ^ (1u << i), cw, ne) && cw == w && ne == 1) fixed1++;
        for (int j = i + 1; j < 31; j++) {
            if (dcs::addressDecode(w ^ (1u << i) ^ (1u << j), cw, ne) && cw == w && ne == 2) fixed2++;
            for (int k = j + 1; k < 31; k += 3) {
                total3++;
                uint32_t c3; int n3;
                if (!dcs::addressDecode(w ^ (1u << i) ^ (1u << j) ^ (1u << k), c3, n3)) lost3++;
            }
        }
    }
    CHECK(fixed1 == 31, "single errors fixed %d", fixed1);
    CHECK(fixed2 == 31 * 30 / 2, "double errors fixed %d", fixed2);
    CHECK(lost3 * 100 > total3 * 45, "triple errors detected %d of %d", lost3, total3);   // a t = 2 code of this length miscorrects about half of the triple errors and rejects the rest
    CHECK(dcs::addressHex(w).size() == 8, "hex");

    // the sync word and the EOT code of the annex
    const uint8_t sync[15] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1};
    for (int i = 0; i < 15; i++) CHECK(dcs::kSync[i] == sync[i], "sync bit %d", i);
    CHECK(dcs::parityOk(dcs::kEot) && dcs::kEot == 0x04, "EOT code");
    for (int c = 0; c < 128; c++) CHECK(dcs::parityOk(dcs::withParity((uint8_t)c)), "parity %d", c);

    // channels
    CHECK(dcs::channelOf(dcs::channelHz(1)) == 1 && dcs::channelOf(dcs::channelHz(266)) == 266 && dcs::channelOf(100e6) == 0, "channel numbers");
    CHECK(dcs::channelOf(dcs::channelHz(100) + 500) == 100 && dcs::channelOf(dcs::channelHz(100) + 800) == 101, "channel rounding");

    // an ASCII report, both formats
    const std::string txt = "WL 12.34\r\nTW 18.5 OK\r\n";
    const std::vector<uint8_t> d7(txt.begin(), txt.end());
    for (int ext = 0; ext < 2; ext++) {
        const auto fb = dcs::frameBits(d21, ext != 0, 0x5A, d7);
        std::vector<uint8_t> after(fb.begin() + 15 + 31, fb.end());
        DcsMessage m;
        CHECK(dcs::parseBody(after, ext != 0, m), "parse ext %d", ext);
        CHECK(m.eot && m.parityErrors == 0 && m.raw.size() == d7.size(), "ext %d: eot %d perr %d n %zu", ext, (int)m.eot, m.parityErrors, m.raw.size());
        CHECK(m.format == 0 && m.text.find("WL 12.34") != std::string::npos, "text '%s'", m.text.c_str());
        CHECK(m.flags == (ext ? 0x5A : -1), "flags %d", m.flags);
        bool wl = false, tw = false, ok = false;
        for (const auto& f : m.fields) { if (f == "WL 12.34") wl = true; if (f == "TW 18.5") tw = true; if (f == "OK") ok = true; }
        CHECK(wl && tw && ok, "fields (%zu)", m.fields.size());
        // the wrong preference must not change a clear case
        DcsMessage m2;
        CHECK(dcs::parseBody(after, ext == 0, m2) && m2.raw == m.raw, "offset chosen by parity, ext %d", ext);
    }
    // pseudo-binary B
    const auto pb = dcs::pseudoBinaryB({0, 5, 17, 1, 0, 44, 12, 9, 33, 0, 0, 63, 40, 40, 7});
    const auto fb = dcs::frameBits(d21, true, 0, pb);
    std::vector<uint8_t> after(fb.begin() + 15 + 31, fb.end());
    DcsMessage m;
    CHECK(dcs::parseBody(after, true, m) && m.format == 1, "pseudo-binary recognised (%d)", m.format);
    CHECK(m.fields.size() == 5 && m.fields[0] == "1: 337" && m.fields[1].rfind("2: ", 0) == 0, "groups of three: %zu '%s'", m.fields.size(), m.fields.empty() ? "" : m.fields[0].c_str());
    // a flipped bit in a character shows as a parity error
    after[40] ^= 1;
    DcsMessage mb;
    dcs::parseBody(after, true, mb);
    CHECK(mb.parityErrors == 1, "parity error counted (%d)", mb.parityErrors);
    return testSummary("goesdcs proto");
}
