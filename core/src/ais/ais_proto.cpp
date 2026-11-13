// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The generator side of the AIS frame layer (putU, putS, putText, appendFcs, burstLineBits, toNmea); the reading side (bit fields, HDLC, NMEA, message decoding) is Rust:
// ais_proto_rs.cpp over rust/onair_proto.
#include "dect2/ais/ais_proto.h"
#include "dect2/ais/ais_tel.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "dect2/common/linecode.h"

namespace dect2 {
namespace ais {


// ---- bit fields
void putU(Bits& b, size_t pos, size_t len, uint32_t v) {
    if (b.size() < pos + len) b.resize(pos + len, 0);
    for (size_t i = 0; i < len; i++) b[pos + i] = len - 1 - i < 32 ? (v >> (len - 1 - i)) & 1u : 0u;   // fields wider than 32 bits are zero above
}
void putS(Bits& b, size_t pos, size_t len, int32_t v) {
    putU(b, pos, len, len >= 32 ? (uint32_t)v : ((uint32_t)v & ((1u << len) - 1u)));
}
void putText(Bits& b, size_t pos, size_t len, const std::string& s) {
    for (size_t i = 0; i + 6 <= len; i += 6) {
        char c = i / 6 < s.size() ? s[i / 6] : '@';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        uint32_t v = (c >= 64 && c < 96) ? (uint32_t)(c - 64) : (c >= 32 && c < 64) ? (uint32_t)c : 0u;   // anything else becomes '@'
        putU(b, pos + i, 6, v);
    }
}

// ---- frame
void appendFcs(Bits& payload) {
    const uint16_t crc = (uint16_t)~fcsRegister(payload.data(), payload.size());
    for (int i = 0; i < 16; i++) payload.push_back((crc >> i) & 1);
}
static const uint8_t kFlag[8] = {0, 1, 1, 1, 1, 1, 1, 0};

Bits burstLineBits(const Bits& payload) {
    Bits fr = payload;
    reverseOctets(fr);           // the order on the air (#87); the FCS is computed over the bits as sent
    appendFcs(fr);
    Bits d;
    d.insert(d.end(), kFlag, kFlag + 8);
    const Bits s = stuff(fr);
    d.insert(d.end(), s.begin(), s.end());
    d.insert(d.end(), kFlag, kFlag + 8);
    for (int i = 0; i < 8; i++) d.push_back(0);                           // the carrier is still on while the transmitter ramps down (the rest of the 24 bit buffer is silence)
    // The training sequence alternates on the line (0101..., the level it modulates; NRZI makes it start with either value, here 0 so it ends on 1);
    // the NRZI coding of the flags and the frame starts from the level it leaves.
    Bits line;
    for (int i = 0; i < 24; i++) line.push_back((uint8_t)(i & 1));
    const Bits rest = nrziEncode(d, 1);
    line.insert(line.end(), rest.begin(), rest.end());
    return line;
}

// ---- messages
const char* shipTypeText(int t) { return aisShipTypeText(t); }

// ---- NMEA 0183
std::vector<std::string> toNmea(const Bits& payload, char ch, int seqId) {
    int fill = 0;
    const std::string all = armour(payload, fill);
    const size_t per = 60;     // payload characters per sentence, as the fragments in the gpsd examples
    const size_t cnt = all.empty() ? 1 : (all.size() + per - 1) / per;
    std::vector<std::string> out;
    for (size_t i = 0; i < cnt; i++) {
        const std::string part = all.substr(i * per, per);
        const int f = i + 1 == cnt ? fill : 0;
        char body[160];
        snprintf(body, sizeof body, "AIVDM,%zu,%zu,%s,%c,%s,%d", cnt, i + 1, cnt > 1 ? std::to_string(seqId % 10).c_str() : "", ch, part.c_str(), f);
        char tail[8];
        snprintf(tail, sizeof tail, "*%02X", nmeaChecksum(body));
        out.push_back(std::string("!") + body + tail);
    }
    return out;
}

} // namespace ais
} // namespace dect2
