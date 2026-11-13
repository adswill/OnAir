// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM packet layer: the sync word chips against the published SoftRF constant, CRC-16 check value, XXTEA round trip, V6 key / packet round trip,
// position and velocity packing, parity rejection.
#include "dect2/flarm/flarm_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2::flarm;
#include "../common/test_util.h"

int main() {
    // Known answer: SoftRF's Legacy.h gives the sync word as the Manchester coding of 0xF531FAB6: 55 99 A5 A9 55 66 65 96
    {
        const uint8_t s[4] = {0xF5, 0x31, 0xFA, 0xB6};
        const std::vector<uint8_t> c = manchester(s, 4);
        const uint8_t want[8] = {0x55, 0x99, 0xA5, 0xA9, 0x55, 0x66, 0x65, 0x96};
        CHECK(c.size() == 64, "chips %zu", c.size());
        bool ok = c.size() == 64;
        for (int i = 0; i < 64 && ok; i++) ok = ((want[i >> 3] >> (7 - (i & 7))) & 1) == c[(size_t)i];
        CHECK(ok, "sync word chips differ from 55 99 A5 A9 55 66 65 96");
        CHECK(kSyncWord == 0xF531FAB6u, "sync constant");
    }
    // Known answer: CRC-16/CCITT-FALSE of "123456789" is 0x29B1 (the standard check value)
    CHECK(crc16((const uint8_t*)"123456789", 9) == 0x29B1, "crc check value %04X", crc16((const uint8_t*)"123456789", 9));
    // XXTEA: decrypt undoes encrypt, for 6 rounds (FLARM) and the standard rounds; a different key does not decrypt
    {
        const uint32_t key[4] = {1, 2, 3, 4}, key2[4] = {1, 2, 3, 5};
        for (int rounds : {6, 0}) {
            uint32_t v[5] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555}, w[5];
            std::memcpy(w, v, sizeof v);
            btea(w, 5, key, rounds);
            CHECK(std::memcmp(v, w, sizeof v) != 0, "encrypt changed nothing");
            uint32_t x[5]; std::memcpy(x, w, sizeof w);
            btea(x, -5, key2, rounds);
            CHECK(std::memcmp(v, x, sizeof v) != 0, "wrong key decrypted");
            btea(w, -5, key, rounds);
            CHECK(std::memcmp(v, w, sizeof v) == 0, "round trip with %d rounds", rounds);
        }
    }
    // Key derivation: depends on the 64 s step and the address; bit 23 of the time switches the table half
    {
        uint32_t a[4], b[4], c[4], d[4];
        makeV6Key(a, 1800000000u, 0x123400); makeV6Key(b, 1800000000u + 63, 0x123400); makeV6Key(c, 1800000000u + 64, 0x123400); makeV6Key(d, 1800000000u, 0x123500);
        CHECK(std::memcmp(a, b, 16) == 0 || (1800000000u >> 6) != ((1800000000u + 63) >> 6), "same step, same key");
        CHECK(std::memcmp(a, c, 16) != 0, "next step, other key");
        CHECK(std::memcmp(a, d, 16) != 0, "other address, other key");
    }
    // V6 packet round trip
    {
        V6 p;
        p.addr = 0xDD1234; p.addrType = AddrFlarm; p.aircraftType = 1; p.alt = 1450; p.vsRaw = -7;
        packPosition(54.7123, 25.2845, p.lat19, p.lon20);
        packVelocity(27.3, 215.0, 2.1, p);
        uint8_t raw[kPayloadBytes];
        const uint32_t ts = 1800000123u;
        encodeV6(p, ts, raw);
        CHECK(packetType(raw) == 0 && packetAddr(raw) == 0xDD1234, "clear header");
        V6 q;
        CHECK(decodeV6(raw, ts, q) == DecOk, "decode");
        CHECK(q.addr == p.addr && q.alt == 1450 && q.aircraftType == 1 && q.lat19 == p.lat19 && q.lon20 == p.lon20 && !q.noTrack && !q.stealth, "fields");
        double la, lo; unpackPosition(q.lat19, q.lon20, 54.0, 25.0, la, lo);
        CHECK(std::fabs(la - 54.7123) < 2e-5 && std::fabs(lo - 25.2845) < 2e-5, "position %.6f %.6f", la, lo);
        double sp, tr, cl; unpackVelocity(q, sp, tr, cl);
        CHECK(std::fabs(sp - 27.3) < 1.0 && std::fabs(tr - 215.0) < 4.0 && std::fabs(cl - 2.1) < 0.5, "velocity %.1f %.0f %.1f", sp, tr, cl);
        // a wrong time step does not give a valid packet (parity is only a 1 bit check: over many wrong steps about half must fail)
        int bad = 0;
        for (int i = 1; i <= 64; i++) { V6 z; if (decodeV6(raw, ts + 64u * (uint32_t)i, z) != DecOk) bad++; }
        CHECK(bad >= 16 && bad <= 48, "wrong key times rejected by parity: %d of 64", bad);
        // flags survive
        p.noTrack = true; p.stealth = true;
        encodeV6(p, ts, raw);
        CHECK(decodeV6(raw, ts, q) == DecOk && q.noTrack && q.stealth, "privacy flags");
        // the newer protocol is told apart by the clear header
        raw[3] = (uint8_t)((raw[3] & 0xF0) | 2);
        CHECK(packetType(raw) == 2, "type 2");
        V6 z; CHECK(decodeV6(raw, ts, z) == DecNotV6, "type 2 is not decoded");
    }
    // Positions wrap around the reference: negative and far longitudes
    {
        const double pts[][2] = {{-33.9, 151.2}, {48.1, 11.6}, {0.0, -0.1}, {47.2, 8.9}};
        for (auto& pt : pts) {
            uint32_t a, o; packPosition(pt[0], pt[1], a, o);
            double la, lo; unpackPosition(a, o, pt[0] + 1.5, pt[1] - 2.0, la, lo);
            CHECK(std::fabs(la - pt[0]) < 2e-5 && std::fabs(lo - pt[1]) < 2e-5, "wrap %.1f %.1f -> %.5f %.5f", pt[0], pt[1], la, lo);
        }
    }
    // frame chips: preamble-less frame is 64 + 416 chips, the CRC checks
    {
        uint8_t raw[kPayloadBytes] = {1, 2, 3};
        const std::vector<uint8_t> c = frameToChips(raw);
        CHECK(c.size() == 64 + 416, "frame chips %zu", c.size());
        uint8_t f[kFrameBytes];
        for (int i = 0; i < kFrameBytes; i++) { int v = 0; for (int b = 0; b < 8; b++) v = (v << 1) | c[(size_t)(64 + 16 * i + 2 * b + 1)]; f[i] = (uint8_t)v; }
        CHECK(checkCrc(f) && std::memcmp(f, raw, kPayloadBytes) == 0, "frame decode");
        f[5] ^= 0x10;
        CHECK(!checkCrc(f), "bad crc accepted");
    }
    printf(fails ? "flarm proto: %d FAILED\n" : "flarm proto: all passed\n", fails);
    return fails ? 1 : 0;
}
