// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-L air format: the worked examples of the EASA specification (Issue 2), the CRC and XXTEA against the specification's own routines, Reed-Solomon, and
// every field value and parameter combination through pack and parse. Loopback and spec examples: no on-air recording was available.
#include "dect2/adsl/adsl_gen.h"
#include "dect2/adsl/adsl_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::adsl;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// The specification's sample (section E.2), kept verbatim as the reference
#define SPEC_MX ((((z>>5)^(y<<2))+((y>>3)^(z<<4)))^((sum^y)+(key[(p&3)^e]^z)) )
static void specXxtea(uint32_t* data, uint32_t num, const uint32_t* key, uint32_t round) {
    uint32_t z, y = data[0], sum = 0, e, DELTA = 0x9e3779b9;
    uint32_t p, q;
    z = data[num - 1];
    q = round;
    while (q-- > 0) {
        sum += DELTA;
        e = sum >> 2 & 3;
        for (p = 0; p < num - 1; p++) { y = data[p + 1]; data[p] += SPEC_MX; z = data[p]; }
        y = data[0];
        data[num - 1] += SPEC_MX;
        z = data[num - 1];
    }
}

int main() {
    // CRC: identical to Mode S, here on a well-known DF17 frame
    {
        const uint8_t f[11] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0};
        CHECK(crc24(f, 11) == 0x576098, "crc %06X", crc24(f, 11));
    }
    // CRC8 start value and a hand check of one byte
    { const uint8_t z = 0; uint8_t c = 0x71; for (int i = 0; i < 8; i++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 7 : c << 1); CHECK(crc8(&z, 1) == c, "crc8"); }
    // Scrambling equals the specification's XXTEA with the zero key, 6 rounds
    {
        Traffic t; t.addr = 0x123456; t.amt = AmtIcao; t.hasPos = true; t.lat = 54.7; t.lon = 25.3;
        uint8_t p[15]; packTraffic(t, p);
        auto d = makeData(TypeTraffic, t.amt, t.addr, false, p, 15);
        CHECK(d.size() == (size_t)kTrafficDataBytes, "data size %zu", d.size());
        uint32_t w[5], key[4] = {};
        std::memcpy(w, d.data(), 20);
        specXxtea(w, 5, key, 6);
        auto net = makeNetwork(d, 1, 0, false);
        CHECK(net.size() == (size_t)kTrafficNetBytes, "net size %zu", net.size());
        CHECK(!std::memcmp(net.data() + 1, w, 20), "XXTEA differs from the specification's routine");
        // key index 3: not scrambled
        auto clear = makeNetwork(d, 1, 3, false);
        CHECK(!std::memcmp(clear.data() + 1, d.data(), 20), "key 3 must be clear");
    }
    // Header bytes: example 2 of B.3 reads the same way (A = 0x12AA, 14 bits; B = 0x355, 10 bits), as the sender address does
    {
        const uint8_t pay[3] = {};
        auto d = makeData(0, 0x2A & 63, 0x4A9554, false, pay, 3);
        CHECK(d[1] == (uint8_t)(0x2A | (0x54 & 3) << 6), "address low byte %02X", d[1]);
    }
    // Tables of G.1.7 - G.1.9
    CHECK(expEncode(320, 12) == 0x0140 && expEncode(1320, 12) == 0x0528 && expEncode(0, 12) == 0, "altitude examples");
    CHECK(expEncode(61104 + 320, 12) == 0x3FFE && expEncode(70000, 12) == 0x3FFE, "altitude clamp %X", expEncode(70000, 12));
    CHECK(expEncode(0, 6) == 0 && expEncode(1, 6) == 1 && expEncode(3, 6) == 3 && expEncode(480, 6) == 0xC4 && expEncode(944, 6) == 0xFE && expEncode(5000, 6) == 0xFE, "speed examples");
    CHECK(expDecode(0xC4, 6) == 480 && expDecode(0xFE, 6) == 944, "speed decode");
    for (uint32_t v = 0; v <= 4095; v++) CHECK(expDecode(expEncode(v, 12), 12) == v, "alt roundtrip %u", v);
    {
        Traffic a, b; uint8_t p[15];
        auto climbRaw = [&](double ms) { a = Traffic(); a.hasClimb = true; a.climbMs = ms; packTraffic(a, p); uint32_t v = 0; for (int i = 0; i < 9; i++) if (p[(86 + i) >> 3] >> ((86 + i) & 7) & 1) v |= 1u << i; return v; };
        CHECK(climbRaw(0) == 0 && climbRaw(0.125) == 1 && climbRaw(-0.125) == 0x101 && climbRaw(10) == 0x048 && climbRaw(-10) == 0x148, "climb examples");
        CHECK(climbRaw(119) == 0x0FF && climbRaw(200) == 0x0FF && climbRaw(-118) == 0x1FE && climbRaw(-300) == 0x1FE, "climb limits %X", climbRaw(-300));
        a = Traffic(); packTraffic(a, p); unpackTraffic(p, 6, 1, false, b);
        CHECK(!b.hasPos && !b.hasAlt && !b.hasSpeed && !b.hasClimb && !b.hasTrack, "invalid values stay invalid");
    }
    // Every field value through pack and parse, with every network parameter combination
    {
        int n = 0;
        for (int version : {0, 1, 7}) for (int key : {0, 3}) for (bool fec : {false, true}) for (bool relay : {false, true})
        for (int amt : {0, 1, 5, 6, 7, 8, 9, 63}) for (int cat = 0; cat < 32; cat += (fec ? 7 : 1)) for (int st = 0; st < 4; st++) for (int em = 0; em < 8; em++) {
            Traffic t; t.amt = amt; t.addr = 0x123456u ^ (uint32_t)(cat * 77); t.relay = relay; t.category = cat; t.flightState = st; t.emergency = em; t.timeQ = (cat + st + em) % 60;
            t.hasPos = true; t.lat = -80.0 + cat * 5.1; t.lon = -170.0 + em * 45.3; t.hasAlt = true; t.altM = -320 + cat * 900;
            t.hasSpeed = true; t.speedMs = st * 2.75; t.hasClimb = true; t.climbMs = em - 3.5; t.hasTrack = st != 0; t.trackDeg = em * 40.0;
            t.sil = em & 3; t.design = st; t.nic = cat & 15; t.hAcc = em; t.vAcc = st; t.velAcc = (st + 1) & 3;
            auto net = adslTrafficNet(t, version, key, fec);
            CHECK(net.size() == (fec ? 53u : 24u), "net size");
            int fixed = checkParity(net.data(), net.size());
            CHECK(fixed == 0, "parity");
            const Parsed q = parseNetwork(net.data(), net.size());
            CHECK(q.kind == Parsed::TrafficMsg && q.version == version && q.keyIndex == key && q.fec == fec && q.relay == relay && q.amt == amt && q.addr == t.addr && q.type == TypeTraffic, "header");
            if (q.targets.size() != 1) { fails++; continue; }
            const Traffic& r = q.targets[0];
            CHECK(r.category == cat && r.flightState == st && r.emergency == em && r.timeQ == t.timeQ && r.sil == t.sil && r.design == t.design && r.nic == t.nic && r.hAcc == t.hAcc && r.vAcc == t.vAcc && r.velAcc == t.velAcc, "fields");
            CHECK(r.hasPos && std::fabs(r.lat - t.lat) < 1.1e-5 && std::fabs(r.lon - t.lon) < 2.2e-5, "position");
            CHECK(r.hasAlt && std::abs(r.altM - t.altM) <= 8, "altitude %d %d", r.altM, t.altM);
            CHECK(r.hasSpeed && std::fabs(r.speedMs - t.speedMs) < 0.13, "speed");
            CHECK(r.hasClimb && std::fabs(r.climbMs - t.climbMs) < 0.07, "climb");
            CHECK(r.hasTrack == t.hasTrack && (!t.hasTrack || std::fabs(r.trackDeg - t.trackDeg) < 0.4), "track");
            n++;
        }
        std::printf("adsl proto: %d combinations\n", n);
    }
    // Reserved key indices are counted, not decoded
    {
        Traffic t; t.addr = 1;
        auto net = adslTrafficNet(t, 1, 1, false);
        CHECK(parseNetwork(net.data(), net.size()).kind == Parsed::Encrypted, "key 1");
    }
    // Status and Traffic Uplink layouts
    {
        Status s; s.maxVersion = 1; s.mRx = 3; s.ldrRx = 2; s.hdrRx = 1; s.mobile = 2; s.econRx = 0xA5; s.econTx = 0x5A; s.xpdr = 2; s.trafficClient = true; s.fisClient = true;
        uint8_t p[7]; packStatus(s, p);
        auto net = makeNetwork(makeData(TypeStatus, AmtFlarm, 0x112233, false, p, 7), 1, 0, false);
        CHECK(net.size() == (size_t)kStatusNetBytes, "status size");
        const Parsed q = parseNetwork(net.data(), net.size());
        CHECK(q.kind == Parsed::StatusMsg && q.status.maxVersion == 1 && q.status.mRx == 3 && q.status.ldrRx == 2 && q.status.hdrRx == 1 && q.status.mobile == 2 && q.status.econRx == 0xA5 && q.status.econTx == 0x5A && q.status.xpdr == 2 && q.status.trafficClient && q.status.fisClient && q.addr == 0x112233, "status fields");
    }
    // Reed-Solomon: a clean block, up to 16 symbol errors, shortened blocks, and too many errors never pass as a good block silently
    {
        std::mt19937 rng(1);
        for (int len : {53, 100, 255}) {
            std::vector<uint8_t> blk((size_t)len);
            for (auto& b : blk) b = (uint8_t)rng();
            uint8_t par[kRsParity];
            rsEncode(blk.data(), (size_t)len - kRsParity, par);
            std::memcpy(blk.data() + len - kRsParity, par, kRsParity);
            CHECK(rsDecode(blk.data(), (size_t)len) == 0, "clean rs %d", len);
            for (int ne = 1; ne <= 16; ne++) {
                auto x = blk;
                for (int i = 0; i < ne; i++) x[(size_t)(rng() % (unsigned)len)] ^= (uint8_t)(1 + rng() % 255);
                const int r = rsDecode(x.data(), (size_t)len);
                CHECK(r >= 0 && r <= ne && x == blk, "rs %d errors, len %d: %d", ne, len, r);
            }
            int miscorrected = 0;
            for (int trial = 0; trial < 200; trial++) {
                auto x = blk;
                for (int i = 0; i < 20; i++) x[(size_t)(rng() % (unsigned)len)] ^= (uint8_t)(1 + rng() % 255);
                const int r = rsDecode(x.data(), (size_t)len);
                if (r >= 0 && x != blk) miscorrected++;
                CHECK(r < 0 || x != blk || r <= 20, "rs beyond capacity");
            }
            CHECK(miscorrected == 0, "%d miscorrections at len %d", miscorrected, len);
        }
    }
    // CRC repair: a wrong bit found with and without help from the weakest bits
    {
        Traffic t; t.addr = 0xABCDEF; t.hasPos = true; t.lat = 54; t.lon = 25;
        const auto net = adslTrafficNet(t);
        for (int b = 0; b < 192; b += 7) {
            auto x = net; x[(size_t)(b >> 3)] ^= (uint8_t)(0x80 >> (b & 7));
            int weak[4] = {100, 101, 102, 103};
            CHECK(correctCrc(x.data(), weak, 4) == 1 && x == net, "single bit %d", b);
        }
        for (int b = 0; b + 40 < 192; b += 11) {
            auto x = net; x[(size_t)(b >> 3)] ^= (uint8_t)(0x80 >> (b & 7)); x[(size_t)((b + 40) >> 3)] ^= (uint8_t)(0x80 >> ((b + 40) & 7));
            int weak[4] = {b + 40, 5, 6, 7};
            CHECK(correctCrc(x.data(), weak, 4) == 2 && x == net, "two bits %d", b);
        }
    }
    printf(fails ? "adsl proto: %d FAILED\n" : "adsl proto: all passed\n", fails);
    return fails ? 1 : 0;
}
