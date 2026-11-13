// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox frame layer: the public example uplink from the renard project's README (device 004D33DB, counter 0x1A0, payload ABAD1DEA) must parse
// with a good CRC; Table 3-1 and Table 3-3 of the specification as coded; every payload length and rank goes through buildFrame and back; the
// convolution codes invert; a flipped bit fails the CRC.
#include "dect2/sigfox/sigfox_proto.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> hexBits(const std::string& h) {
    std::vector<uint8_t> b;
    for (char c : h) { const int v = std::stoi(std::string(1, c), nullptr, 16); for (int i = 3; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
    return b;
}

int main() {
    {   // the example frame: preamble 0xAAAAA is the 19 preamble bits and the first (0) bit of the frame type
        const std::vector<uint8_t> b = hexBits("aaaaa35f01a0db334d00abad1deadc32758a");
        sigfox::Uplink u;
        const bool ok = sigfox::parseFrame(b, sigfox::kPreambleBits, u);
        CHECK(ok, "example frame did not parse");
        if (ok) {
            CHECK(u.ft == 0x035F && u.container == 12 && u.rank == 1, "frame type %03X container %d rank %d", u.ft, u.container, u.rank);
            CHECK(u.id == 0x004D33DB, "id %08X", u.id);
            CHECK(u.mc == 0x1A0, "counter %03X", u.mc);
            CHECK(sigfox::hexOf(u.payload) == "ABAD1DEA", "payload %s", sigfox::hexOf(u.payload).c_str());
            CHECK(sigfox::hexOf(u.auth) == "DC32", "auth %s", sigfox::hexOf(u.auth).c_str());
            CHECK(u.crc == 0x758A && !u.downlinkRequest && !u.singleBit, "crc %04X", u.crc);
        }
    }
    {   // Table 3-1
        bool sb = false;
        CHECK(sigfox::payloadBytes(8, 0, sb) == 0 && !sb, "8/00");
        CHECK(sigfox::payloadBytes(8, 2, sb) == 0 && sb, "8/10 single bit");
        CHECK(sigfox::payloadBytes(12, 1, sb) == 3 && sigfox::authBytes(12, 1) == 3, "12/01");
        CHECK(sigfox::payloadBytes(16, 3, sb) == 5 && sigfox::authBytes(16, 3) == 5, "16/11");
        CHECK(sigfox::payloadBytes(20, 0, sb) == 12 && sigfox::authBytes(20, 0) == 2, "20/00");
        CHECK(sigfox::payloadBytes(12, 3, sb) < 0, "12/11 must be invalid");
        CHECK(sigfox::payloadBytes(9, 1, sb) < 0, "9/01 must be invalid");
    }
    {   // Table 3-3: every entry is found from itself, and one bit error away from it
        int n = 0;
        const sigfox::FrameType* t = sigfox::frameTypes(n);
        CHECK(n == 18, "%d frame types", n);
        for (int i = 0; i < n; i++) {
            CHECK(sigfox::frameTypeOf(t[i].ft) == &t[i], "frame type %04X", t[i].ft);
            for (int k = 0; k < 13; k++) CHECK(sigfox::frameTypeOf((uint16_t)(t[i].ft ^ (1u << k)), 1) == &t[i], "frame type %04X bit %d", t[i].ft, k);
        }
    }
    {   // the convolution codes invert
        std::vector<uint8_t> v;
        unsigned s = 7;
        for (int i = 0; i < 200; i++) { s = s * 1103515245u + 12345u; v.push_back((uint8_t)((s >> 16) & 1)); }
        for (int r = 1; r <= 3; r++) CHECK(sigfox::convDecode(sigfox::convEncode(v, r), r) == v, "rank %d", r);
        CHECK(sigfox::convEncode(v, 2) != v && sigfox::convEncode(v, 3) != v && sigfox::convEncode(v, 2) != sigfox::convEncode(v, 3), "codes differ");
    }
    {   // round trips
        for (int len = 0; len <= 12; len++)
            for (int r = 1; r <= 3; r++) {
                std::vector<uint8_t> p;
                for (int i = 0; i < len; i++) p.push_back((uint8_t)(i * 37 + r));
                const std::vector<uint8_t> b = sigfox::buildFrame(0x12345678u + (uint32_t)len, (uint32_t)(len * 100 + r), p, r, len == 5);
                sigfox::Uplink u;
                const bool ok = sigfox::parseFrame(b, sigfox::kPreambleBits, u);
                CHECK(ok, "len %d rank %d did not parse", len, r);
                if (!ok) continue;
                CHECK(u.payload == p && u.rank == r && u.id == 0x12345678u + (uint32_t)len && u.mc == (uint32_t)(len * 100 + r) && u.downlinkRequest == (len == 5), "len %d rank %d fields", len, r);
                CHECK(b.size() == (size_t)(sigfox::kPreambleBits + sigfox::frameBitsAfterPreamble(u.container)), "len %d: %zu bits", len, b.size());
                std::vector<uint8_t> bad = b;
                bad[b.size() - 30] ^= 1;
                CHECK(!sigfox::parseFrame(bad, sigfox::kPreambleBits, u), "len %d rank %d: a flipped bit passed", len, r);
            }
        for (int v = 0; v < 2; v++) {
            const std::vector<uint8_t> b = sigfox::buildFrame(0xCAFEu, 9, {}, 2, false, true, v == 1);
            sigfox::Uplink u;
            CHECK(sigfox::parseFrame(b, sigfox::kPreambleBits, u) && u.singleBit && u.bitValue == (v == 1) && u.container == 8, "single bit %d", v);
        }
    }
    return testSummary("sigfox proto");
}
