// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 2 layers without a radio signal: header code, scrambler, Reed-Solomon code with shortened blocks and erasures, interleaving,
// HDLC framing, AVLC addresses, and the decoders of what the frames carry (ACARS with ADS-C, X.25 / CLNP / COTP, XID).
#include "dect2/vdl2/vdl2_proto.h"
#include "dect2/aero/aero_adsc.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static bool bodyRoundTrip(const std::vector<uint8_t>& data, int flips, std::mt19937& rng, int& fixedOut) {
    std::vector<uint8_t> b = vdl2BurstBits(data);
    // errors in whole octets of the body, at most one octet per block per flip round
    for (int f = 0; f < flips; f++) {
        const size_t pos = kVdl2HeaderBits + 8 * (rng() % ((b.size() - kVdl2HeaderBits) / 8));
        b[pos + rng() % 8] ^= 1;
    }
    vdl2Scramble(b, 0);
    uint32_t w = 0, len = 0;
    bool corr = false;
    for (int i = 0; i < kVdl2HeaderBits; i++) w = (w << 1) | b[(size_t)i];
    Vdl2Layout l;
    if (!vdl2HeaderDecode(w, len, corr) || len != data.size() || !vdl2Layout(len, l)) return false;
    std::vector<uint8_t> out;
    if (!vdl2BurstData(b.data() + kVdl2HeaderBits, l, len, out, fixedOut)) return false;
    return out == data;
}

int main() {
    std::mt19937 rng(5);
    // header: every single-bit error in the 22 protected bits is corrected
    for (uint32_t len : {1u, 77u, 800u, 1992u, 8191u, 12000u}) {
        const uint32_t w = vdl2HeaderWord(len);
        uint32_t got = 0; bool corr = true;
        CHECK(vdl2HeaderDecode(w, got, corr) && got == len && !corr, "header %u", len);
        for (int b = 0; b < 22; b++) {
            const bool ok = vdl2HeaderDecode(w ^ (1u << b), got, corr);
            if (len <= 0x1FFF) CHECK(ok && got == len && corr, "header %u bit %d -> %u", len, b, got);
        }
        CHECK(vdl2HeaderDecode(w | (7u << 22), got, corr) && got == len, "reserved bits are ignored");
    }
    // layout rules
    Vdl2Layout l;
    CHECK(!vdl2Layout(16, l), "two octets carry no FEC: not a burst");
    CHECK(vdl2Layout(8 * 30, l) && l.lastFec == 2 && l.fecOct == 2, "30 octets: 2 FEC");
    CHECK(vdl2Layout(8 * 31, l) && l.lastFec == 4, "31 octets: 4 FEC");
    CHECK(vdl2Layout(8 * 68, l) && l.lastFec == 6, "68 octets: 6 FEC");
    CHECK(vdl2Layout(8 * 249, l) && l.blocks == 1 && l.fecOct == 6 && l.lastLen == 249, "one full block");
    CHECK(vdl2Layout(8 * 300, l) && l.blocks == 2 && l.fecOct == 10 && l.lastLen == 51, "300 octets");
    // Reed-Solomon: up to three errors in a full block, one error with four erasures
    {
        uint8_t d[255];
        for (int t = 0; t < 200; t++) {
            for (int i = 0; i < 249; i++) d[i] = (uint8_t)rng();
            vdl2RsEncode(d, d + 249);
            uint8_t c[255];
            memcpy(c, d, 255);
            const int ne = t % 4;
            for (int e = 0; e < ne; e++) c[(e * 83 + t) % 255] ^= (uint8_t)(1 + rng() % 255);
            const int f = vdl2RsDecode(c, 6);
            CHECK(f == ne && !memcmp(c, d, 255), "RS %d errors: %d", ne, f);
        }
        for (int t = 0; t < 100; t++) {
            for (int i = 0; i < 249; i++) d[i] = i < 20 ? (uint8_t)rng() : 0;
            vdl2RsEncode(d, d + 249);
            uint8_t c[255];
            memcpy(c, d, 255);
            for (int i = 251; i < 255; i++) c[i] = 0;              // only two parity octets sent
            if (t % 2) c[t % 20] ^= 0x5A;
            const int f = vdl2RsDecode(c, 2);
            CHECK(f == t % 2 && !memcmp(c, d, 251), "shortened RS: %d", f);
        }
        for (int i = 0; i < 249; i++) d[i] = (uint8_t)rng();
        vdl2RsEncode(d, d + 249);
        for (int e = 0; e < 4; e++) d[e * 50] ^= 0x33;
        CHECK(vdl2RsDecode(d, 6) < 0, "four errors are refused");
    }
    // burst body: interleaving and FEC for many lengths, with errors spread over the blocks
    int fixed = 0;
    for (int n : {17, 23, 24, 100, 250, 600, 1500, 1992, 4000, 8000}) {
        std::vector<uint8_t> data((size_t)n);
        for (auto& v : data) v = (uint8_t)(rng() & 1);
        CHECK(bodyRoundTrip(data, 0, rng, fixed) && fixed == 0, "body %d bits", n);
        if (n >= 8 * 68) CHECK(bodyRoundTrip(data, 1, rng, fixed) && fixed == 1, "body %d bits, one error", n);
    }
    // HDLC: stuffing of long runs of ones, two frames between shared flags
    {
        std::vector<uint8_t> a = {0xFF, 0xFF, 0x7E, 0x7E, 0x3F, 0x01}, b = {0x55, 0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0x12};
        const auto bits = vdl2HdlcBits({a, b});
        const auto fr = vdl2HdlcFrames(bits);
        CHECK(fr.size() == 2 && fr[0] == a && fr[1] == b, "hdlc %zu frames", fr.size());
    }
    // AVLC addresses
    for (uint32_t a : {0x000001u, 0x4CA2B1u, 0xFFFFFFu, 0x10A123u})
        for (int ty : {1, 4, 5, 7}) {
            uint8_t o[4];
            vdl2PutAddr(o, {a, ty, true}, true);
            const Vdl2Addr r = vdl2GetAddr(o);
            CHECK(r.addr == a && r.type == ty && r.status && (o[3] & 1), "address %06X type %d", a, ty);
        }
    // ACARS with ADS-C over AVLC
    {
        std::vector<uint8_t> g = aeroAdscBasicGroup(51.5, -0.4, 36000, 1234.5);
        const auto f = aeroAdscFlightIdGroup("BA0117");
        g.insert(g.end(), f.begin(), f.end());
        const std::string text = "M01ABA0117" + aeroBuildAdscText("LHRCAYA", "G-EUUA", g);
        const auto fr = vdl2Frame({0x10A000, 4, false}, {0x4CA2B1, 1, false}, vdl2ControlI(2, 5, false), vdl2AcarsInfo("G-EUUA", "B6", '3', text));
        CHECK(vdl2FcsOk(fr.data(), fr.size()), "fcs");
        Vdl2Message m;
        CHECK(vdl2ParseFrame(fr.data(), fr.size() - 2, m), "parse");
        printf("  ACARS: %s\n", m.summary.c_str());
        CHECK(m.src == 0x4CA2B1 && m.fromAir && m.dst == 0x10A000 && m.dstType == 4 && m.frameKind == 'I' && m.ns == 2 && m.nr == 5, "AVLC fields");
        CHECK(m.acars && m.acarsCrcOk && m.reg == "G-EUUA" && m.label == "B6" && m.flight == "BA0117", "ACARS fields %s %s %s", m.reg.c_str(), m.label.c_str(), m.flight.c_str());
        CHECK(m.hasPos && std::fabs(m.lat - 51.5) < 0.01 && std::fabs(m.lon + 0.4) < 0.01 && m.hasAlt && std::abs(m.altFt - 36000) < 10, "ADS-C position %.3f %.3f %d", m.lat, m.lon, m.altFt);
    }
    // X.25 data with CLNP and COTP
    {
        std::vector<uint8_t> src(20, 0x11), dst(20, 0x22), user = {1, 2, 3, 4, 5};
        const auto fr = vdl2Frame({0x10A000, 4, false}, {0x4CA2B1, 1, false}, vdl2ControlI(0, 0, false), vdl2X25ClnpData(1, 3, 4, src, dst, user));
        Vdl2Message m;
        CHECK(vdl2ParseFrame(fr.data(), fr.size() - 2, m), "parse x25");
        printf("  X.25: %s\n", m.summary.c_str());
        CHECK(m.proto == "COTP" && m.summary.find("P(S) 3 P(R) 4") != std::string::npos && m.detail.find("CLNP: DT") != std::string::npos && m.detail.find("COTP") != std::string::npos, "%s", m.detail.c_str());
        const auto cr = vdl2Frame({0x10A000, 4, false}, {0x4CA2B1, 1, false}, vdl2ControlI(1, 0, false), vdl2X25CallRequest(1, "1234", "56789"));
        Vdl2Message c;
        CHECK(vdl2ParseFrame(cr.data(), cr.size() - 2, c) && c.summary.find("call request") != std::string::npos && c.detail.find("called DTE 1234, calling DTE 56789") != std::string::npos, "%s", c.detail.c_str());
    }
    // XID: GSIF with the ground station's location, an aircraft's link establishment
    {
        const auto fr = vdl2Frame({0xFFFFFF, 7, false}, {0x10A000, 4, false}, kVdl2CtrlXID, vdl2XidGsif(51.4, -0.5, "EGLL EGKK"));
        Vdl2Message m;
        CHECK(vdl2ParseFrame(fr.data(), fr.size() - 2, m) && m.frameName == "GSIF" && m.hasPos && std::fabs(m.lat - 51.4) < 0.01 && std::fabs(m.lon + 0.5) < 0.01, "GSIF %s %.2f %.2f", m.frameName.c_str(), m.lat, m.lon);
        CHECK(m.detail.find("airport coverage EGLL EGKK") != std::string::npos && m.fromGround, "%s", m.detail.c_str());
        const auto le = vdl2Frame({0x10A000, 4, false}, {0x4CA2B1, 1, false}, kVdl2CtrlXID | 0x10, vdl2XidAircraftLe(-33.9, 151.2, 37000, "YSSY"));
        Vdl2Message a;
        CHECK(vdl2ParseFrame(le.data(), le.size() - 2, a) && a.frameName == "XID_CMD_LE" && a.hasPos && std::fabs(a.lat + 33.9) < 0.01 && std::fabs(a.lon - 151.2) < 0.01 && a.altFt == 37000, "LE %s", a.frameName.c_str());
        printf("  XID: %s / %s\n", m.summary.c_str(), a.summary.c_str());
    }
    printf(fails ? "vdl2 proto: %d FAILED\n" : "vdl2 proto: all passed\n", fails);
    return fails ? 1 : 0;
}
