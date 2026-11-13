// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR air interface pieces: syncs, Hamming (12,8) with errors, CRCs, interleaved blocks, header / CCH / end / data round trips,
// colour codes, the PMR446 channel raster.
#include "dect2/dpmr/dpmr_proto.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // the syncs: FS1 starts 0x57 = 01 01 01 11 -> +3 +3 +3 -3; all four differ
    CHECK(dpmr::kFs1[0] == 3 && dpmr::kFs1[3] == -3 && dpmr::kFs2[0] == 3 && dpmr::kFs3[0] == 3 && dpmr::kFs4[0] == -3, "sync symbols");
    int same13 = 0, same14 = 0;
    for (int i = 0; i < 24; i++) { same13 += dpmr::kFs1[i] == dpmr::kFs3[i]; same14 += dpmr::kFs1[i] == dpmr::kFs4[i]; }
    CHECK(same13 < 16 && same14 < 16, "syncs distinct %d %d", same13, same14);
    // Hamming: every single error corrected
    for (int v = 0; v < 256; v++) {
        uint8_t dd[8], c[12];
        for (int i = 0; i < 8; i++) dd[i] = (v >> i) & 1;
        dpmr::hamEncode(dd, c);
        for (int e = 0; e < 12; e++) {
            uint8_t x[12]; memcpy(x, c, 12); x[e] ^= 1;
            CHECK(dpmr::hamDecode(x) == 1 && !memcmp(x, c, 8), "hamming %d bit %d", v, e);
        }
    }
    // blocks with one error per word (spread by the interleaver: a burst of 10 air bits in a 10-word block)
    dpmr::Header h; h.type = dpmr::kHdrComm; h.called = 0xABCDEF; h.own = 1234567; h.commMode = dpmr::kVoice; h.callType = dpmr::kIndividual; h.emergency = true; h.extra = 9;
    uint8_t info[80], air[120], back[80];
    dpmr::headerBits(h, info);
    dpmr::blockEncode(info, 10, air);
    for (int i = 30; i < 40; i++) air[i] ^= 1;
    const int fixed = dpmr::blockDecode(air, 10, back);
    dpmr::Header r;
    CHECK(fixed == 10 && dpmr::headerParse(back, r) && r.called == 0xABCDEF && r.own == 1234567 && r.emergency && r.extra == 9 && r.callType == 0, "header burst: %d", fixed);
    back[5] ^= 1;
    CHECK(!dpmr::headerParse(back, r), "header CRC-8 catches an error");
    dpmr::Cch c; c.frame = 2; c.idPart = 0x5A5; c.commMode = dpmr::kVoice; c.slowData = 0x2A5A5;
    uint8_t ci[48], ca[72], cb[48];
    dpmr::cchBits(c, ci); dpmr::blockEncode(ci, 6, ca); ca[7] ^= 1;
    dpmr::Cch c2;
    CHECK(dpmr::blockDecode(ca, 6, cb) == 1 && dpmr::cchParse(cb, c2) && c2.frame == 2 && c2.idPart == 0x5A5 && c2.slowData == 0x2A5A5, "CCH");
    dpmr::End e; e.own = 777;
    dpmr::endBits(e, ci);
    dpmr::End e2;
    CHECK(dpmr::endParse(ci, e2) && e2.own == 777, "end");
    uint8_t di[192]; int blk = 0; std::string txt;
    dpmr::dataBits(3, "HELLO", di);
    CHECK(dpmr::dataParse(di, blk, txt) && blk == 3 && txt == "HELLO", "data block");
    for (int cc = 0; cc < 64; cc++) {
        int s[12]; float v[12];
        dpmr::ccSymbols(cc, s);
        for (int i = 0; i < 12; i++) v[i] = (float)s[i] * 0.8f;
        v[cc % 12] = 0;   // one symbol lost
        CHECK(dpmr::ccDecode(v) == cc, "colour code %d", cc);
    }
    CHECK(dpmr::pmr446Mhz(1) == 446.103125 && dpmr::pmr446Mhz(16) > 446.196 && dpmr::pmr446Mhz(16) < 446.197, "channels");
    CHECK(dpmr::idText(0xFFFFFF) == "ALL" && dpmr::idText(42) == "00000042", "ids");
    printf(fails ? "dpmr proto: %d FAILED\n" : "dpmr proto: all passed\n", fails);
    return fails ? 1 : 0;
}
