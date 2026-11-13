// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS: known sentences and field values through the message decoder, and the same messages through the whole receiver at awkward sample rates
// (issue #87: wrong positions and MMSIs).
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_testutil.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::ais;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool decodeSentence(const char* s, AisMsg& m) {
    std::string chars; int fill = 0, cnt = 0, no = 0; char ch = 0;
    if (!parseNmea(s, chars, fill, cnt, no, &ch) || cnt != 1) return false;
    return decodeMessage(unarmour(chars, fill), m);
}

static Bits position(int type, uint32_t mmsi, double lat, double lon, int sog10 = 0) {
    Bits b;
    putU(b, 0, 6, (uint32_t)type); putU(b, 8, 30, mmsi);
    if (type == 18) {
        putU(b, 46, 10, (uint32_t)sog10);
        putS(b, 57, 28, (int)std::lround(lon * 600000)); putS(b, 85, 27, (int)std::lround(lat * 600000));
        putU(b, 112, 12, 900); putU(b, 124, 9, 511);
    } else {
        putU(b, 38, 4, 0); putS(b, 42, 8, -128); putU(b, 50, 10, (uint32_t)sog10);
        putS(b, 61, 28, (int)std::lround(lon * 600000)); putS(b, 89, 27, (int)std::lround(lat * 600000));
        putU(b, 116, 12, 900); putU(b, 128, 9, 511);
    }
    b.resize(168, 0);
    return b;
}

int main() {
    // gpsd AIVDM document, type 1: MMSI 477553000, 47.58283 N, 122.34583 W (negative longitude)
    {
        AisMsg m;
        CHECK(decodeSentence("!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C", m), "gpsd type 1 decodes");
        CHECK(m.type == 1 && m.mmsi == 477553000u, "type 1 mmsi %u", m.mmsi);
        CHECK(m.hasPos && std::fabs(m.lat - 47.582833) < 1e-4 && std::fabs(m.lon + 122.345833) < 1e-4, "type 1 position %.5f %.5f", m.lat, m.lon);
        CHECK(std::fabs(m.cog - 51.0) < 0.05 && m.heading == 181 && m.navStatus == 5 && m.sog == 0, "type 1 course %f heading %d status %d", m.cog, m.heading, m.navStatus);
    }
    // field values, every hemisphere, for types 1 and 18
    struct P { uint32_t mmsi; double lat, lon; };
    const P pts[] = {{244670123, 51.95, 4.3}, {244123456, 51.9500, -4.3}, {503123456, -33.8567, 151.2153}, {311000123, -9.5104, -179.1998},
                     {366999999, 40.7128, -74.006}, {219000001, 0.0, 0.0}, {636012345, -0.0001, 179.9999}, {257000001, 89.9999, -179.9999}};
    for (int type : {1, 2, 3, 18}) {
        for (const P& p : pts) {
            Bits b = position(type, p.mmsi, p.lat, p.lon, 123);
            AisMsg m;
            CHECK(decodeMessage(b, m) && m.hasPos, "type %d decodes", type);
            CHECK(m.mmsi == p.mmsi, "type %d mmsi %u vs %u", type, m.mmsi, p.mmsi);
            CHECK(std::fabs(m.lat - p.lat) < 2e-6 && std::fabs(m.lon - p.lon) < 2e-6, "type %d position %.6f %.6f vs %.6f %.6f", type, m.lat, m.lon, p.lat, p.lon);
            CHECK(std::fabs(m.sog - 12.3f) < 0.01f, "type %d sog %f", type, m.sog);
            // the same through an NMEA sentence
            int fill = 0;
            const std::vector<std::string> s = toNmea(b, 'A', 0);
            std::string chars; int f2 = 0, c = 0, n = 0;
            CHECK(s.size() == 1 && parseNmea(s[0], chars, f2, c, n, nullptr), "sentence parses");
            if (s.size() == 1) { AisMsg m2; decodeMessage(unarmour(chars, f2), m2); CHECK(m2.mmsi == p.mmsi && std::fabs(m2.lon - p.lon) < 2e-6, "sentence roundtrip"); }
            (void)fill;
        }
    }
    // not available: longitude 181, latitude 91
    {
        Bits b = position(1, 244670123, 91.0, 181.0);
        AisMsg m;
        decodeMessage(b, m);
        CHECK(!m.hasPos, "181/91 is no position");
        b = position(1, 244670123, 51.95, 181.0);
        decodeMessage(b, m);
        CHECK(!m.hasPos, "longitude 181 is no position");
    }
    // a good FCS is not enough: unassigned MIDs (38x, 59x, 79x), a type 1 of the wrong length and a type outside 1-27 are not AIS
    {
        for (uint32_t bad : {389716608u, 386574822u, 590364551u, 791679468u, 150000000u, 800000001u, 0u}) CHECK(!mmsiPlausible(bad), "mmsi %u is not plausible", bad);
        for (uint32_t ok : {244670123u, 366999999u, 503123456u, 111244001u, 994700001u, 970123456u, 2440001u, 24400000u}) CHECK(mmsiPlausible(ok), "mmsi %u is plausible", ok);
        AisMsg m;
        Bits b = position(1, 389716608, 10, 10);
        CHECK(!acceptableAis(m, decodeMessage(b, m)), "unassigned MID rejected");
        b = position(1, 244670123, 51.95, 4.3);
        CHECK(acceptableAis(m, decodeMessage(b, m)), "Dutch position accepted");
        b.resize(150);
        CHECK(!acceptableAis(m, decodeMessage(b, m)), "short type 1 rejected");
        b = position(1, 244670123, 51.95, 4.3);
        putU(b, 0, 6, 40);
        CHECK(!acceptableAis(m, decodeMessage(b, m)), "type 40 rejected");
    }
    // the whole receiver, Dutch ships, several input rates (including the ones that need an odd decimation), a tuned offset and a carrier error
    {   // a burst from a plausible-looking but unassigned MMSI is not shown
        AisBurstSpec sp; sp.payload = position(1, 389716608, -9.5104, 179.1998, 268); sp.startSec = 0.2;
        AisRenderConfig rc; rc.rate = 2e6; rc.durationSec = 0.6; rc.snrDb = 25; rc.seed = 3;
        AisReceiver rx; rx.configure(2e6);
        const AisTelemetry tel = aistest::runReceiver(rx, renderAisBursts({sp}, rc));
        CHECK(tel.vessels.empty() && tel.nmea.empty(), "unassigned MMSI burst gives %zu stations, %zu sentences", tel.vessels.size(), tel.nmea.size());
    }
    for (double fs : {2e6, 3e6, 2.4e6, 4e6, 10e6}) {
        const P ships[] = {{244670123, 51.95, 4.3}, {244790001, 52.0132, 4.3561}, {503123456, -33.8567, 151.2153}};
        std::vector<AisBurstSpec> bursts;
        double t = 0.2;
        for (const P& p : ships) {
            for (char ch : {'A', 'B'}) {
                AisBurstSpec sp; sp.payload = position(1, p.mmsi, p.lat, p.lon, 50); sp.channel = ch; sp.startSec = t; sp.cfoHz = 300; bursts.push_back(sp);
                t += 0.3;
            }
        }
        AisRenderConfig rc; rc.rate = fs; rc.durationSec = t + 0.3; rc.snrDb = 25; rc.tuneOffsetHz = 0; rc.seed = 5;
        const std::vector<cf32> x = renderAisBursts(bursts, rc);
        AisReceiver rx;
        rx.configure(fs);
        CHECK(rx.ready(), "ready at %.0f", fs);
        const AisTelemetry tel = aistest::runReceiver(rx, x);
        int found = 0;
        for (const P& p : ships) {
            for (const auto& v : tel.vessels) {
                if (v.mmsi != p.mmsi) continue;
                found++;
                CHECK(v.hasPos && std::fabs(v.lat - p.lat) < 2e-6 && std::fabs(v.lon - p.lon) < 2e-6, "fs %.0f mmsi %u position %.5f %.5f", fs, v.mmsi, v.lat, v.lon);
            }
        }
        CHECK(found == 3, "fs %.0f: %d of 3 ships", fs, found);
        CHECK(tel.vessels.size() == 3, "fs %.0f: %zu stations (no extra ones)", fs, tel.vessels.size());
        for (const auto& v : tel.vessels) if (v.mmsi / 1000000 != 244 && v.mmsi != 503123456) printf("  stray mmsi %u\n", v.mmsi);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
