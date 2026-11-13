// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ASTERIX: records assembled byte by byte from the category descriptions (not by the encoder) decode to the expected fields; the encoder and decoder
// agree on a range of values; blocks of several records, other categories, truncated blocks and records with an item of unknown length are handled.
#include "dect2/asterix/asterix_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2::asterix;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> block(int cat, const std::vector<uint8_t>& rec) {
    std::vector<uint8_t> b = {(uint8_t)cat, (uint8_t)((rec.size() + 3) >> 8), (uint8_t)(rec.size() + 3)};
    b.insert(b.end(), rec.begin(), rec.end());
    return b;
}

int main() {
    // ---- CAT 048: 010 140 020 040 070 090
    {
        const std::vector<uint8_t> rec = {0xFC, 0x08, 0x01, 0x54, 0x60, 0x00, 0x60, 0x0A, 0x00, 0x40, 0x00, 0x0F, 0xC0, 0x05, 0x78};
        std::vector<Target> out;
        BlockStats st;
        const std::vector<uint8_t> b = block(48, rec);
        CHECK(decodeBlocks(b.data(), b.size(), out, &st) == b.size(), "consumed");
        CHECK(out.size() == 1 && st.records == 1 && st.blocks == 1, "one record, got %zu", out.size());
        if (!out.empty()) {
            const Target& t = out[0];
            CHECK(t.cat == 48 && t.sac == 8 && t.sic == 1, "source %d/%d", t.sac, t.sic);
            CHECK(t.hasTime && t.todS == 43200.0, "time %.3f", t.todS);
            CHECK(t.kind == "SSR + PSR", "kind '%s'", t.kind.c_str());
            CHECK(t.hasPolar && t.rhoNm == 10.0 && t.thetaDeg == 90.0, "polar %.3f %.3f", t.rhoNm, t.thetaDeg);
            CHECK(t.squawk == 7700 && !t.squawkInvalid, "squawk %d", t.squawk);
            CHECK(t.hasLevel && t.flightLevel == 350.0, "FL %.2f", t.flightLevel);
            CHECK(t.items == "010 140 020 040 070 090", "items '%s'", t.items.c_str());
            CHECK(!t.partial, "partial");
        }
    }
    // ---- CAT 048: 010 140 020 040 070 090 | 220 240 161 200, a negative flight level, a Mode 3/A code 1000
    {
        // FSPEC: FRN 1-6 + FX; FRN 8 (0x80), 9 (0x40), 11 (0x10), 13 (0x04)
        std::vector<uint8_t> rec = {0xFD, 0x80 | 0x40 | 0x10 | 0x04};
        const std::vector<uint8_t> items = {0x01, 0x02,                        // 010
                                            0x00, 0x00, 0x80,                  // 140: 1.0 s
                                            0x20,                              // 020: PSR
                                            0x00, 0x80, 0x00, 0x00,            // 040: 0.5 NM, 0 deg
                                            0x02, 0x00,                        // 070: 1000
                                            0xFF, 0xF8,                        // 090: V, G, -8 quarter levels
                                            0x4C, 0xA7, 0xB1,                  // 220
                                            0x3C, 0xB0, 0x42, 0x0F, 0x1C, 0xA0,   // 240: OKABC12
                                            0x00, 0xE1,                        // 161: 225
                                            0x08, 0x00, 0x80, 0x00};           // 200: 450 kt, 180 deg
        rec.insert(rec.end(), items.begin(), items.end());
        std::vector<Target> out;
        const std::vector<uint8_t> b = block(48, rec);
        decodeBlocks(b.data(), b.size(), out);
        CHECK(out.size() == 1, "records %zu", out.size());
        if (out.size() == 1) {
            const Target& t = out[0];
            CHECK(t.sac == 1 && t.sic == 2 && t.todS == 1.0, "source / time %d %d %.3f", t.sac, t.sic, t.todS);
            CHECK(t.kind == "PSR", "kind '%s'", t.kind.c_str());
            CHECK(t.hasPolar && t.rhoNm == 0.5 && t.thetaDeg == 0.0, "polar %.3f %.3f", t.rhoNm, t.thetaDeg);
            CHECK(t.squawk == 1000, "squawk %d", t.squawk);
            CHECK(t.hasLevel && t.flightLevel == -2.0, "FL %.2f", t.flightLevel);
            CHECK(t.address == 0x4CA7B1, "address %lld", (long long)t.address);
            CHECK(t.callsign == "OKABC12", "callsign '%s'", t.callsign.c_str());
            CHECK(t.trackNo == 225, "track %d", t.trackNo);
            CHECK(t.hasGs && std::fabs(t.gsKt - 450.0) < 0.01 && t.hasTrack && t.trackDeg == 180.0, "velocity %.2f kt %.2f deg", t.gsKt, t.trackDeg);
        }
    }
    // ---- CAT 021: 010 040 131 080 073 145 160 170
    {
        const std::vector<uint8_t> rec = {0xC3, 0x19, 0x03, 0x09, 0x80,
                                          0x00, 0x05,                                  // 010
                                          0x00,                                        // 040
                                          0x11, 0xC7, 0x1C, 0x72, 0x04, 0xFA, 0x4F, 0xA5,   // 131: 50.0, 14.0
                                          0x4C, 0xA7, 0xB1,                            // 080
                                          0x54, 0x60, 0x00,                            // 073
                                          0x05, 0x28,                                  // 145: FL 330
                                          0x08, 0x00, 0x80, 0x00,                      // 160: 450 kt, 180 deg
                                          0x49, 0x94, 0xB1, 0xCB, 0x3D, 0x20};         // 170: RYR1234
        std::vector<Target> out;
        const std::vector<uint8_t> b = block(21, rec);
        decodeBlocks(b.data(), b.size(), out);
        CHECK(out.size() == 1, "records %zu", out.size());
        if (out.size() == 1) {
            const Target& t = out[0];
            CHECK(t.sac == 0 && t.sic == 5, "source %d/%d", t.sac, t.sic);
            CHECK(t.hasLatLon && std::fabs(t.lat - 50.0) < 1e-6 && std::fabs(t.lon - 14.0) < 1e-6, "position %.7f %.7f", t.lat, t.lon);
            CHECK(t.address == 0x4CA7B1 && t.todS == 43200.0, "address / time");
            CHECK(t.hasLevel && t.flightLevel == 330.0, "FL %.2f", t.flightLevel);
            CHECK(t.hasGs && std::fabs(t.gsKt - 450.0) < 0.01 && t.hasTrack && t.trackDeg == 180.0, "vector %.3f %.3f", t.gsKt, t.trackDeg);
            CHECK(t.callsign == "RYR1234", "callsign '%s'", t.callsign.c_str());
            CHECK(t.items == "010 040 131 080 073 145 160 170", "items '%s'", t.items.c_str());
            CHECK(t.kind.find("ADS-B") == 0, "kind '%s'", t.kind.c_str());
        }
    }
    // ---- CAT 062: 010 070 105 185 060 245 040 136
    {
        const std::vector<uint8_t> rec = {0x9B, 0x69, 0x20,
                                          0x00, 0x07,                                  // 010
                                          0x54, 0x60, 0x00,                            // 070
                                          0x00, 0x8E, 0x81, 0xB5, 0x00, 0x28, 0x64, 0x20,   // 105: 50.1, 14.2
                                          0x01, 0x90, 0x00, 0x00,                      // 185: vx 100 m/s east
                                          0x02, 0x9C,                                  // 060: 1234
                                          0x00, 0x3C, 0xB0, 0x42, 0x0F, 0x1C, 0xA0,    // 245: OKABC12
                                          0x10, 0xE1,                                  // 040: 4321
                                          0x05, 0xF0};                                 // 136: FL 380
        std::vector<Target> out;
        const std::vector<uint8_t> b = block(62, rec);
        decodeBlocks(b.data(), b.size(), out);
        CHECK(out.size() == 1, "records %zu", out.size());
        if (out.size() == 1) {
            const Target& t = out[0];
            CHECK(t.sic == 7 && t.todS == 43200.0, "source / time");
            CHECK(t.hasLatLon && std::fabs(t.lat - 50.1) < 1e-5 && std::fabs(t.lon - 14.2) < 1e-5, "position %.7f %.7f", t.lat, t.lon);
            CHECK(t.hasGs && std::fabs(t.gsKt - 100.0 / 0.514444) < 0.01 && std::fabs(t.trackDeg - 90.0) < 1e-9, "velocity %.3f kt %.3f", t.gsKt, t.trackDeg);
            CHECK(t.squawk == 1234, "squawk %d", t.squawk);
            CHECK(t.callsign == "OKABC12", "callsign '%s'", t.callsign.c_str());
            CHECK(t.trackNo == 4321, "track %d", t.trackNo);
            CHECK(t.hasLevel && t.flightLevel == 380.0, "FL %.2f", t.flightLevel);
        }
    }
    // ---- CAT 062 with the aircraft derived data (380: ADR, ID, then COM), flight plan data (390: CSN, DEP, DST) and an item of unknown length after
    {
        // FSPEC: 010 (1), 380 (11), 040 (12), 390 (21), 500 (27)  -> bytes: FRN1 | FRN11 FRN12 | FRN21 | FRN27
        // byte 1: FRN 1 + FX; byte 2: FRN 11 (0x10), 12 (0x08) + FX; byte 3: FRN 21 (0x02) + FX; byte 4: FRN 27 (0x04)
        std::vector<uint8_t> rec = {0x81, 0x19, 0x03, 0x04};
        const std::vector<uint8_t> items = {
            0x00, 0x09,                                                  // 010
            0xC0, 0x4C, 0xA7, 0xB1, 0x0D, 0x30, 0x71, 0xCB, 0x38, 0x20,   // 380: primary 0xC0 (ADR, ID), ADR, ID "CSA123"
            0x03, 0x21,                                                  // 040: 801
            // 390: primary 0x43 0x80: CSN (subfield 2), DEP (7), FX; DST (8). Then CSN, DEP and DST
            0x43, 0x80, 'C', 'S', 'A', '1', '2', '3', ' ', 'L', 'K', 'P', 'R', 'E', 'H', 'A', 'M',
        };
        rec.insert(rec.end(), items.begin(), items.end());
        std::vector<Target> out;
        const std::vector<uint8_t> b = block(62, rec);
        BlockStats st;
        decodeBlocks(b.data(), b.size(), out, &st);
        CHECK(out.size() == 1, "records %zu", out.size());
        if (out.size() == 1) {
            const Target& t = out[0];
            CHECK(t.address == 0x4CA7B1 && t.callsign == "CSA123", "380: %06llX '%s'", (long long)t.address, t.callsign.c_str());
            CHECK(t.trackNo == 801, "track %d", t.trackNo);
            CHECK(t.dep == "LKPR" && t.dst == "EHAM", "390: '%s' '%s'", t.dep.c_str(), t.dst.c_str());
            CHECK(t.partial, "the 500 item has bytes missing: partial");
            CHECK(st.partialRecords == 1, "partial count");
        }
    }
    // ---- several records in one block, two blocks in one datagram, another category, and a block cut short
    {
        Target a;
        a.cat = 48; a.sac = 1; a.sic = 2; a.hasPolar = true; a.rhoNm = 40.5; a.thetaDeg = 270.0; a.squawk = 4321; a.hasLevel = true; a.flightLevel = 120; a.trackNo = 17;
        Target c = a; c.trackNo = 18; c.rhoNm = 41.25;
        std::vector<uint8_t> dg = wrapBlock(48, {encodeRecord(a), encodeRecord(c)});
        const std::vector<uint8_t> other = {0x22, 0x00, 0x05, 0xAA, 0xBB};     // CAT 034, 2 data bytes
        dg.insert(dg.end(), other.begin(), other.end());
        Target e;
        e.cat = 62; e.sac = 9; e.sic = 9; e.hasLatLon = true; e.lat = -33.9; e.lon = 151.2; e.trackNo = 5; e.callsign = "QFA1"; e.hasLevel = true; e.flightLevel = 410;
        const std::vector<uint8_t> b62 = wrapBlock(62, {encodeRecord(e)});
        dg.insert(dg.end(), b62.begin(), b62.end());
        std::vector<Target> out;
        BlockStats st;
        const size_t used = decodeBlocks(dg.data(), dg.size(), out, &st);
        CHECK(used == dg.size(), "consumed %zu of %zu", used, dg.size());
        CHECK(out.size() == 3 && st.blocks == 3 && st.otherCategory == 1 && st.perCat[34] == 1, "records %zu blocks %u other %u", out.size(), st.blocks, st.otherCategory);
        if (out.size() == 3) {
            CHECK(out[1].trackNo == 18 && out[1].rhoNm == 41.25 && out[0].thetaDeg == 270.0 && out[0].squawk == 4321 && out[0].flightLevel == 120.0, "048 round trip");
            CHECK(std::fabs(out[2].lat + 33.9) < 1e-5 && std::fabs(out[2].lon - 151.2) < 1e-5 && out[2].callsign == "QFA1" && out[2].flightLevel == 410.0, "062 round trip %.6f %.6f", out[2].lat, out[2].lon);
        }
        // cut short: the last block is incomplete
        std::vector<Target> o2;
        BlockStats s2;
        const size_t u2 = decodeBlocks(dg.data(), dg.size() - 3, o2, &s2);
        CHECK(u2 == dg.size() - b62.size() && o2.size() == 2 && s2.badBlocks == 1, "truncated: consumed %zu, %zu records, %u bad", u2, o2.size(), s2.badBlocks);
        // garbage length
        const uint8_t bad[] = {48, 0x00, 0x02, 0x00};
        std::vector<Target> o3;
        BlockStats s3;
        CHECK(decodeBlocks(bad, sizeof bad, o3, &s3) == 0 && s3.badBlocks == 1 && o3.empty(), "length below 3");
    }
    // ---- encoder / decoder over a range of values (quantisation inside one LSB)
    {
        for (int i = 0; i < 40; i++) {
            Target t;
            t.cat = i % 3 == 0 ? 21 : (i % 3 == 1 ? 48 : 62);
            t.sac = 10 + i; t.sic = 200 - i;
            t.hasTime = true; t.todS = 86399.0 * i / 40.0;
            t.hasLatLon = t.cat != 48;
            t.lat = -85.0 + 4.3 * i; t.lon = -179.0 + 8.7 * i;
            t.hasPolar = t.cat == 48; t.rhoNm = 0.5 + 6.1 * i; t.thetaDeg = 9.0 * i;
            t.squawk = i % 8 * 1000 + (i * 3 % 8) * 100 + (i * 5 % 8) * 10 + (i * 7 % 8);
            t.hasLevel = true; t.flightLevel = -10.0 + 11.25 * i;
            t.hasGs = t.hasTrack = true; t.gsKt = 20.0 + 11.7 * i; t.trackDeg = 8.3 * i;
            t.address = 0x100000 + 12345 * i;
            t.callsign = std::string("AB") + std::to_string(1000 + i);
            t.trackNo = 100 + i * 7;
            const std::vector<uint8_t> rec = encodeRecord(t);
            const std::vector<uint8_t> b = wrapBlock(t.cat, {rec});
            std::vector<Target> out;
            decodeBlocks(b.data(), b.size(), out);
            CHECK(out.size() == 1, "round trip %d: %zu records", i, out.size());
            if (out.size() != 1) continue;
            const Target& d = out[0];
            CHECK(d.sac == t.sac && d.sic == t.sic, "%d: source", i);
            CHECK(std::fabs(d.todS - t.todS) <= 1.0 / 256, "%d: time %.4f vs %.4f", i, d.todS, t.todS);
            if (t.hasLatLon) CHECK(std::fabs(d.lat - t.lat) < 1e-5 && std::fabs(d.lon - t.lon) < 1e-5, "%d: position %.7f %.7f vs %.7f %.7f", i, d.lat, d.lon, t.lat, t.lon);
            if (t.hasPolar) CHECK(std::fabs(d.rhoNm - t.rhoNm) <= 1.0 / 512 && std::fabs(d.thetaDeg - t.thetaDeg) <= 360.0 / 65536, "%d: polar", i);
            CHECK(d.squawk == t.squawk, "%d: squawk %d vs %d", i, d.squawk, t.squawk);
            CHECK(d.hasLevel && std::fabs(d.flightLevel - t.flightLevel) <= 0.125 + (t.cat == 62 ? 0.0 : 0.0), "%d: FL %.2f vs %.2f", i, d.flightLevel, t.flightLevel);
            CHECK(d.hasGs && std::fabs(d.gsKt - t.gsKt) < 0.5 + (t.cat == 62 ? 0.5 : 0.0), "%d: gs %.2f vs %.2f", i, d.gsKt, t.gsKt);
            if (t.cat != 62) CHECK(std::fabs(d.trackDeg - t.trackDeg) <= 360.0 / 65536 + 1e-9, "%d: track", i);
            if (t.cat == 62) CHECK(std::fabs(std::remainder(d.trackDeg - t.trackDeg, 360.0)) < 2.0, "%d: 062 track %.2f vs %.2f", i, d.trackDeg, t.trackDeg);
            CHECK(d.callsign == t.callsign, "%d: callsign '%s' vs '%s'", i, d.callsign.c_str(), t.callsign.c_str());
            CHECK(d.address == t.address, "%d: address %lld vs %lld", i, (long long)d.address, (long long)t.address);
            if (t.cat != 21) CHECK(d.trackNo == t.trackNo, "%d: track number %d vs %d", i, d.trackNo, t.trackNo);
        }
    }
    // fuzz: random bytes never run past the buffer
    {
        uint32_t s = 12345;
        std::vector<uint8_t> junk(512);
        size_t seen = 0;
        for (int round = 0; round < 3000; round++) {
            for (auto& x : junk) { s = s * 1664525u + 1013904223u; x = (uint8_t)(s >> 24); }
            junk[0] = (round % 3 == 0) ? 21 : (round % 3 == 1 ? 48 : 62);
            junk[1] = 0x01; junk[2] = 0xF0;
            std::vector<Target> out;
            decodeBlocks(junk.data(), junk.size(), out);
            seen += out.size();
        }
        std::printf("fuzz: %zu records out of 3000 random blocks\n", seen);
    }
    std::printf("%s: %d failure(s)\n", fails ? "test_asterix_proto FAILED" : "test_asterix_proto OK", fails);
    return fails ? 1 : 0;
}
