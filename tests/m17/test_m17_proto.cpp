// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 protocol layers against the specification: CRC test vectors, base-40 callsigns, Golay(24,12) corrections, the interleaver, the
// frame builders and their soft decoders (with bit errors), GNSS and text META.
#include "dect2/m17/m17_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

// symbols of a frame (payload part) to soft bits, with nflip symbol errors
static void toSoft(uint16_t sync, const uint8_t bits[368], float soft[368], int nflip, std::mt19937& rng) {
    int8_t sym[192];
    m17::frameSymbols(sync, bits, sym);
    float z[184];
    for (int i = 0; i < 184; i++) z[i] = sym[8 + i];
    for (int k = 0; k < nflip; k++) { const int i = (int)(rng() % 184); z[i] = z[i] > 0 ? z[i] - 2 : z[i] + 2; }   // to the neighbouring level
    m17::softBits(z, soft);
}

int main() {
    // CRC (spec table)
    CHECK(m17::crc16(nullptr, 0) == 0xFFFF, "crc empty");
    CHECK(m17::crc16((const uint8_t*)"A", 1) == 0x206E, "crc A %04X", m17::crc16((const uint8_t*)"A", 1));
    CHECK(m17::crc16((const uint8_t*)"123456789", 9) == 0x772B, "crc 123456789");
    uint8_t all[256]; for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    CHECK(m17::crc16(all, 256) == 0x1C31, "crc 00..FF %04X", m17::crc16(all, 256));
    // callsigns (spec example: AB1CD = 0x9fdd51)
    CHECK(m17::encodeCallsign("AB1CD") == 0x9fdd51, "AB1CD");
    for (const char* c : {"N0CALL", "M17-M17 C", "AB1CD/P", "SP5WWP", "ECHO"}) CHECK(m17::decodeCallsign(m17::encodeCallsign(c)) == c, "round trip %s", c);
    CHECK(m17::decodeCallsign(m17::kBroadcast) == "@ALL" && m17::encodeCallsign("@ALL") == m17::kBroadcast, "broadcast");
    CHECK(m17::encodeCallsign(".........") == 0xEE6B27FFFFFFull, "top of the base-40 range");
    // Golay: every word with up to 3 errors comes back, 4 errors are never taken for a wrong word silently in most cases
    std::mt19937 rng(5);
    int bad = 0, det4 = 0;
    for (int n = 0; n < 4000; n++) {
        const uint16_t d = (uint16_t)(rng() & 0xFFF);
        const uint32_t cw = m17::golayEncode(d);
        uint32_t e = 0; const int w = n % 4;
        while (__builtin_popcount(e) < w) e |= 1u << (rng() % 24);
        uint16_t o; int er;
        if (!m17::golayDecode(cw ^ e, o, er) || o != d || er != w) bad++;
        uint32_t e4 = 0; while (__builtin_popcount(e4) < 4) e4 |= 1u << (rng() % 24);
        if (!m17::golayDecode(cw ^ e4, o, er)) det4++;
    }
    CHECK(bad == 0, "%d golay failures", bad);
    CHECK(det4 == 4000, "4 errors detected %d / 4000", det4);   // the extended code has distance 8: 4 errors are always seen
    CHECK(m17::golayEncode(0x800) == (0x800000u | 0xC75), "golay row of the top data bit");
    // LSF loopback with errors
    m17::Lsf l; l.src = m17::encodeCallsign("N0CALL"); l.dst = m17::kBroadcast; l.type = m17::makeType(true, 2, 0, 1, 7);
    m17::Gnss g; g.posValid = g.altValid = g.velValid = true; g.lat = 48.2085; g.lon = -16.3725; g.altM = 180.5; g.speedKmh = 36.5; g.bearing = 271; g.station = 2; g.source = 1; g.radius = 3;
    m17::gnssPack(g, l.meta);
    uint8_t lsf[30], bits[368], back[30];
    m17::lsfPack(l, lsf);
    CHECK(m17::crc16(lsf, 30) == 0, "lsf crc over all is 0");
    m17::buildLsf(lsf, bits);
    float soft[368];
    int corr;
    for (int nf : {0, 2}) {
        toSoft(m17::kSyncLsf, bits, soft, nf, rng);
        const bool ok = m17::decodeLsf(soft, back, corr);
        CHECK(ok && !memcmp(back, lsf, 30), "lsf with %d symbol errors (corrected %d)", nf, corr);
    }
    m17::Lsf l2;
    CHECK(m17::lsfUnpack(back, l2) && m17::typeCan(l2.type) == 7 && m17::typeData(l2.type) == 2 && m17::typeEncSub(l2.type) == 1, "type fields");
    const m17::Gnss g2 = m17::gnssUnpack(l2.meta);
    CHECK(std::fabs(g2.lat - g.lat) < 2e-5 && std::fabs(g2.lon - g.lon) < 3e-5 && std::fabs(g2.altM - 180.5) < 0.01 && std::fabs(g2.speedKmh - 36.5) < 0.01 &&
          g2.bearing == 271 && g2.station == 2 && g2.source == 1 && g2.radius == 3 && g2.posValid && g2.altValid && g2.velValid, "gnss round trip %.6f %.6f", g2.lat, g2.lon);
    CHECK(m17::typeText(m17::makeType(true, 3, 0, 0, 0)) == "voice + data 1600" && m17::encText(m17::makeType(true, 2, 2, 2, 0)) == "AES-256" &&
          m17::encText(m17::makeType(true, 2, 1, 1, 0)) == "scrambler 16-bit", "type texts");
    // stream frames: LICH and payload
    uint8_t pl[16], pl2[16], lich[6];
    for (int i = 0; i < 16; i++) pl[i] = (uint8_t)(i * 29 + 3);
    for (int cnt = 0; cnt < 6; cnt++) {
        m17::buildStream(lsf, cnt, (uint16_t)(cnt == 5 ? 0x8005 : cnt), pl, bits);
        toSoft(m17::kSyncStream, bits, soft, cnt * 2, rng);
        int ge; uint16_t fn;
        const bool ok = m17::decodeStream(soft, lich, ge, fn, pl2, corr);
        CHECK(ok && !memcmp(lich, lsf + 5 * cnt, 5) && (lich[5] >> 5) == cnt && fn == (cnt == 5 ? 0x8005 : cnt) && !memcmp(pl, pl2, 16), "stream frame %d", cnt);
    }
    // packet frames
    uint8_t ch[25], ch2[25];
    for (int i = 0; i < 25; i++) ch[i] = (uint8_t)(200 - i);
    m17::buildPacket(ch, true, 17, bits);
    toSoft(m17::kSyncPacket, bits, soft, 8, rng);
    bool eof; int cnt;
    m17::decodePacket(soft, ch2, eof, cnt, corr);
    CHECK(eof && cnt == 17 && !memcmp(ch, ch2, 25), "packet frame");
    // text META
    uint8_t meta[14];
    m17::textBlockPack("Hello M17 world, 73 from OnAir", 1, meta);
    CHECK(meta[0] == 0x72 && meta[1] == 'l' && meta[2] == 'd', "text block control %02X", meta[0]);
    // sync bursts (spec table)
    int8_t s[8];
    m17::syncSymbols(m17::kSyncLsf, s);
    CHECK(s[0] == 3 && s[4] == -3 && s[6] == 3 && s[7] == -3, "LSF sync symbols");
    m17::syncSymbols(m17::kSyncPacket, s);
    CHECK(s[0] == 3 && s[1] == -3 && s[2] == 3 && s[7] == -3, "packet sync symbols");
    printf(fails ? "m17 proto: %d FAILED\n" : "m17 proto: all passed\n", fails);
    return fails ? 1 : 0;
}
