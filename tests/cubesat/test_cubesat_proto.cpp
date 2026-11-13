// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CubeSat framings without a radio: Reed-Solomon (255,223) in both bases, full and shortened, up to 16 byte errors and beyond; Golay (24,12)
// with every error pattern of up to three bits on a few words; the CCSDS randomiser sequence; AX100 Mode 5 / Mode 6 and CCSDS frames
// built, damaged and decoded; the CSP header; the satellite table as text.
#include "dect2/cubesat/cubesat_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2::cubesat;
#include "../common/test_util.h"

static void rsCase(size_t dataLen, int errs, bool dual, std::mt19937& rng) {
    std::vector<uint8_t> d(dataLen);
    for (auto& v : d) v = (uint8_t)rng();
    uint8_t p[kRsParity];
    if (dual) rsParityDual(d.data(), d.size(), p); else rsParity(d.data(), d.size(), p);
    std::vector<uint8_t> blk = d;
    blk.insert(blk.end(), p, p + kRsParity);
    std::vector<uint8_t> bad = blk;
    std::vector<size_t> pos(bad.size());
    for (size_t i = 0; i < pos.size(); i++) pos[i] = i;
    std::shuffle(pos.begin(), pos.end(), rng);
    for (int e = 0; e < errs; e++) bad[pos[(size_t)e]] ^= (uint8_t)(1 + rng() % 255);
    const int r = dual ? rsDecodeDual(bad.data(), bad.size()) : rsDecode(bad.data(), bad.size());
    if (errs <= 16) CHECK(r == errs && bad == blk, "RS %s len %zu errs %d: got %d", dual ? "dual" : "conv", dataLen, errs, r);
    else CHECK(r < 0, "RS %s len %zu errs %d: claims %d", dual ? "dual" : "conv", dataLen, errs, r);
}

int main() {
    std::mt19937 rng(5);
    // Reed-Solomon
    for (bool dual : {false, true})
        for (size_t len : {223u, 100u, 20u})
            for (int e : {0, 1, 5, 16, 17, 20}) rsCase(len, e, dual, rng);
    {   // a known value: the parity of all zero data is zero, and one byte of data gives a non-zero parity
        uint8_t z[223] = {}, p[32];
        rsParity(z, 223, p);
        bool zero = true; for (uint8_t v : p) zero &= v == 0;
        CHECK(zero, "zero data, zero parity");
        z[222] = 1; rsParity(z, 223, p);
        bool nz = false; for (uint8_t v : p) nz |= v != 0;
        CHECK(nz, "parity of one byte");
    }
    // Golay
    int gbad = 0;
    for (uint32_t d : {0u, 1u, 0x5A5u, 0xFFFu, 0x8C3u, 0x4D2u}) {
        const uint32_t w = golayEncode(d);
        CHECK(golayDecode(w) == (int)d, "golay clean %03X", d);
        for (int a = 0; a < 24; a++)
            for (int b = a; b < 24; b++)
                for (int c = b; c < 24; c++) {
                    uint32_t e = (1u << a) | (1u << b) | (1u << c);
                    int n = 0;
                    if (golayDecode(w ^ e, &n) != (int)d) gbad++;
                }
    }
    CHECK(gbad == 0, "golay: %d patterns of up to 3 errors not corrected", gbad);
    {   // four errors are never "corrected" back to the sent word
        const uint32_t w = golayEncode(0x123);
        CHECK(golayDecode(w ^ 0xF) != 0x123, "golay 4 errors");
    }
    // randomiser
    {
        uint8_t z[8] = {};
        ccsdsRandomize(z, 8);
        const uint8_t want[8] = {0xFF, 0x48, 0x0E, 0xC0, 0x9A, 0x0D, 0x70, 0xBC};
        CHECK(std::memcmp(z, want, 8) == 0, "randomiser %02X %02X %02X %02X", z[0], z[1], z[2], z[3]);
    }
    // CSP
    CspHeader h; h.prio = 2; h.src = 1; h.dst = 10; h.dport = 31; h.sport = 5; h.flags = 0x01;
    const std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o', ' ', 'c', 'u', 'b', 'e'};
    const std::vector<uint8_t> csp = buildCsp(h, payload);
    CspHeader g;
    CHECK(parseCsp(csp, g) && g.prio == 2 && g.src == 1 && g.dst == 10 && g.dport == 31 && g.sport == 5 && g.flags == 1, "CSP round trip");
    // Mode 5
    {
        std::vector<uint8_t> air = ax100Mode5Encode(csp);
        CHECK(air.size() == 4 + 3 + csp.size() + 32, "mode 5 length %zu", air.size());
        const uint32_t asmw = ((uint32_t)air[0] << 24) | ((uint32_t)air[1] << 16) | ((uint32_t)air[2] << 8) | air[3];
        CHECK(asmw == kAsmAx100, "mode 5 sync word");
        uint32_t gw = ((uint32_t)air[4] << 16) | ((uint32_t)air[5] << 8) | air[6];
        gw ^= 0x000801;                                       // two bit errors in the length field
        const int f = golayDecode(gw);
        CHECK(f >= 0 && (f & 0xFF) == (int)(csp.size() + 32) && (f & kAx100Rs) && (f & kAx100Scrambler), "mode 5 length field %X", f);
        std::vector<uint8_t> body(air.begin() + 7, air.end());
        for (int i = 0; i < 10; i++) body[(size_t)(i * 3)] ^= 0x5A;           // ten byte errors
        std::vector<uint8_t> out; int rs = -2;
        CHECK(ax100Mode5Decode(body.data(), body.size(), f & 0xF00, out, rs) && out == csp && rs == 10, "mode 5 decode rs %d", rs);
    }
    // Mode 6
    {
        std::vector<uint8_t> blk = ax100Mode6Encode(csp);
        blk[2] ^= 0xFF; blk[blk.size() - 1] ^= 1;
        std::vector<uint8_t> out; int rs = -2;
        CHECK(ax100Mode6Decode(blk, out, rs) && out == csp && rs == 2, "mode 6 decode rs %d", rs);
    }
    // CCSDS, full and shortened
    for (int L : {255, 128}) {
        std::vector<uint8_t> d(payload);
        std::vector<uint8_t> air = ccsdsEncode(d, L);
        CHECK((int)air.size() == 4 + L, "ccsds length");
        std::vector<uint8_t> blk(air.begin() + 4, air.end());
        for (int i = 0; i < 16; i++) blk[(size_t)(i * 5)] ^= 0x33;
        std::vector<uint8_t> out; int rs = -2;
        d.resize((size_t)(L - 32), 0);
        CHECK(ccsdsDecode(blk.data(), blk.size(), out, rs) && out == d && rs == 16, "ccsds %d decode rs %d", L, rs);
    }
    // the satellite table
    {
        std::vector<SatDef> v = defaultSats();
        v.push_back({"Test|sat", 99999, 437.125, Ax100Mode5, 9600, "TEST"});
        const std::vector<SatDef> w = satsFromText(satsToText(v));
        CHECK(w.size() == v.size() && w.back().name == "Test sat" && w.back().norad == 99999 && w.back().framing == Ax100Mode5 && w.back().baud == 9600 && w.back().call == "TEST",
              "table round trip");
        CHECK(w.front().call == "RS0ISS", "default table");
    }
    CHECK(hexDump({0x41, 0x42, 0x00}).find("41 42 00") != std::string::npos && asciiOf({0x41, 0x00}) == "A.", "dump");
    printf(fails ? "cubesat proto: %d FAILED\n" : "cubesat proto: all passed\n", fails);
    return fails ? 1 : 0;
}
