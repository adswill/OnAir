// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos message layers: every A2 length goes through buildA2 / parseA2 with its fields; nearestA2Length only gives lengths that exist; the
// HDLC deframer gets frames back from hdlcFrameBits (zero-bit insertion, back-to-back flags, the FCS), a damaged frame fails the FCS and
// the CRC-16/X.25 check value is right.
#include "dect2/argos/argos_proto.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    for (int k = 0; k <= 7; k++) {
        const int dataBits = 24 + 32 * k;
        std::vector<uint8_t> data;
        for (int i = 0; i < dataBits / 8; i++) data.push_back((uint8_t)(i * 29 + k));
        const std::vector<uint8_t> b = argos::buildA2(0x5A5A5A, k, 0x0ABCDEFu + (uint32_t)k, data, dataBits);
        CHECK((int)b.size() == argos::kMinBits + 32 * k, "k %d: %zu bits", k, b.size());
        argos::A2Message m;
        CHECK(argos::parseA2(b, m), "k %d did not parse", k);
        CHECK(m.id == 0x0ABCDEFu + (uint32_t)k && m.lenField == k && m.dataBits == dataBits && m.data == data && m.syncHex == "5A5A5A", "k %d fields", k);
        CHECK(m.idHex.size() == 7, "id hex %s", m.idHex.c_str());
    }
    CHECK(argos::nearestA2Length(80) == 80 && argos::nearestA2Length(82) == 80 && argos::nearestA2Length(110) == 112 && argos::nearestA2Length(96) == 0 && argos::nearestA2Length(400) == 0, "nearest length");
    std::vector<uint8_t> bad(81, 0);
    argos::A2Message m;
    CHECK(!argos::parseA2(bad, m), "81 bits must not parse");

    {
        const char* t = "123456789";
        CHECK(argos::fcs16((const uint8_t*)t, 9) == 0x906E, "X.25 check value");
    }
    {
        const std::vector<std::vector<uint8_t>> frames = {{0x01, 0x02, 0x03}, {0xFF, 0xFF, 0x7E, 0x7E, 0x3F, 0xFC}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
        std::vector<uint8_t> stream;
        for (int i = 0; i < 16; i++) stream.push_back((uint8_t)(i % 3 == 0));   // junk before the first flag
        for (const auto& f : frames) { const auto b = argos::hdlcFrameBits(f); stream.insert(stream.end(), b.begin(), b.end()); }
        argos::HdlcDeframer d;
        std::vector<argos::HdlcFrame> got;
        for (uint8_t v : stream) if (d.push(v)) got.push_back(d.frame());
        CHECK(got.size() == frames.size(), "%zu frames back", got.size());
        for (size_t i = 0; i < got.size() && i < frames.size(); i++) CHECK(got[i].fcsOk && got[i].bytes == frames[i], "frame %zu", i);
        // a flipped bit in the middle of a frame: the FCS fails
        std::vector<uint8_t> one = argos::hdlcFrameBits({0x10, 0x20, 0x30, 0x40});
        one[20] ^= 1;
        argos::HdlcDeframer d2;
        bool any = false, ok = false;
        for (uint8_t v : one) if (d2.push(v)) { any = true; ok = d2.frame().fcsOk; }
        CHECK(!any || !ok, "damaged frame passed the FCS");
    }
    return testSummary("argos proto");
}
