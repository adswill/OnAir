// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SBAS message content: field layouts (DO-229 public description) against messages built here bit by bit, the CRC, and the decoded state of a service
// (PRN mask, fast corrections, integrity, ionospheric grid, GEO navigation, time offset, almanac, long-term corrections).
#include "dect2/sbas/sbas_msg.h"
#include "dect2/gnss/gnss_msg.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2;
using namespace dect2::sbas;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// a whole message from the message bits of its data field (data[0] is message bit 14)
static void build(int index, int type, const uint8_t* data, uint8_t* out) { sbasBuildMessage(index, type, data, out); }

int main() {
    // bit helpers
    {
        uint8_t b[64] = {};
        putBits(b, 3, 12, 0xABC);
        CHECK(getBits(b, 3, 12) == 0xABC, "getBits/putBits");
        putBits(b, 20, 12, (uint32_t)(-37) & 0xFFF);
        CHECK(getSigned(b, 20, 12) == -37, "getSigned %d", getSigned(b, 20, 12));
    }
    // CRC and preamble
    {
        uint8_t d[212] = {}, m[250];
        build(0, 63, d, m);
        CHECK(crcOk(m) && preambleIndex(m) == 0 && messageType(m) == 63, "null message");
        m[100] ^= 1;
        CHECK(!crcOk(m), "a flipped bit should fail the CRC");
        build(1, 2, d, m);
        CHECK(preambleIndex(m) == 1, "second preamble");
        build(2, 2, d, m);
        CHECK(preambleIndex(m) == 2, "third preamble");
    }
    // tables
    CHECK(std::fabs(udreVariance(0) - 0.052) < 1e-9 && std::fabs(udreVariance(13) - 2078.695) < 1e-6 && udreVariance(14) == 0, "UDRE table");
    CHECK(std::fabs(giveVariance(14) - 187.0826) < 1e-9 && giveVariance(15) == 0, "GIVE table");
    CHECK(std::fabs(degradationA(15) - 0.0058) < 1e-12, "ai table");
    {
        int p = 0;
        CHECK(maskBitName(5, &p) == "GPS 5" && p == 5, "mask bit 5");
        CHECK(maskBitName(38, &p) == "GLONASS 1" && p == 1, "mask bit 38");
        CHECK(maskBitName(131, &p) == "SBAS 131" && p == 131, "mask bit 131");
        CHECK(maskBitName(195, &p) == "QZSS 195" && p == 195, "mask bit 195");
        CHECK(maskBitName(100, &p) == "reserved 100" && p == 0, "mask bit 100");
    }

    // a service: the satellites of the test signal
    Network net;
    for (int prn = 131; prn <= 133; prn++) {
        for (int64_t mm = 0; mm < 32; mm++) {
            static const int kTypes[16] = {9, 2, 3, 4, 5, 18, 26, 25, 7, 10, 12, 17, 24, 27, 28, 1};
            const int type = kTypes[mm % 16];
            uint8_t d[212], m[250];
            if (!synthData(prn, mm, type, d)) std::memset(d, 0, sizeof d);
            build((int)(mm % 3), type, d, m);
            CHECK(crcOk(m), "crc of built type %d", type);
            net.add(prn, m, (double)mm);
        }
    }
    CHECK(net.geos().size() == 3, "three satellites, got %zu", net.geos().size());
    CHECK(net.total() == 96, "96 messages, got %llu", (unsigned long long)net.total());
    const GeoState& g = net.geos().at(131);
    CHECK(g.messages == 32 && g.maskValid && g.iodp == 1, "mask: %d messages, valid %d, iodp %d", (int)g.messages, g.maskValid, g.iodp);
    CHECK(g.nMasked == 15, "mask holds %d satellites, want 14 GPS + the SBAS one", g.nMasked);
    CHECK(Network::slotPrn(g, 0) == 1 && Network::slotPrn(g, 13) == 14 && Network::slotPrn(g, 14) == 131, "slot to PRN: %d %d %d", Network::slotPrn(g, 0), Network::slotPrn(g, 13), Network::slotPrn(g, 14));
    CHECK(Network::slotName(g, 14) == "SBAS 131", "slot name '%s'", Network::slotName(g, 14).c_str());
    {
        const Summary s = Network::summarise(g);
        CHECK(s.nFast >= 40, "fast corrections in %d slots", s.nFast);
        CHECK(g.fast[0].valid && g.fast[0].udrei == 0 && g.fast[0].iodp == 1, "slot 1: valid %d udrei %d", g.fast[0].valid, g.fast[0].udrei);
        CHECK(std::fabs(g.fast[0].prcM) <= 8.0, "PRC %.3f m in range", g.fast[0].prcM);
        CHECK(std::fabs(std::fmod(g.fast[0].prcM / 0.125, 1.0)) < 1e-9, "PRC is a multiple of 0.125 m");
    }
    CHECK(g.degValid && g.tLat == 3 && g.iodpDeg == 1, "type 7: %d %d", g.tLat, g.iodpDeg);
    CHECK(g.iodf[0] == 1 || g.iodf[0] == -1, "iodf");
    // GEO navigation
    {
        const GeoNav& n = g.nav;
        const double r = std::sqrt(n.pos[0] * n.pos[0] + n.pos[1] * n.pos[1] + n.pos[2] * n.pos[2]);
        CHECK(n.valid && std::fabs(r - 42164169.0) < 100.0, "GEO radius %.1f m", r);
        CHECK(n.iodn == 17 && n.ura == 2, "iodn %d ura %d", n.iodn, n.ura);
        CHECK(std::fabs(n.pos[2] - 48.0) < 1e-6, "Z %.3f", n.pos[2]);
    }
    // time offset
    {
        const TimeOffset& u = g.utc;
        CHECK(u.valid && u.dtLS == 18 && u.dtLSF == 18 && u.wn == 400 && u.utcId == 1, "type 12: dtLS %d wn %d id %d", u.dtLS, u.wn, u.utcId);
        CHECK(std::fabs(u.a0 - (-1100.0 * std::ldexp(1.0, -30))) < 1e-18, "A0 %.3e", u.a0);
        CHECK(std::fabs(u.a1 - (7.0 * std::ldexp(1.0, -50))) < 1e-24, "A1 %.3e", u.a1);
    }
    // almanac
    CHECK(g.almanac.size() == 3 && g.almanac[0].prn == 120 && g.almanac[2].prn == 122, "almanac %zu entries", g.almanac.size());
    CHECK(g.almanac.size() == 3 && g.almanac[0].pos[0] == 16000 * 2600.0, "almanac X %.0f", g.almanac.empty() ? 0.0 : g.almanac[0].pos[0]);
    // ionospheric grid: band 0 has 60 points (75 mask bits, every fifth off), the delays arrive in blocks of 15
    {
        const IonoSummary s = Network::ionoSummary(g, 0);
        CHECK(s.nIgp == 60 && s.blocksNeeded == 4, "iono band 0: %d IGPs, %d blocks needed", s.nIgp, s.blocksNeeded);
        CHECK(s.nValue >= 15 && s.minM >= 3.0f && s.maxM < 20.0f, "iono: %d values, %.3f .. %.3f m", s.nValue, s.minM, s.maxM);
        CHECK((s.blocksSeen & 1) != 0, "block 0 should be in");
    }
    // long-term corrections
    {
        CHECK(g.longTermMessages > 0 && !g.longTerm.empty(), "long-term: %u messages", g.longTermMessages);
        bool found = false;
        for (auto& l : g.longTerm) if (l.slot == 1 && l.iode == 33 && std::fabs(l.dpos[0] - 1.5) < 1e-9 && std::fabs(l.dpos[1] + 0.875) < 1e-9) found = true;
        CHECK(found, "long-term correction of slot 1 not found");
    }
    // degradation parameters (type 10) are raw
    CHECK(g.deg.valid && g.deg.brrc == 80 && g.deg.cer == 10 && g.deg.cionoRamp == 50, "type 10: %d %d %d", g.deg.brrc, g.deg.cer, g.deg.cionoRamp);

    // type 26 before its mask is held: no values are taken
    {
        Network n2;
        uint8_t d[212], m[250];
        synthData(131, 5, 26, d);
        build(0, 26, d, m);
        n2.add(131, m, 0);
        CHECK(Network::ionoSummary(n2.geos().at(131), 0).nValue == 0, "delays without a mask should not count");
        synthData(131, 0, 18, d);
        build(1, 18, d, m);
        n2.add(131, m, 1);
        synthData(131, 5, 26, d);
        build(2, 26, d, m);
        n2.add(131, m, 2);
        CHECK(Network::ionoSummary(n2.geos().at(131), 0).nValue == 15, "after the mask: %d values", Network::ionoSummary(n2.geos().at(131), 0).nValue);
    }
    // type 26 all ones = not monitored
    {
        Network n3;
        uint8_t d[212], m[250];
        synthData(131, 0, 18, d);
        build(0, 18, d, m);
        n3.add(131, m, 0);
        std::memset(d, 0, sizeof d);
        putBits(d, 0, 4, 0); putBits(d, 4, 4, 0);
        putBits(d, 8, 9, 0x1FF); putBits(d, 17, 4, 15);
        build(1, 26, d, m);
        n3.add(131, m, 1);
        const IonoSummary s = Network::ionoSummary(n3.geos().at(131), 0);
        CHECK(s.nNotMonitored >= 1, "not monitored count %d", s.nNotMonitored);
    }
    // the viewer text
    {
        uint8_t d[212] = {}, m[250];
        build(0, 63, d, m);
        Network n4;
        const std::string t = n4.add(135, m, 0);
        CHECK(t.find("null") != std::string::npos, "text '%s'", t.c_str());
        CHECK(std::string(typeName(18)).find("Ionospheric") == 0, "type name");
        CHECK(std::string(udreText(14)) == "not monitored", "udre text");
    }

    std::printf("%s: %d failure(s)\n", fails ? "test_sbas_msg FAILED" : "test_sbas_msg OK", fails);
    return fails ? 1 : 0;
}
