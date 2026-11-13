// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT message layer: every test beacon survives encode -> decode with all its fields, the BCH codes repair up to 3 / 2 damaged bits and refuse
// more, the fixed fields land where the format puts them, Baudot packing, positions in all four quadrants and the position default.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::sarsat;
#include "../common/test_util.h"

static bool dividesXnPlus1(uint32_t g, int deg, int n) {   // g(x) | x^n + 1 : the generator of a (shortened) cyclic code
    std::vector<uint8_t> w((size_t)n + 1, 0);
    w[0] = 1; w[(size_t)n] = 1;
    return bchRemainder(w, 1, n + 1, g, deg) == 0;
}

int main() {
    CHECK(dividesXnPlus1(0x26D9E3, 21, 127), "BCH-1 generator");
    CHECK(dividesXnPlus1(0x1539, 12, 63), "BCH-2 generator");
    // every test beacon
    for (const Spec& s : testBeacons()) {
        std::vector<uint8_t> b = encode(s);
        CHECK(b.size() == (size_t)(s.longMsg ? 144 : 112), "length %zu", b.size());
        Beacon o;
        CHECK(decode(b, o), "decode failed");
        CHECK(o.valid && o.corrected == 0 && o.longMsg == s.longMsg && o.userProto == s.userProto && o.mid == s.mid && o.protoCode == s.protoCode, "fields");
        CHECK(o.hexId.size() == 15, "hex id %s", o.hexId.c_str());
        CHECK(!o.country.empty(), "country of %d", o.mid);
        CHECK(o.homing == s.homing, "homing");
        if (s.longMsg) CHECK(o.bch2Present && o.bch2Ok, "second check");
        if (s.hasPos && !s.userProto) {
            CHECK(o.hasPos, "position missing");
            const double tol = s.longMsg && s.fine ? 0.0012 : 0.126;
            CHECK(std::fabs(o.lat - s.lat) <= tol && std::fabs(o.lon - s.lon) <= tol, "position %.4f %.4f vs %.4f %.4f", o.lat, o.lon, s.lat, s.lon);
            CHECK(o.fine == (s.longMsg && s.fine), "fine flag");
        } else CHECK(!o.hasPos, "a position that is not there");
        printf("  %s %s %s | %s | %s\n", o.hexId.c_str(), o.beaconType.c_str(), o.country.c_str(), o.idText.c_str(), o.hasPos ? "pos" : "-");
        // up to 3 damaged bits in bits 25-106 are repaired
        for (int nerr = 1; nerr <= 3; nerr++) {
            std::vector<uint8_t> d = encode(s);
            const int pos[3] = {30, 67, 100};
            for (int k = 0; k < nerr; k++) d[(size_t)pos[k]] ^= 1;
            Beacon r;
            CHECK(decode(d, r) && r.valid && r.corrected == nerr && r.hexId == o.hexId, "repair of %d bits", nerr);
        }
        if (s.longMsg) {   // two in the second field
            std::vector<uint8_t> d = encode(s);
            d[112] ^= 1; d[130] ^= 1;
            Beacon r;
            CHECK(decode(d, r) && r.bch2Ok && r.corrected == 2 && std::fabs(r.lat - o.lat) < 1e-9, "repair in the second field");
            d = encode(s);
            d[110] ^= 1; d[115] ^= 1; d[140] ^= 1;
            CHECK(decode(d, r) && r.valid && !r.bch2Ok, "three errors in the second field must fail its check");
        }
    }
    // four damaged bits are beyond the code: refused (or, rarely, a different word; never the original with a clean report)
    {
        int refused = 0, total = 0;
        for (const Spec& s : testBeacons()) {
            for (int off = 0; off < 6; off++) {
                std::vector<uint8_t> d = encode(s);
                for (int k = 0; k < 4; k++) d[(size_t)(26 + off * 3 + k * 17)] ^= 1;
                Beacon r;
                total++;
                refused += !decode(d, r);
            }
        }
        CHECK(refused * 10 >= total * 9, "4 damaged bits refused in %d of %d", refused, total);
    }
    // quadrants and the default
    const double pts[4][2] = {{55.7041, 21.0}, {-33.9249, 18.4241}, {40.7128, -74.0060}, {-12.0464, -77.0428}};
    for (auto& p : pts) {
        Spec s; s.protoCode = 3; s.lat = p[0]; s.lon = p[1]; s.ident = 12345;
        std::vector<uint8_t> b = encode(s);
        Beacon o;
        CHECK(decode(b, o) && o.hasPos && std::fabs(o.lat - p[0]) < 0.0012 && std::fabs(o.lon - p[1]) < 0.0012, "quadrant %.2f %.2f -> %.4f %.4f", p[0], p[1], o.lat, o.lon);
    }
    {
        Spec s; s.protoCode = 3; s.hasPos = false;
        std::vector<uint8_t> b = encode(s);
        Beacon o;
        CHECK(decode(b, o) && !o.hasPos, "default position is no position");
    }
    // sync bits and Baudot
    {
        const std::vector<uint8_t> sb = syncBits(false);
        CHECK(sb.size() == 24 && sb[0] == 1 && sb[14] == 1 && sb[15] == 0 && sb[18] == 1 && sb[19] == 0 && sb[23] == 1, "sync bits");
        const std::vector<uint8_t> st = syncBits(true);
        CHECK(st[16] == 1 && st[17] == 1 && st[18] == 0 && st[19] == 1 && st[23] == 0, "self-test sync");
        CHECK(baudotUnpack(baudotPack("OXTEST", 6), 6) == "OXTEST" && baudotUnpack(baudotPack("N123-9", 6), 6) == "N123-9", "Baudot");
        Spec s; s.longMsg = false; s.selfTest = true; s.protoCode = 14; s.ident = 1;
        std::vector<uint8_t> b = encode(s);
        Beacon o;
        CHECK(decode(b, o) && o.selfTest && o.beaconType == "Test", "self-test");
    }
    CHECK(countryOfMid(277) == "Lithuania" && countryOfMid(366) == "USA" && countryOfMid(1).empty(), "countries");
    CHECK(countryOfMid(310) == "Bermuda" && countryOfMid(309) == "Bahamas" && countryOfMid(311) == "Bahamas", "MID 310 is Bermuda");
    {   // protocol codes of C/S T.001 tables A2-A (user) and A2-B (standard location)
        std::string t;
        CHECK(protocolName(false, 3, &t).find("aircraft address") != std::string::npos && t == "ELT", "location 3: ELT 24-bit address");
        CHECK(protocolName(false, 4, &t).find("serial") != std::string::npos && t == "ELT", "location 4: ELT serial");
        CHECK(protocolName(false, 6, &t).find("serial") != std::string::npos && t == "EPIRB", "location 6: EPIRB serial");
        CHECK(protocolName(false, 7, &t).find("serial") != std::string::npos && t == "PLB", "location 7: PLB serial");
        CHECK(protocolName(true, 6, &t).find("call sign") != std::string::npos && t == "EPIRB", "user 6: radio call sign");
        CHECK(protocolName(true, 7, &t) == "Test user" && t == "Test", "user 7: test");
    }
    {   // a 24-bit address ELT: the address is read from bits 41-64
        Spec s; s.protoCode = 3; s.mid = 211; s.ident = 0x3C6444; s.lat = 54.0; s.lon = 12.0;
        std::vector<uint8_t> b = encode(s);
        Beacon o;
        CHECK(decode(b, o) && o.beaconType == "ELT" && o.idText == "aircraft address 3C6444", "24-bit address: %s %s", o.beaconType.c_str(), o.idText.c_str());
    }
    {   // serial user protocol, beacon type in bits 40-42
        const char* want[8] = {"ELT", "ELT", "EPIRB", "ELT", "EPIRB", "?", "PLB", "?"};
        for (int ty = 0; ty < 8; ty++) {
            Spec s; s.longMsg = false; s.userProto = true; s.protoCode = 3; s.mid = 366; s.hasPos = false; s.userBits = (uint64_t)ty << 43;
            std::vector<uint8_t> b = encode(s);
            Beacon o;
            CHECK(decode(b, o) && o.beaconType == want[ty], "serial user type %d: %s, want %s", ty, o.beaconType.c_str(), want[ty]);
        }
    }
    {   // the 15 hex ID does not change with the position (C/S T.001 3.3)
        Spec s; s.protoCode = 7; s.mid = 366; s.ident = (0x1A5u << 14) | 4021u; s.lat = 37.7749; s.lon = -122.4194;
        std::vector<uint8_t> b1 = encode(s);
        s.lat = -12.5; s.lon = 40.25;
        std::vector<uint8_t> b2 = encode(s);
        s.hasPos = false;
        std::vector<uint8_t> b3 = encode(s);
        Beacon o1, o2, o3;
        CHECK(decode(b1, o1) && decode(b2, o2) && decode(b3, o3) && o1.hexId == o2.hexId && o1.hexId == o3.hexId, "hex id %s %s %s", o1.hexId.c_str(), o2.hexId.c_str(), o3.hexId.c_str());
    }
    // ELT(DT) location protocol 9 (issue #407), built bit by bit from C/S T.001 Issue 4 Rev. 7 A3.3.8, not with encode()
    auto put2 = [](std::vector<uint8_t>& b, int f, int l, uint64_t v) { for (int i = l; i >= f; i--) { b[(size_t)i - 1] = v & 1; v >>= 1; } };
    auto seal = [&](std::vector<uint8_t>& b) {
        std::vector<uint8_t> w(b.begin(), b.begin() + 85); w.resize(106, 0);
        put2(b, 86, 106, bchRemainder(w, 25, 106, 0x26D9E3, 21));
        std::vector<uint8_t> w2(b.begin(), b.begin() + 132); w2.resize(144, 0);
        put2(b, 133, 144, bchRemainder(w2, 107, 144, 0x1539, 12));
    };
    auto eltdt = [&](uint64_t hex15, int act, int alt, int fresh, uint64_t pdf2tail) {   // bits 26-85 from a 15 hex ID, then PDF-2
        std::vector<uint8_t> b(144, 0);
        for (int i = 1; i <= 15; i++) b[(size_t)i - 1] = 1;
        put2(b, 16, 24, 0x02F);   // normal frame sync 000101111
        b[24] = 1;
        put2(b, 26, 85, hex15);
        put2(b, 107, 108, (uint64_t)act); put2(b, 109, 112, (uint64_t)alt); put2(b, 113, 114, (uint64_t)fresh); put2(b, 115, 132, pdf2tail);
        seal(b);
        return b;
    };
    auto hexWithPos = [](uint64_t hex15, int ns, int latHalf, int ew, int lonHalf) {   // replace bits 67-85 of a 15 hex ID (bit 26 = MSB of 60)
        hex15 &= ~((1ull << 19) - 1);
        return hex15 | ((uint64_t)ns << 18) | ((uint64_t)latHalf << 10) | ((uint64_t)ew << 9) | (uint64_t)lonHalf;
    };
    {   // the French ELT(DT) test traffic of #407 as an independent decoder (dec406) printed it: hex ID 1C538006253FDFF, PDF-1 43.5 N 1.5 E,
        // manual activation, altitude > 10000 m, location current, offsets +3 min 36 s / -1 min 8 s -> 43.56000 N 1.48111 E
        const uint64_t hx = 0x1C538006253FDFFull;
        const uint64_t tail = (1ull << 17) | (3ull << 13) | (9ull << 9) | (0ull << 8) | (1ull << 4) | 2ull;   // 1 0011 1001 | 0 0001 0010
        std::vector<uint8_t> b = eltdt(hexWithPos(hx, 0, 87, 0, 3), 0, 14, 3, tail);
        Beacon o;
        CHECK(decode(b, o) && o.valid && o.bch2Ok && o.corrected == 0, "ELT-DT decode");
        CHECK(o.protoCode == 9 && o.beaconType == "ELT-DT" && o.protoName == "ELT-DT location" && o.mid == 226, "ELT-DT name %s / %s", o.protoName.c_str(), o.beaconType.c_str());
        CHECK(o.hexId == "1C538006253FDFF", "ELT-DT hex id %s (dec406: 1C538006253FDFF)", o.hexId.c_str());
        CHECK(o.hasPos && o.fine && std::fabs(o.lat - 43.56) < 1e-4 && std::fabs(o.lon - 1.481111) < 1e-4, "ELT-DT position %.5f %.5f", o.lat, o.lon);
        CHECK(!o.homing, "ELT-DT has no homing bit (bits 109-112 are the altitude)");
        CHECK(o.idText.find("manual activation") != std::string::npos && o.idText.find(">10000 m") != std::string::npos && o.idText.find("current") != std::string::npos
              && o.idText.find("type 11") != std::string::npos, "ELT-DT fields: %s", o.idText.c_str());
        // the same transmitter elsewhere, without offsets (seconds 1111 = default): same hex ID, PDF-1 position only
        std::vector<uint8_t> b2 = eltdt(hexWithPos(hx, 1, 20, 1, 40), 1, 15, 1, 0x10F | (0x10Full << 9));
        Beacon o2;
        CHECK(decode(b2, o2) && o2.hasPos && !o2.fine && std::fabs(o2.lat + 10.0) < 1e-9 && std::fabs(o2.lon + 20.0) < 1e-9, "ELT-DT default offsets %.4f %.4f fine %d", o2.lat, o2.lon, (int)o2.fine);
        CHECK(o2.hexId == o.hexId, "ELT-DT hex id must not follow the position: %s %s", o.hexId.c_str(), o2.hexId.c_str());
        CHECK(o2.idText.find("by the beacon") != std::string::npos && o2.idText.find("not available") != std::string::npos, "ELT-DT fields 2: %s", o2.idText.c_str());
    }
    {   // type 00 (24-bit address), the PDF-2 rotating field with the 3LD (A3.3.8.3 g example "ZLR" = 000 10001 01001 01010), and no position
        const uint64_t hx = (0ull << 59) | (226ull << 49) | (9ull << 45) | (0ull << 43) | (0x3C6444ull << 19) | (0ull << 18) | (255ull << 10) | (0ull << 9) | 511ull;
        std::vector<uint8_t> b = eltdt(hx, 2, 3, 0, (0x11ull << 10) | (0x09ull << 5) | 0x0Aull);
        Beacon o;
        CHECK(decode(b, o) && o.bch2Ok && !o.hasPos, "ELT-DT rotating field decode");
        CHECK(o.idText.find("aircraft address 3C6444") != std::string::npos && o.idText.find("operator ZLR") != std::string::npos && o.idText.find("external means") != std::string::npos, "ELT-DT 3LD: %s", o.idText.c_str());
    }
    {   // type 01: designator + serial, the location test (bits 43-66 all 0), and the cancellation message (A3.3.8.5)
        const uint64_t base = (226ull << 49) | (9ull << 45) | (255ull << 10) | 511ull;
        const uint64_t afr = (0x18ull << 10) | (0x16ull << 5) | 0x0Aull;   // A=111000 F=110110 R=101010 without the leading 1
        std::vector<uint8_t> b = eltdt(base | (1ull << 43) | (((afr << 9) | 77ull) << 19), 0, 0, 3, 0x10F | (0x10Full << 9));
        Beacon o;
        CHECK(decode(b, o) && o.idText.find("operator AFR, serial 77") != std::string::npos, "ELT-DT type 01: %s", o.idText.c_str());
        std::vector<uint8_t> t = eltdt(base, 0, 0, 3, 0x10F | (0x10Full << 9));
        CHECK(decode(t, o) && o.beaconType == "Test" && o.idText.find("location test") != std::string::npos, "ELT-DT location test: %s / %s", o.beaconType.c_str(), o.idText.c_str());
        std::vector<uint8_t> c(144, 0);
        for (int i = 1; i <= 15; i++) c[(size_t)i - 1] = 1;
        put2(c, 16, 24, 0x02F); c[24] = 1;
        put2(c, 27, 36, 226); put2(c, 37, 40, 9); put2(c, 43, 66, 0x3C6444);
        put2(c, 67, 75, 0x1FA); put2(c, 76, 85, 0x3FA); put2(c, 107, 114, 0x3C); put2(c, 115, 123, 0x0F0); put2(c, 124, 132, 0x0F0);
        seal(c);
        CHECK(decode(c, o) && !o.hasPos && o.idText.find("cancellation") != std::string::npos, "ELT-DT cancellation: %s", o.idText.c_str());
    }
    {   // a standard location protocol (ELT 24-bit address, PC 0011) with PDF-2 built from A3.3.5.4: 5-bit minutes, 4-bit seconds, homing in bit 112
        std::vector<uint8_t> b(144, 0);
        for (int i = 1; i <= 15; i++) b[(size_t)i - 1] = 1;
        put2(b, 16, 24, 0x02F); b[24] = 1;
        put2(b, 27, 36, 211); put2(b, 37, 40, 3); put2(b, 41, 64, 0x3C6444);
        b[64] = 0; put2(b, 66, 74, 216);   // 54.00 N
        b[74] = 0; put2(b, 76, 85, 48);    // 12.00 E
        put2(b, 107, 110, 0xD); b[110] = 1; b[111] = 1;
        b[112] = 1; put2(b, 114, 118, 20); put2(b, 119, 122, 9);   // +20 min 36 s
        b[122] = 0; put2(b, 124, 128, 3); put2(b, 129, 132, 2);    // -3 min 8 s
        seal(b);
        Beacon o;
        CHECK(decode(b, o) && o.bch2Ok && o.fine && o.homing, "standard location decode fine %d homing %d", (int)o.fine, (int)o.homing);
        CHECK(std::fabs(o.lat - (54.0 + 20 / 60.0 + 36 / 3600.0)) < 1e-6 && std::fabs(o.lon - (12.0 - 3 / 60.0 - 8 / 3600.0)) < 1e-6, "standard location offsets %.6f %.6f", o.lat, o.lon);
        put2(b, 119, 122, 15); seal(b);   // seconds 1111: the default, no offset
        CHECK(decode(b, o) && !o.fine && std::fabs(o.lat - 54.0) < 1e-9, "standard location default offset");
    }
    {   // user protocol homing: bits 84-85 = 01 (A3.3.2.2), not bit 111
        Spec s; s.longMsg = false; s.userProto = true; s.protoCode = 2; s.mid = 219; s.hasPos = false;
        s.userBits = (baudotPack("OXTEST", 6) << 10) | (baudotPack("1", 1) << 4) | 1u;   // bits 84-85 are the two lowest of bits 40-85
        std::vector<uint8_t> b = encode(s);
        Beacon o;
        CHECK(decode(b, o) && o.homing, "user protocol homing from bits 84-85");
    }
    return testSummary("test_sarsat_proto");
}
