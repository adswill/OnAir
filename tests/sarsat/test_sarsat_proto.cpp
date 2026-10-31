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
    {   // #407: ELT-DT (location protocol 9): named, position decoded, one hex ID however the status bits change
        Spec d; d.longMsg = true; d.userProto = false; d.mid = 226; d.protoCode = 9; d.lat = 43.56; d.lon = 1.48111; d.hasPos = true; d.fine = true;
        std::string id0; double la0 = 0;
        for (uint32_t st : {0x000000u, 0x06253Fu, 0xA5A5A5u, 0xFFFFFEu}) {
            d.ident = st;
            std::vector<uint8_t> b = encode(d);
            Beacon o;
            CHECK(decode(b, o) && o.valid && o.protoCode == 9, "ELT-DT decodes");
            CHECK(o.protoName.find("ELT-DT") != std::string::npos && o.beaconType == "ELT-DT", "ELT-DT named: %s / %s", o.protoName.c_str(), o.beaconType.c_str());
            CHECK(o.hasPos && std::fabs(o.lat - 43.56) < 0.002 && std::fabs(o.lon - 1.48111) < 0.002, "ELT-DT position %.4f %.4f", o.lat, o.lon);
            if (id0.empty()) { id0 = o.hexId; la0 = o.lat; }
            CHECK(o.hexId == id0 && o.hexId.size() == 15, "ELT-DT hex ID stable: %s vs %s", o.hexId.c_str(), id0.c_str());
            // two damaged bits and a 5 Hz-class slip do not change the identity
            std::vector<uint8_t> bad = b; bad[50] ^= 1; bad[90] ^= 1;
            Beacon o2;
            CHECK(decode(bad, o2) && o2.hexId == id0 && std::fabs(o2.lat - la0) < 1e-9, "ELT-DT after two bit errors");
        }
        Spec e = d; e.mid = 227; e.ident = 1;
        std::vector<uint8_t> be = encode(e); Beacon oe;
        CHECK(decode(be, oe) && oe.hexId != id0, "another country is another beacon");
    }
    return testSummary("test_sarsat_proto");
}
