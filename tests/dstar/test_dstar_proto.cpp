// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR protocol pieces: the CRC, the header FEC chain (with bit errors), the interleaver, the AMBE bit order, the GPS parsers.
#include "dect2/dstar/dstar_proto.h"
#include "dect2/dstar/dstar_gen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // CRC-CCITT as D-STAR uses it is CRC-16/X-25: check value 0x906E
    CHECK(dstar::crc((const uint8_t*)"123456789", 9) == 0x906E, "crc %04X", dstar::crc((const uint8_t*)"123456789", 9));

    // the interleaver is a permutation
    {
        uint8_t in[dstar::kHeaderBits], out[dstar::kHeaderBits];
        std::set<int> seen;
        for (int i = 0; i < dstar::kHeaderBits; i++) {
            memset(in, 0, sizeof in); in[i] = 1;
            dstar::interleave(in, out);
            for (int k = 0; k < dstar::kHeaderBits; k++) if (out[k]) seen.insert(k);
        }
        CHECK(seen.size() == (size_t)dstar::kHeaderBits, "interleaver covers %zu", seen.size());
    }

    // the header through the whole chain, clean and with spread bit errors
    dstar::Header h;
    h.flag[0] = 0x40; h.rpt2 = "LY0VNO G"; h.rpt1 = "LY0VNO B"; h.ur = "CQCQCQ"; h.my = "N0CALL"; h.sfx = "ID51";
    uint8_t hb[dstar::kHeaderBytes], bits[dstar::kHeaderBits];
    dstar::headerPack(h, hb);
    dstar::headerEncode(hb, bits);
    for (int nerr : {0, 8, 20, 30}) {
        float soft[dstar::kHeaderBits];
        for (int i = 0; i < dstar::kHeaderBits; i++) soft[i] = bits[i] ? 1.f : -1.f;
        for (int k = 0; k < nerr; k++) { const int i = (k * dstar::kHeaderBits / std::max(1, nerr) + 5) % dstar::kHeaderBits; soft[i] = -soft[i]; }
        uint8_t out[dstar::kHeaderBytes];
        const int corr = dstar::headerDecode(soft, out);
        dstar::Header g;
        const bool ok = dstar::headerUnpack(out, g);
        CHECK(ok && corr == nerr, "%d errors: ok %d, corrected %d", nerr, ok, corr);
        CHECK(g.my == "N0CALL" && g.sfx == "ID51" && g.ur == "CQCQCQ" && g.rpt1 == "LY0VNO B" && g.rpt2 == "LY0VNO G" && g.flag[0] == 0x40, "%d errors: fields", nerr);
    }
    // a damaged header fails its CRC
    {
        uint8_t d[dstar::kHeaderBytes];
        memcpy(d, hb, sizeof d); d[30] ^= 4;
        dstar::Header g;
        CHECK(!dstar::headerUnpack(d, g), "CRC catches a wrong byte");
    }
    CHECK(dstar::flagText(h.flag) == "via repeater", "flags '%s'", dstar::flagText(h.flag).c_str());

    // AMBE order: every one of the 72 bits lands in its own place
    {
        std::set<int> seen;
        for (int i = 0; i < 72; i++) {
            uint8_t v[72] = {}; v[i] = 1;
            char fr[4][24];
            dstar::ambeFrame(v, fr);
            int cnt = 0;
            for (int w = 0; w < 4; w++) for (int x = 0; x < 24; x++) if (fr[w][x]) { cnt++; seen.insert(w * 24 + x); }
            CHECK(cnt == 1, "bit %d in %d places", i, cnt);
        }
        CHECK(seen.size() == 72, "AMBE places %zu", seen.size());
    }

    // GPS: what the generator writes, the parsers read back
    {
        dstar::Fix f;
        std::string l = dstarRmc(54.68720, 25.27970, 36, 90);
        l.resize(l.find('\r'));
        CHECK(dstar::parseNmea(l, f) && std::fabs(f.lat - 54.6872) < 1e-4 && std::fabs(f.lon - 25.2797) < 1e-4 && f.velValid && std::fabs(f.speedKmh - 36) < 0.2 && f.course == 90, "RMC %s", l.c_str());
        std::string bad = l; bad[20] = bad[20] == '1' ? '2' : '1';
        CHECK(!dstar::parseNmea(bad, f), "RMC checksum");
        l = dstarGga(-33.9, -70.65, 520);
        l.resize(l.find('\r'));
        CHECK(dstar::parseNmea(l, f) && std::fabs(f.lat + 33.9) < 1e-4 && std::fabs(f.lon + 70.65) < 1e-4 && f.altValid && std::fabs(f.altM - 520) < 0.5, "GGA %s", l.c_str());
        l = dstarDprs("LY1TST-7", 54.89850, 23.90360, "hello");
        l.resize(l.find('\r'));
        CHECK(dstar::parseDprs(l, f) && f.call == "LY1TST-7" && std::fabs(f.lat - 54.8985) < 2e-4 && std::fabs(f.lon - 23.9036) < 2e-4 && f.comment == "hello", "D-PRS %s -> %s", l.c_str(), f.comment.c_str());
        std::string bd = l; bd[bd.size() - 2] = 'X';
        CHECK(!dstar::parseDprs(bd, f), "D-PRS CRC");
        // a line from a real radio's format: no course / speed, altitude
        const std::string body = "DL1ABC-9>API705,DSTAR*:!4811.50N/01622.35E>/A=000656 mobile";
        char crc[16]; snprintf(crc, sizeof crc, "$$CRC%04X,", dstar::dprsCrc(body));
        CHECK(dstar::parseDprs(crc + body, f) && std::fabs(f.lat - 48.19167) < 1e-4 && std::fabs(f.lon - 16.3725) < 1e-4 && f.altValid && std::fabs(f.altM - 200) < 1 && f.comment == " mobile", "D-PRS altitude '%s'", f.comment.c_str());
    }

    printf(fails ? "dstar proto: %d FAILED\n" : "dstar proto: all passed\n", fails);
    return fails ? 1 : 0;
}
