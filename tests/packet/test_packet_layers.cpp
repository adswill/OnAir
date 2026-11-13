// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / packet layers without the radio: "fix bits" (destuffing, the repair of one wrong line bit at 1200 baud with NRZI and at 9600 baud
// through the G3RUH scrambler, the plausibility check of a repaired frame) and aprs::parseFull() (a '!' position after other text, raw NMEA,
// third-party packets, the reply-ack message number).
#include "dect2/packet/packet_aprs.h"
#include "dect2/packet/packet_ax25.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

namespace {

ax25::Frame sample() {
    ax25::Frame f;
    f.from.call = "N0CALL"; f.from.ssid = 9; f.to.call = "APRS";
    ax25::Address d; d.call = "WIDE1"; d.ssid = 1; f.path.push_back(d);
    f.info = "!4903.50N/07201.75W-fix bits test frame";
    return f;
}

// the bits a receiver sees between the flags (NRZI undone, still stuffed), after the line levels went through the channel: flip = the line
// bits to invert, scrambled = 9600 baud G3RUH
std::vector<uint8_t> received(const std::vector<int>& flip, bool scrambled) {
    const std::vector<uint8_t> frame = ax25::buildFrame(sample());
    const std::vector<uint8_t> bits = ax25::hdlcBits(frame, 4, 2);
    std::vector<uint8_t> lv = ax25::nrziEncode(bits);
    uint32_t st = 0;
    if (scrambled) lv = ax25::scramble(lv, st);
    for (int k : flip) lv[(size_t)k] ^= 1;
    // receiver: descramble, NRZI, cut between the 4th flag and the closing flag
    std::vector<uint8_t> d(lv.size());
    uint32_t sr = 0;
    int prev = 0;
    for (size_t i = 0; i < lv.size(); i++) {
        uint32_t v = lv[i];
        if (scrambled) { const uint32_t o = v ^ ((sr >> 11) & 1u) ^ ((sr >> 16) & 1u); sr = ((sr << 1) | v) & 0x1FFFFu; v = o; }
        d[i] = (uint8_t)((int)v == prev ? 1 : 0);
        prev = (int)v;
    }
    return std::vector<uint8_t>(d.begin() + 32, d.end() - 16);
}

} // namespace

int main() {
    std::vector<uint8_t> out;
    const std::vector<uint8_t> want = [] { std::vector<uint8_t> b = ax25::buildFrame(sample()); b.resize(b.size() - 2); return b; }();
    CHECK(ax25::frameFromStuffedBits(received({}, false), out) && out == want, "clean 1200 frame destuffs");
    CHECK(ax25::plausibleFrame(want), "a real APRS frame is plausible");
    for (bool scr : {false, true}) {
        for (int at : {40, 200, 333}) {
            const std::vector<uint8_t> bad = received({at + (scr ? 0 : 32)}, scr);
            CHECK(!ax25::frameFromStuffedBits(bad, out), "%s: one wrong line bit breaks the FCS", scr ? "9600" : "1200");
            CHECK(ax25::repairFrame(bad, ax25::fixPatterns(0, scr), out, ax25::plausibleFrame) == 0, "fix bits off: no repair");
            const int k = ax25::repairFrame(bad, ax25::fixPatterns(1, scr), out, ax25::plausibleFrame);
            CHECK(k > 0 && out == want, "%s: one wrong line bit at %d repaired (pattern %d)", scr ? "9600" : "1200", at, k);
        }
        const std::vector<uint8_t> two = received({150 + (scr ? 0 : 32), 151 + (scr ? 0 : 32)}, scr);
        CHECK(ax25::repairFrame(two, ax25::fixPatterns(1, scr), out, ax25::plausibleFrame) == 0 || out == want, "level 1 never returns a wrong frame");
        const int k2 = ax25::repairFrame(two, ax25::fixPatterns(2, scr), out, ax25::plausibleFrame);
        CHECK(k2 > 0 && out == want, "%s: two wrong line bits in a row repaired at level 2", scr ? "9600" : "1200");
    }
    {   // garbage that passes the FCS is not plausible: lower-case address, a control byte, a non-UI frame
        std::vector<uint8_t> g = want;
        g[0] = (uint8_t)('a' << 1);
        CHECK(!ax25::plausibleFrame(g), "lower-case address rejected");
        g = want; g.back() = 0x01;
        CHECK(!ax25::plausibleFrame(g), "control character in the text rejected");
        g = want; g[7 * 3] = 0x3F;
        CHECK(!ax25::plausibleFrame(g), "a SABM is not repaired");
    }
    aprs::Info a;
    std::string inner;
    CHECK(aprs::parseFull("APRS", "TheNet X1J4 (BFLD)!4903.50N/07201.75W-", a) && a.hasPos && std::fabs(a.lat - 49.0583) < 1e-3 && a.comment.find("TheNet") == 0,
          "'!' position after other text");
    CHECK(aprs::parseFull("APRS", "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", a) && a.hasPos && std::fabs(a.lat - 48.1173) < 1e-3 &&
          std::fabs(a.lon - 11.5167) < 1e-3 && a.hasSpeed && std::fabs(a.speedKnots - 22.4) < 1e-6, "raw NMEA RMC position");
    CHECK(aprs::parseFull("APRS", "$GPGGA,123519,4807.038,S,01131.000,W,1,08,0.9,545.4,M,46.9,M,,*47", a) && a.hasPos && a.lat < 0 && a.lon < 0 && a.hasAlt,
          "raw NMEA GGA position");
    CHECK(aprs::parseFull("APRS", "$GPRMC,123519,V,4807.038,N,01131.000,E,,,230394,,*6A", a) && !a.hasPos, "RMC without a fix has no position");
    CHECK(aprs::parseFull("APRS", "}DL1ABC-7>APRS,TCPIP,WIDE1*:=4903.50N/07201.75W-inner station", a, &inner) && inner == "DL1ABC-7" && a.hasPos &&
          a.comment == "inner station", "third-party packet: the inner station and its position");
    CHECK(aprs::parseFull("APRS", ":WU2Z     :Testing{AB}CD", a) && a.msgNo == "AB" && a.text == "Testing", "reply-ack message number");
    return testSummary("packet_layers");
}
