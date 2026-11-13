// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Heard-ID repair (voice/heard_ids.h): the dictionary, D-STAR header repair with the CRC as the judge and DMR voice LC repair with the Reed-Solomon
// check as the judge. Measures how many damaged headers come back and that a wrong ID is never made up.
#include "dect2/voice/heard_ids.h"
#include "dect2/dstar/dstar_repair.h"
#include "dect2/dmr/dmr_repair.h"
#include "dect2/dmr/dmr_proto.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <set>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::string callsign(std::mt19937& g) {
    static const char* pre[] = {"DL", "DK", "LY", "OH", "SM", "G", "F", "PA", "ON", "HB"};
    std::string s = pre[g() % 10];
    s += (char)('0' + g() % 10);
    const int n = 2 + (int)(g() % 2);
    for (int i = 0; i < n; i++) s += (char)('A' + g() % 26);
    s.resize(8, ' ');
    return s;
}

int main() {
    // dictionary
    {
        voice::HeardIds h;
        h.note(0x123456); h.note(0x123456); h.note(0x123457); h.note(0xABCDEF);
        auto c = h.nearIds(0x123454, 2);
        CHECK(c.size() == 2 && c[0].id == 0x123456 && c[0].flips == 1);   // 0x123454 -> ...56 one flip, ...57 two
        CHECK(h.nearIds(0x123456, 2).size() == 1);                       // itself excluded
        CHECK(h.known(0xABCDEF) && !h.known(1));
        h.noteText("DL1ABC  "); h.noteText("DL1ABD  ");
        CHECK(h.nearText("DL1ABX  ", 1).size() == 2);
        CHECK(h.nearText("XX1ABX  ", 1).empty());
        CHECK(h.accidentalChance(24, 2) < 1e-3);
    }
    // D-STAR header repair
    {
        std::mt19937 g(5);
        printf("D-STAR header: damaged characters -> repaired / not repaired / WRONG (of 1000, 40 callsigns heard)\n");
        for (int k = 1; k <= 3; k++) {
            int ok = 0, none = 0, wrong = 0;
            for (int t = 0; t < 1000; t++) {
                voice::HeardIds heard;
                std::vector<std::string> pool;
                for (int i = 0; i < 40; i++) pool.push_back(callsign(g));
                for (auto& p : pool) heard.noteText(p);
                heard.noteText("CQCQCQ  "); heard.noteText("DIRECT  ");
                dstar::Header h;
                h.rpt2 = "DB0XYZ B"; h.rpt1 = "DB0XYZ G"; h.ur = "CQCQCQ"; h.my = pool[g() % 40]; h.sfx = "D75A";
                for (auto* f : {&h.rpt2, &h.rpt1, &h.my}) *f = (f->size() < 8 ? *f + std::string(8 - f->size(), ' ') : *f);
                heard.noteText(h.rpt2); heard.noteText(h.rpt1);
                uint8_t raw[dstar::kHeaderBytes];
                dstar::headerPack(h, raw);
                // damage k characters of the callsign fields (MY, RPT1, RPT2, UR)
                static const int pos[4] = {27, 11, 3, 19};
                for (int i = 0; i < k; i++) raw[pos[g() % 4] + (int)(g() % 8)] ^= (uint8_t)(1 + g() % 63);
                dstar::Header u;
                if (dstar::headerUnpack(raw, u)) { ok++; continue; }   // damage that cancelled out
                dstar::Header r;
                if (!dstar::headerRepair(raw, heard, r)) { none++; continue; }
                uint8_t chk[dstar::kHeaderBytes];
                dstar::headerPack(h, chk);
                uint8_t rep[dstar::kHeaderBytes];
                dstar::headerPack(r, rep);
                if (!memcmp(chk, rep, dstar::kHeaderBytes)) ok++; else wrong++;
            }
            printf("  %d char(s): %4d %4d %3d\n", k, ok, none, wrong);
            CHECK(wrong * 100 <= 1000);          // under 1 percent wrong
            if (k <= 2) CHECK(ok >= 700);        // one or two damaged characters mostly come back
        }
    }
    // DMR voice LC repair
    {
        std::mt19937 g(6);
        printf("DMR voice LC: damaged ID bits (Reed-Solomon cannot fix) -> repaired / not repaired / WRONG (of 1000)\n");
        for (int flips = 3; flips <= 6; flips += 3) {
            int ok = 0, none = 0, wrong = 0, skipped = 0;
            for (int t = 0; t < 1000; t++) {
                voice::HeardIds heard;
                std::vector<uint32_t> pool;
                for (int i = 0; i < 40; i++) { pool.push_back(2000000 + g() % 900000); heard.note(pool.back()); }
                const uint32_t dst = pool[g() % 40], src = pool[g() % 40];
                uint8_t lc[9] = {0, 0, 0};
                dmr::putBe24(lc + 3, dst); dmr::putBe24(lc + 6, src);
                dmr::Bits info;
                dmr::lcBurstInfo(lc, false, info);
                dmr::Bits bad = info;
                // flip bits inside the ID bytes (bits 24..71), at most 2 per ID so the nearest-ID search can reach back
                int d = 0, s = 0;
                for (int i = 0; i < flips; i++) {
                    const bool inDst = (g() & 1) ? d < 2 : s >= 2;
                    (inDst ? d : s)++;
                    bad[(size_t)((inDst ? 24 : 48) + (int)(g() % 24))] ^= 1;
                }
                uint8_t out[9];
                if (dmr::lcBurstDecode(bad, false, out) >= 0) { skipped++; continue; }   // Reed-Solomon fixed it alone
                dmr::Bits fx;
                if (!lcRepairHeard(bad, false, heard, fx)) { none++; continue; }
                CHECK(dmr::lcBurstDecode(fx, false, out) == 0);
                if (dmr::be24(out + 3) == dst && dmr::be24(out + 6) == src) ok++; else wrong++;
            }
            printf("  %d flips: %4d %4d %3d   (%d fixed by the code alone)\n", flips, ok, none, wrong, skipped);
            CHECK(wrong * 200 <= 1000);
            if (flips == 3) CHECK(ok >= 900);   // with six flips one ID often has three: beyond the two-flip search, left alone (never guessed)
        }
    }
    printf("%s\n", fails ? "FAILED" : "voice_heard OK");
    return fails ? 1 : 0;
}
