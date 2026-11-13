// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: the bit-stream transforms, the text views, the pattern search and the exports.
#include "../common/tmp_path.h"
#include "wb_test.h"
#include <cstring>
using namespace dect2::wb;

static Bits fromStr(const char* s) { Bits b; for (; *s; s++) if (*s == '0' || *s == '1') b.push_back((uint8_t)(*s - '0')); return b; }

int main() {
    {   // invert, differential, NRZI
        BitOpts o; o.invert = true;
        CHECK(applyBits(fromStr("0011"), o) == fromStr("1100"), "invert");
        BitOpts d; d.diffDecode = true;
        CHECK(applyBits(fromStr("0110"), d) == fromStr("0101"), "diff %s", bitsToString(applyBits(fromStr("0110"), d)).c_str());
        BitOpts n; n.nrzi = true;
        CHECK(applyBits(fromStr("1100"), n) == fromStr("0101"), "nrzi %s", bitsToString(applyBits(fromStr("1100"), n)).c_str());
    }
    {   // Manchester: IEEE 802.3 01 = 1, 10 = 0; Thomas the other way; errors counted
        BitOpts m; m.manchester = 1;
        size_t e = 0;
        CHECK(applyBits(fromStr("01100110"), m, &e) == fromStr("1010") && e == 0, "manch ieee");
        m.manchester = 2;
        CHECK(applyBits(fromStr("01100110"), m) == fromStr("0101"), "manch thomas");
        m.manchester = 1;
        CHECK(applyBits(fromStr("0111"), m, &e) == fromStr("1") && e == 1, "manch error %zu", e);
        m.manchesterPhase = 1;
        CHECK(applyBits(fromStr("001011"), m) == fromStr("11"), "manch phase %s", bitsToString(applyBits(fromStr("001011"), m)).c_str());
    }
    {   // bit reversal per byte, skip, packing, views
        BitOpts r; r.bitReverse = true;
        CHECK(applyBits(fromStr("1000000001000000"), r) == fromStr("0000000100000010"), "reverse");
        BitOpts s; s.skip = 3;
        CHECK(applyBits(fromStr("101011"), s) == fromStr("011"), "skip");
        const Bits b = fromStr("0100000101000010");   // 'A' 'B'
        const std::vector<uint8_t> p = packBits(b);
        CHECK(p.size() == 2 && p[0] == 0x41 && p[1] == 0x42, "pack");
        const std::vector<uint8_t> q = packBits(b, true);
        CHECK(q[0] == 0x82, "pack lsb %02x", q[0]);
        const std::string hex = formatBits(b, ViewHex, 16);
        CHECK(hex.find("41 42") != std::string::npos, "hex view: %s", hex.c_str());
        const std::string asc = formatBits(b, ViewAscii, 16);
        CHECK(asc.find("AB") != std::string::npos, "ascii view: %s", asc.c_str());
        const std::string bin = formatBits(b, ViewBinary, 16);
        CHECK(bin.find("01000001 01000010") != std::string::npos, "binary view: %s", bin.c_str());
    }
    {   // patterns
        Bits p;
        CHECK(parsePattern("0x7E", p) && p == fromStr("01111110"), "hex pattern");
        CHECK(parsePattern("1011 0010", p) && p.size() == 8, "binary pattern");
        CHECK(parsePattern("A5F0", p) && p.size() == 16, "bare hex");
        CHECK(!parsePattern("xyz", p), "bad pattern");
        CHECK(parsePattern("0b1010", p) && p == fromStr("1010"), "0b");
        const Bits b = fromStr("0001111110000111111000001010");
        Bits pat; parsePattern("0x7E", pat);
        const std::vector<size_t> h = findPattern(b, pat);
        CHECK(h.size() == 2 && h[0] == 2 && h[1] == 12, "found %zu", h.size());
        std::vector<size_t> inv;
        Bits ib = b; for (auto& v : ib) v ^= 1;
        const std::vector<size_t> h2 = findPattern(ib, pat, true, 0, 100, &inv);
        CHECK(h2.empty() && inv.size() == 2, "inverted hits %zu %zu", h2.size(), inv.size());
        Bits noisy = fromStr("0000011111010000");   // 0x7E with one bit wrong
        CHECK(findPattern(noisy, pat, false, 0).empty() && findPattern(noisy, pat, false, 1).size() == 1, "mismatch tolerance");
    }
    {   // export
        const Bits b = fromStr("010000010100001000001");
        std::string err;
        const std::string pt = tmpPath("wb_bits_test.txt"), pb = tmpPath("wb_bits_test.bin");
        CHECK(writeBitsText(pt, b, err), "text %s", err.c_str());
        CHECK(writeBitsBin(pb, b, false, err), "bin %s", err.c_str());
        FILE* f = fopen(pb.c_str(), "rb");
        uint8_t buf[8] = {};
        const size_t got = f ? fread(buf, 1, 8, f) : 0;
        if (f) fclose(f);
        CHECK(got == 3 && buf[0] == 0x41 && buf[1] == 0x42 && buf[2] == 0x08, "bin file %zu %02x %02x %02x", got, buf[0], buf[1], buf[2]);
        f = fopen(pt.c_str(), "rb");
        char t[64] = {};
        if (f) { size_t g = fread(t, 1, 63, f); (void)g; fclose(f); }
        CHECK(strncmp(t, "010000010100001000001", 21) == 0, "text file %s", t);
    }
    printf(fails ? "FAILED\n" : "wb_bits ok\n");
    return fails ? 1 : 0;
}
