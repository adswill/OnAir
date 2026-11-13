// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave frame layer: checksum and CRC-16, build / parse round trips (singlecast, ack, multicast, R3), names, encrypted payloads
#include "dect2/zwave/zwave_proto.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

int main() {
    const uint8_t cc[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(zwave::crc16(cc, 9) == 0xE5CC, "CRC-16/AUG-CCITT check value %04X", zwave::crc16(cc, 9));
    const uint8_t x[] = {0x01, 0x02, 0x04};
    CHECK(zwave::checksum8(x, 3) == (uint8_t)(0xFF ^ 1 ^ 2 ^ 4), "checksum");
    for (int rate = 1; rate <= 3; rate++) {
        zwave::Frame f;
        f.homeId = 0xC0FFEE01; f.src = 1; f.dst = 5; f.ackReq = true; f.seq = 9; f.payload = {0x25, 0x01, 0xFF};
        auto m = zwave::build(f, rate);
        CHECK(m.size() == 9 + 3 + (size_t)zwave::checkOctets(rate) && m[7] == m.size(), "length field %d", m[7]);
        zwave::Frame g;
        CHECK(zwave::parse(m.data(), m.size(), rate, g) && g.checkOk, "parse R%d", rate);
        CHECK(g.homeId == 0xC0FFEE01 && g.src == 1 && g.dst == 5 && g.ackReq && g.seq == 9 && g.payload == f.payload, "fields");
        CHECK(zwave::describe(g) == "SWITCH_BINARY Set on", "describe '%s'", zwave::describe(g).c_str());
        m[10] ^= 0x10;
        CHECK(zwave::parse(m.data(), m.size(), rate, g) && !g.checkOk, "corrupted octet is caught");
        m[10] ^= 0x10;
        m.pop_back();
        CHECK(!zwave::parse(m.data(), m.size(), rate, g), "length mismatch");
    }
    zwave::Frame mc;
    mc.homeId = 1; mc.src = 1; mc.headerType = zwave::kHdrMulticast; mc.group = {2, 3, 40}; mc.payload = {0x25, 0x01, 0x00};
    auto mm = zwave::build(mc, 2);
    zwave::Frame g;
    CHECK(zwave::parse(mm.data(), mm.size(), 2, g) && g.checkOk && g.group == mc.group && g.payload == mc.payload, "multicast group %zu", g.group.size());
    zwave::Frame ack;
    ack.homeId = 1; ack.src = 2; ack.dst = 1; ack.headerType = zwave::kHdrAck;
    auto am = zwave::build(ack, 2);
    CHECK(am.size() == 10 && zwave::parse(am.data(), am.size(), 2, g) && zwave::describe(g) == "ack", "ack %zu octets", am.size());
    // encrypted payloads are named, never decoded
    std::vector<uint8_t> s0 = {0x98, 0x81, 1, 2, 3}, s2 = {0x9F, 0x03, 9, 9}, plain = {0x98, 0x40};
    CHECK(zwave::encryption(s0) == 1 && zwave::encryption(s2) == 2 && zwave::encryption(plain) == 0, "encryption flags");
    zwave::Frame e; e.payload = s0;
    CHECK(zwave::describe(e).find("S0 encrypted") != std::string::npos, "S0 text");
    e.payload = s2;
    CHECK(zwave::describe(e).find("S2 encrypted") != std::string::npos, "S2 text");
    e.payload = plain;
    CHECK(zwave::describe(e) == "SECURITY Nonce Get", "nonce get '%s'", zwave::describe(e).c_str());
    e.payload = {0x31, 0x05, 0x01, 0x22, 0x00, 0xDC};
    CHECK(zwave::describe(e).find("temperature 22.0 C") != std::string::npos, "sensor '%s'", zwave::describe(e).c_str());
    CHECK(std::strcmp(zwave::ccName(0x62), "DOOR_LOCK") == 0 && std::strcmp(zwave::ccName(0x9F), "SECURITY_2") == 0, "names");
    CHECK(zwave::region("EU") && zwave::region("US") && !zwave::region("XX") && zwave::regions().size() >= 8, "regions");
    CHECK(zwave::nodeText(255) == "all" && zwave::homeText(0xABCD) == "0000ABCD", "text");
    if (fails) printf("zwave proto: %d FAILED\n", fails); else printf("zwave proto: all passed\n");
    return fails ? 1 : 0;
}
