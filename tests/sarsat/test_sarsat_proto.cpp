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
    {   // ELT-DT (protocol 9, issue #407): live France burst, PDF-1 43.5 N 1.5 E, offsets +3 min 36 s / -1 min 8 s
        auto build = [&](double lat, double lon, bool offs) {
            Spec s; s.protoCode = 9; s.mid = 226; s.hasPos = false; s.homing = false;
            std::vector<uint8_t> b = encode(s);
            auto put2 = [&](int f, int l, uint64_t v) { for (int i = l; i >= f; i--) { b[(size_t)i - 1] = v & 1; v >>= 1; } };
            put2(41, 42, 3);
            put2(43, 66, 0x006253);
            b[66] = lat < 0; put2(68, 75, (uint64_t)std::lround(std::fabs(lat) * 2));
            b[75] = lon < 0; put2(77, 85, (uint64_t)std::lround(std::fabs(lon) * 2));
            put2(107, 108, 0); put2(109, 112, 14); put2(113, 114, offs ? 3 : 0);
            if (offs) { b[114] = 1; put2(116, 119, 3); put2(120, 123, 9); b[123] = 0; put2(125, 128, 1); put2(129, 132, 2); }
            else put2(115, 132, 0x3FFFF);
            std::vector<uint8_t> w(b.begin(), b.begin() + 85); w.resize(106, 0);
            put2(86, 106, bchRemainder(w, 25, 106, 0x26D9E3, 21));
            std::vector<uint8_t> w2(b.begin(), b.begin() + 132); w2.resize(144, 0);
            put2(133, 144, bchRemainder(w2, 107, 144, 0x1539, 12));
            return b;
        };
        std::vector<uint8_t> b1 = build(43.5, 1.5, true), b2 = build(10.0, 20.0, false);
        Beacon o1, o2;
        CHECK(decode(b1, o1) && o1.valid && o1.bch2Ok && o1.corrected == 0, "ELT-DT decode");
        CHECK(o1.protoCode == 9 && o1.beaconType == "ELT-DT" && o1.protoName == "ELT-DT location", "ELT-DT name %s / %s", o1.protoName.c_str(), o1.beaconType.c_str());
        CHECK(o1.hasPos && std::fabs(o1.lat - 43.56) < 0.001 && std::fabs(o1.lon - 1.48111) < 0.001, "ELT-DT position %.5f %.5f", o1.lat, o1.lon);
        CHECK(decode(b2, o2) && o2.hasPos && std::fabs(o2.lat - 10.0) < 0.01, "ELT-DT without offsets");
        CHECK(o1.hexId == o2.hexId, "ELT-DT hex id must not follow the position: %s %s", o1.hexId.c_str(), o2.hexId.c_str());
    }
    return testSummary("test_sarsat_proto");
}
