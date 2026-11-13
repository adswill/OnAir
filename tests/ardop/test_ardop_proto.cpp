// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP frame layer: the frame type table, type parity, CRC-16, Reed-Solomon (golden values from an independent implementation),
// six-bit call signs, and the bodies of ID, connect, ping, acknowledge and 4FSK data frames.
#include "dect2/ardop/ardop_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace dect2::ardop;
#include "../common/test_util.h"

static bool same(const std::vector<uint8_t>& v, std::initializer_list<int> g) {
    if (v.size() != g.size()) return false;
    size_t i = 0;
    for (int x : g) if (v[i++] != (uint8_t)x) return false;
    return true;
}

int main() {
    // the table
    FrameDef d;
    CHECK(frameDef(0x30, d) && d.kind == KId && d.name == "IDFrame", "id");
    CHECK(frameDef(0x32, d) && d.kind == KConReq && d.name == "ConReq500M" && d.bandwidth == 500, "conreq %s", d.name.c_str());
    CHECK(frameDef(0x38, d) && d.name == "ConReq2000F", "conreq 2000 F");
    CHECK(frameDef(0x3A, d) && d.kind == KConAck && d.name == "ConAck500", "conack");
    CHECK(frameDef(0x3D, d) && d.kind == KPingAck && frameDef(0x3E, d) && d.kind == KPing, "ping");
    CHECK(frameDef(0x4A, d) && d.name == "4FSK.500.100.E" && d.fsk && d.dataBytes == 64 && d.rsBytes == 16 && !d.odd, "4fsk even %s", d.name.c_str());
    CHECK(frameDef(0x4B, d) && d.name == "4FSK.500.100.O" && d.odd, "4fsk odd");
    CHECK(frameDef(0x44, d) && d.kind == KData && !d.fsk && d.name == "8PSK.200.100.E" && d.dataBytes == 108, "8psk");
    CHECK(frameDef(0x7A, d) && d.baud == 600 && !d.fsk, "600 baud is not read");
    CHECK(frameDef(0xF0, d) && d.kind == KAck && ackQuality(0xF0) == 70, "ack quality %d", ackQuality(0xF0));
    CHECK(frameDef(0x05, d) && d.kind == KNak && ackQuality(0x05) == 48, "nak quality");
    CHECK(!frameDef(0x25, d) && !frameDef(0x3F, d) && !frameDef(0x4E, d) && !frameDef(0x80, d), "unused codes");
    int known = 0;
    for (int c = 0; c < 256; c++) known += frameDef(c, d) ? 1 : 0;
    CHECK(known == 32 + 32 + 6 + 1 + 8 + 4 + 2 + 36, "table size %d", known);

    // type parity: 1 xor the four symbols
    CHECK(typeParity(0x30) == 2 && typeParity(0x00) == 1 && typeParity(0xFF) == 1 && typeParity(0x4A) == 0, "parity %d %d", typeParity(0x30), typeParity(0x4A));

    // CRC-16 and Reed-Solomon: values from the reference implementation
    uint8_t c12[12];
    for (int i = 0; i < 12; i++) c12[i] = (uint8_t)(i + 1);
    CHECK(crc16(c12, 12) == 0xD58C, "crc %04X", crc16(c12, 12));
    std::vector<uint8_t> v(c12, c12 + 12);
    rsAppend(v, 4);
    CHECK(same(v, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x2B, 0xEA, 0x74, 0xA3}), "rs 12+4");
    {
        auto w = v;
        CHECK(rsCorrect(w.data(), 16, 4) == 0, "clean");
        w[3] ^= 0x55; w[14] ^= 0x21;
        CHECK(rsCorrect(w.data(), 16, 4) == 2 && w == v, "two errors fixed");
        w[0] ^= 1; w[5] ^= 7; w[9] ^= 9;
        CHECK(rsCorrect(w.data(), 16, 4) == -1 || w != v, "three errors are not claimed fixed");
    }
    {   // every length used by the frames, errors up to rs/2 anywhere
        srand(3);
        for (int rs : {4, 8, 16}) {
            for (int trial = 0; trial < 60; trial++) {
                const int dl = rs == 4 ? 12 : rs == 8 ? 35 : 67;
                std::vector<uint8_t> a((size_t)dl);
                for (auto& x : a) x = (uint8_t)rand();
                auto ref = a;
                rsAppend(a, rs);
                ref = a;
                const int nerr = 1 + rand() % (rs / 2);
                for (int e = 0; e < nerr; e++) a[(size_t)(rand() % (dl + rs))] ^= (uint8_t)(1 + rand() % 255);
                CHECK(rsCorrect(a.data(), dl + rs, rs) >= 0 && a == ref, "rs %d with %d errors", rs, nerr);
            }
        }
    }

    // six-bit call signs
    CHECK(unpacked6(packed6("LY2ABC-5").data()) == "LY2ABC-5", "call round trip");
    CHECK(unpacked6(packed6("ly2abc").data()) == "LY2ABC", "lower case folded up, trailing spaces gone");
    CHECK(unpacked6(packed6("KO24HL").data()) == "KO24HL" && packed6("").size() == 6, "locator");
    {
        const auto p = packed6("A");   // 'A' = 33, then seven spaces (0): 100001 000000 000000 000000
        CHECK(p[0] == 0x84 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x00, "packing %02X %02X", p[0], p[1]);
    }

    // frame bodies
    {
        auto id = makeIdFrame("LY2ABC", "KO24HL");
        CHECK(id.size() == 16 && bodyLength(0x30) == 16 && bodyLength(0x32) == 16 && bodyLength(0x3E) == 16, "id size");
        Parsed p;
        CHECK(parseBody(0x30, id.data(), id.size(), p) && p.bodyOk && p.from == "LY2ABC" && p.locator == "KO24HL", "id parse");
        id[1] ^= 0xFF; id[13] ^= 0x10;
        Parsed q;
        CHECK(parseBody(0x30, id.data(), id.size(), q) && q.rsFixed == 2 && q.from == "LY2ABC", "id with two byte errors");
        auto cr = makeCallFrame(0x32, "LY2ABC", "LY1XYZ-3");
        Parsed r;
        CHECK(parseBody(0x32, cr.data(), cr.size(), r) && r.from == "LY2ABC" && r.to == "LY1XYZ-3", "conreq parse");
        auto ca = makeConAck(0x3A, 160, 0xFF);
        Parsed s;
        CHECK(ca.size() == 3 && parseBody(0x3A, ca.data(), 3, s) && s.leaderMs == 160, "conack leader");
        ca[1] ^= 0x33;
        Parsed s2;
        CHECK(parseBody(0x3A, ca.data(), 3, s2) && s2.leaderMs == 160, "conack majority");
        auto pa = makePingAck(12, 80);
        Parsed t;
        CHECK(parseBody(0x3D, pa.data(), 3, t) && t.snrDb == 12 && t.quality == 80, "pingack %d %d", t.snrDb, t.quality);
        auto pa2 = makePingAck(30, 100);
        Parsed t2;
        CHECK(parseBody(0x3D, pa2.data(), 3, t2) && t2.snrDb == 21 && t2.quality == 100, "pingack saturates");
    }
    {   // 4FSK data frames
        const std::string msg = "HELLO FROM LY2ABC";
        std::vector<uint8_t> payload(msg.begin(), msg.end()), blk;
        CHECK(makeDataBlock(0x4C, payload, blk) && (int)blk.size() == bodyLength(0x4C) && bodyLength(0x4C) == 43, "block size");
        // the same block as the reference implementation makes it (HELLO only, golden)
        std::vector<uint8_t> hello{'H', 'E', 'L', 'L', 'O'}, gb;
        makeDataBlock(0x4C, hello, gb);
        CHECK(same(gb, {5, 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xA2, 0xFC, 0x9A, 0xDD, 0xDB, 0x4D, 0xDE, 0xB7, 0xD9, 0x9C}), "500S golden block");
        Parsed p;
        CHECK(parseBody(0x4C, blk.data(), blk.size(), p) && p.payload == payload, "data parse");
        blk[5] ^= 0xFF; blk[20] ^= 0x01; blk[38] ^= 0x80; blk[41] ^= 0x10;   // four errors, rs 8 corrects four
        Parsed q;
        CHECK(parseBody(0x4C, blk.data(), blk.size(), q) && q.rsFixed == 4 && q.payload == payload, "data with four errors");
        for (int i = 0; i < 6; i++) blk[(size_t)(2 + i * 6)] ^= 0x5A;
        Parsed r;
        CHECK(!parseBody(0x4C, blk.data(), blk.size(), r), "too many errors");
        // the 64 byte frame, golden parity
        std::vector<uint8_t> big, bb;
        for (int i = 0; i < 64; i++) big.push_back((uint8_t)(i * 5 + 1));
        CHECK(makeDataBlock(0x4A, big, bb) && bb.size() == 83, "big block");
        CHECK(bb[65] == 0x7A && bb[66] == 0xD6 && bb[67] == 0x1D && bb[68] == 0x32 && bb[82] == 0x67, "golden CRC and RS parity of the 64 byte block %02X", bb[67]);
        Parsed s;
        CHECK(parseBody(0x4A, bb.data(), bb.size(), s) && s.payload == big, "64 byte block");
        // the CRC low byte carries the frame type: the odd frame does not accept the even frame's block
        Parsed u;
        CHECK(!parseBody(0x4B, bb.data(), bb.size(), u), "type is part of the CRC");
        CHECK(!makeDataBlock(0x44, payload, blk) && !makeDataBlock(0x4C, std::vector<uint8_t>(33), blk), "not 4FSK / too long");
        CHECK(bodyLength(0x44) < 0 && bodyLength(0x7A) < 0 && bodyLength(0x2C) == 0 && bodyLength(0x3A) == 3, "body lengths");
    }
    {
        const auto fb = makeFrameBytes(0x4A, 0x5A, {1, 2});
        CHECK(same(fb, {0x4A, 0x4A ^ 0x5A, 1, 2}), "frame bytes");
    }
    return testSummary("ardop proto");
}
