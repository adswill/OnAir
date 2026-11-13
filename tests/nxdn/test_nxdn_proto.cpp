// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN protocol: CRCs, LICH, the three coded channels with bit errors, layer 3 messages, AMBE+2 bit order, scrambler.
#include "dect2/nxdn/nxdn_proto.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static uint32_t rng = 7;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main() {
    // CRC: appending the register makes the check of the whole come out as the plain remainder (a single flipped bit is caught)
    {
        uint8_t b[40];
        for (auto& x : b) x = (uint8_t)(rnd() & 1);
        const uint32_t c6 = nxdn::crc6(b, 40), c12 = nxdn::crc12(b, 40), c16 = nxdn::crc16(b, 40);
        b[13] ^= 1;
        CHECK(nxdn::crc6(b, 40) != c6 && nxdn::crc12(b, 40) != c12 && nxdn::crc16(b, 40) != c16, "crc misses a bit error");
        CHECK(c6 < 64 && c12 < 4096 && c16 < 65536, "crc width");
    }
    // LICH
    for (int v = 0; v < 128; v++) {
        nxdn::Lich l{(v >> 5) & 3, (v >> 3) & 3, (v >> 1) & 3, v & 1}, r;
        const uint8_t e = nxdn::lichEncode(l);
        CHECK(nxdn::lichDecode(e, r) && r.rfct == l.rfct && r.fct == l.fct && r.opt == l.opt && r.dir == l.dir, "lich %d", v);
        CHECK(!nxdn::lichDecode((uint8_t)(e ^ 0x80), r), "lich parity %d", v);
    }
    // coded channels: clean, with spread bit errors (corrected), and with too many (CRC fails)
    const nxdn::Ch chs[3] = {nxdn::Ch::Sacch, nxdn::Ch::Facch1, nxdn::Ch::Cac};
    const int fixable[3] = {3, 3, 10};
    for (int ci = 0; ci < 3; ci++) {
        const nxdn::Ch c = chs[ci];
        const int ni = nxdn::infoBits(c), na = nxdn::airBits(c);
        int okClean = 0, okErr = 0, falseOk = 0;
        for (int trial = 0; trial < 50; trial++) {
            std::vector<uint8_t> info((size_t)ni), air((size_t)na), out((size_t)ni);
            for (auto& x : info) x = (uint8_t)(rnd() & 1);
            nxdn::encode(c, info.data(), air.data());
            std::vector<float> soft((size_t)na);
            for (int k = 0; k < na; k++) soft[(size_t)k] = air[(size_t)k] ? 1.f : -1.f;
            int errs = -1;
            if (nxdn::decode(c, soft.data(), out.data(), &errs) && out == info && errs == 0) okClean++;
            std::set<int> pos;
            while ((int)pos.size() < fixable[ci]) pos.insert((int)(rnd() % (uint32_t)na));
            for (int p : pos) soft[(size_t)p] = -soft[(size_t)p];
            if (nxdn::decode(c, soft.data(), out.data(), &errs) && out == info && errs == fixable[ci]) okErr++;
            for (int k = 0; k < na; k++) soft[(size_t)k] = (rnd() & 1) ? 1.f : -1.f;
            if (nxdn::decode(c, soft.data(), out.data())) falseOk++;
        }
        printf("  channel %d: %d/50 clean, %d/50 with %d errors, %d/50 random passed CRC\n", ci, okClean, okErr, fixable[ci], falseOk);
        CHECK(okClean == 50, "clean %d", ci);
        CHECK(okErr >= 45, "errors %d: %d", ci, okErr);
        CHECK(falseOk <= (ci == 0 ? 4 : 0), "false CRC %d: %d", ci, falseOk);
    }
    // layer 3
    {
        nxdn::Msg m; m.type = nxdn::kVcall; m.callType = 4; m.option = 2; m.src = 54321; m.dst = 65000; m.cipher = 1; m.keyId = 9;
        uint8_t b[72]; nxdn::Msg r;
        CHECK(nxdn::buildMsg(m, b, 72) == 56 && nxdn::parseMsg(b, 72, r), "vcall build");
        CHECK(r.type == nxdn::kVcall && r.callType == 4 && r.option == 2 && r.src == 54321 && r.dst == 65000 && r.cipher == 1 && r.keyId == 9, "vcall fields");
        nxdn::Msg d; d.type = nxdn::kSdData; d.block = 3; d.text = "ABCDEFGH";
        uint8_t b2[80];
        nxdn::buildMsg(d, b2, 80);
        CHECK(nxdn::parseMsg(b2, 80, r) && r.block == 3 && r.text == "ABCDEFGH", "short data");
        nxdn::Msg s; s.type = nxdn::kSiteInfo; s.location = 0xABCDEF; s.service = 0x3C00; s.channel = 0x0101;
        uint8_t b3[147];
        nxdn::buildMsg(s, b3, 147);
        CHECK(nxdn::parseMsg(b3, 147, r) && r.location == 0xABCDEF && r.service == 0x3C00 && r.channel == 0x0101, "site info");
    }
    // AMBE+2 order: every one of the 72 bits lands in its own place
    {
        std::set<int> seen;
        for (int i = 0; i < 72; i++) {
            uint8_t b[72] = {}; b[i] = 1;
            char fr[4][24];
            nxdn::ambeFrame(b, fr);
            int n = 0, at = -1;
            for (int w = 0; w < 4; w++) for (int x = 0; x < 24; x++) if (fr[w][x]) { n++; at = w * 24 + x; }
            CHECK(n == 1, "ambe bit %d", i);
            seen.insert(at);
        }
        CHECK(seen.size() == 72, "ambe places %zu", seen.size());
    }
    // PN9: period 511, first bits from the seed 0xE4
    {
        uint8_t p[1022];
        nxdn::pn9(p, 1022);
        CHECK(memcmp(p, p + 511, 511) == 0 && p[0] == 0 && p[1] == 0 && p[2] == 1, "pn9");
    }
    printf(fails ? "nxdn proto: %d FAILED\n" : "nxdn proto: all passed\n", fails);
    return fails ? 1 : 0;
}
