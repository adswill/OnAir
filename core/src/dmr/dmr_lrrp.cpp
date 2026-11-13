// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola LRRP over IP: see dmr_lrrp.h.
//
// The layout confirmed from a real capture (2026-10-23, 161.600 MHz, talkgroup 64250):
//   IPv4 (20 bytes, protocol 17), UDP (8 bytes, ports 4001/4001), then the LRRP document:
//     doc id      uintvar    0x0D = Triggered-Location-Report (NCDT, no constant data table)
//     doc length  uintvar    length of the token stream that follows
//     tokens:
//       0x22 request-id   opaque (uintvar length + that many bytes)
//       0x51 circle-2d    latitude int32 BE (deg * 2^31 / 90), longitude int32 BE (deg * 2^32 / 360),
//                         radius ufloatvar (metres)
//       0x6C speed-hor    ufloatvar, metres per second (km/h = value * 3.6)
//       0x56 direction-hor uint8, degrees = value * 2
//       0x37 result       uintvar code (16 = no fix): the radio heard nothing / no position
//   37-byte packets carry the NoFix form (doc 0x0D + request-id + result 16), 51-byte packets a fix.
#include "dect2/dmr/dmr_lrrp.h"
#include <cmath>

namespace dect2 {
namespace {

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }

// Motorola MBXML varint: 7 bits per byte, the high bit marks a continuation. False on a run past `end`.
bool uintvar(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    out = 0;
    for (int i = 0; i < 5; i++) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        out = (out << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) return true;
    }
    return false;   // more than 5 bytes: not a varint we want to follow
}

// Unsigned float with a variable-length mantissa: integer part + fraction / 128^(bytes of the fraction).
bool ufloatvar(const uint8_t*& p, const uint8_t* end, double& out) {
    uint32_t integer = 0, fraction = 0;
    if (!uintvar(p, end, integer)) return false;
    const uint8_t* f0 = p;
    if (!uintvar(p, end, fraction)) return false;
    double scale = 1.0;
    for (const uint8_t* q = f0; q < p; q++) scale *= 128.0;
    out = (double)integer + (double)fraction / scale;
    return true;
}

// A position token (0x51 with a radius, 0x66 without): 4 bytes latitude, 4 bytes longitude.
bool decodePoint(const uint8_t*& p, const uint8_t* end, DmrLocation& out, bool withRadius) {
    if (p + 8 > end) return false;
    const int32_t la = (int32_t)be32(p);
    const uint32_t lo = be32(p + 4);
    p += 8;
    out.lat = (double)la * 90.0 / 2147483648.0;
    double lon = (double)lo * 360.0 / 4294967296.0;
    if (lon > 180.0) lon -= 360.0;
    out.lon = lon;
    out.hasFix = true;
    if (withRadius) {
        double r = 0;
        if (!ufloatvar(p, end, r)) return false;
        out.hasRadius = true;
        out.radiusM = r;
    }
    return true;
}

} // namespace

// The LRRP document at `body`: id, length, then the tokens within that length. strict: the tokens must fill the document exactly, every one known.
static bool parseDoc(const uint8_t* body, const uint8_t* end, DmrLocation& out, bool strict) {
    uint32_t doc = 0, docLen = 0;
    const uint8_t* p = body;
    if (!uintvar(p, end, doc)) return false;
    if (!uintvar(p, end, docLen)) return false;
    const uint8_t* docEnd = p + docLen;
    if (docEnd > end) { if (strict) return false; docEnd = end; }
    bool hasResult = false;
    while (p < docEnd) {
        uint32_t tok = 0;
        if (!uintvar(p, docEnd, tok)) break;
        if (tok == 0x22) {            // request-id: opaque, skip it
            uint32_t n = 0;
            if (!uintvar(p, docEnd, n)) break;
            if (p + n > docEnd) break;
            p += n;
        } else if (tok == 0x51) {     // circle-2d: latitude, longitude, radius
            if (!decodePoint(p, docEnd, out, true)) break;
        } else if (tok == 0x66) {     // point-2d: latitude, longitude
            if (!decodePoint(p, docEnd, out, false)) break;
        } else if (tok == 0x6C) {     // speed-hor: ufloatvar, metres per second
            double s = 0;
            if (!ufloatvar(p, docEnd, s)) break;
            out.hasSpeed = true;
            out.speedMs = s;
        } else if (tok == 0x56) {     // direction-hor: one byte, degrees = value * 2
            if (p >= docEnd) break;
            out.hasHeading = true;
            out.headingDeg = (double)(*p++) * 2.0;
        } else if (tok == 0x37) {     // result: uintvar code (16 = no fix)
            uint32_t code = 0;
            if (!uintvar(p, docEnd, code)) break;
            out.resultCode = (int)code;
            hasResult = true;
        } else if (tok == 0x34) {     // info-time: 40 bits, year 14, month 4, day 5, hour 5, minute 6, second 6 (UTC)
            if (p + 5 > docEnd) break;
            p += 5;
        } else {
            if (strict) return false;
            break;                    // an unknown token: keep what we already read
        }
    }
    if (strict && p != docEnd) return false;
    return out.hasFix || hasResult;   // a fix or a NoFix status; anything else is not an LRRP report
}

bool dmrIpv4HeaderOk(const uint8_t* data, size_t size) {
    if (size < 20 || (data[0] >> 4) != 4) return false;
    const size_t ihl = (size_t)(data[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > size) return false;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < ihl; i += 2) sum += be16(data + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
}

bool parseDmrLrrp(const uint8_t* data, size_t size, DmrLocation& out) {
    out = DmrLocation();
    if (size < 28) return false;                    // IPv4 (20 bytes) + UDP (8 bytes)
    if ((data[0] >> 4) != 4) return false;          // IPv4
    const size_t ihl = (size_t)(data[0] & 0x0F) * 4;
    if (ihl < 20 || ihl + 8 > size) return false;
    if (data[9] != 17) return false;                // UDP
    const size_t total = be16(data + 2);
    if (total < ihl + 8 || total > size) return false;
    const size_t udpLen = be16(data + ihl + 4);
    if (udpLen < 8 || ihl + udpLen > total) return false;
    const uint8_t* body = data + ihl + 8;
    return parseDoc(body, body + (udpLen - 8), out, false);
}

// ---- encoder (dmr_lrrp.h): the inverse of the parser above, byte for byte
namespace {
void putUintvar(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = (uint8_t)(x & 0x7F);
    while ((x >>= 7)) tmp[n++] = (uint8_t)((x & 0x7F) | 0x80);
    for (int i = n - 1; i >= 0; i--) v.push_back(tmp[i]);   // most significant group first, continuation bit on all but the last
}
void putUfloatvar(std::vector<uint8_t>& v, double x) {
    if (x < 0) x = 0;
    const uint32_t integer = (uint32_t)x;
    const uint32_t fraction = (uint32_t)((x - (double)integer) * 128.0 + 0.5);   // one fraction octet: scale 128
    putUintvar(v, integer);
    putUintvar(v, fraction & 0x7F);
}
void putBe32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

std::vector<uint8_t> wrapIpUdp(const std::vector<uint8_t>& doc) {
    std::vector<uint8_t> pkt;
    const size_t total = 20 + 8 + doc.size();
    // IPv4 header
    pkt.push_back(0x45); pkt.push_back(0x00);
    pkt.push_back((uint8_t)(total >> 8)); pkt.push_back((uint8_t)total);
    pkt.push_back(0x00); pkt.push_back(0x00);     // id
    pkt.push_back(0x00); pkt.push_back(0x00);     // flags / fragment
    pkt.push_back(0x40);                          // TTL 64
    pkt.push_back(17);                            // protocol UDP
    pkt.push_back(0x00); pkt.push_back(0x00);     // header checksum (filled below)
    pkt.push_back(10); pkt.push_back(0); pkt.push_back(0); pkt.push_back(1);   // src 10.0.0.1
    pkt.push_back(10); pkt.push_back(0); pkt.push_back(0); pkt.push_back(2);   // dst 10.0.0.2
    uint32_t sum = 0;
    for (size_t i = 0; i < 20; i += 2) sum += (uint32_t)((pkt[i] << 8) | pkt[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    const uint16_t hc = (uint16_t)(~sum);
    pkt[10] = (uint8_t)(hc >> 8); pkt[11] = (uint8_t)hc;
    // UDP header (checksum 0: optional for IPv4)
    const size_t udpLen = 8 + doc.size();
    pkt.push_back(0x0F); pkt.push_back(0xA1);     // src port 4001
    pkt.push_back(0x0F); pkt.push_back(0xA1);     // dst port 4001
    pkt.push_back((uint8_t)(udpLen >> 8)); pkt.push_back((uint8_t)udpLen);
    pkt.push_back(0x00); pkt.push_back(0x00);     // checksum
    pkt.insert(pkt.end(), doc.begin(), doc.end());
    return pkt;
}
} // namespace

std::vector<uint8_t> dmrLrrpEncode(double lat, double lon, double radiusM, bool hasSpeed, double speedMs, bool hasHeading, double headingDeg, uint16_t requestId) {
    std::vector<uint8_t> tok;
    tok.push_back(0x22); putUintvar(tok, 2); tok.push_back((uint8_t)(requestId >> 8)); tok.push_back((uint8_t)requestId);   // request-id, two octets
    tok.push_back(0x51);                                                           // circle-2d: lat, lon, radius
    const double latN = lat > 90 ? 90 : lat < -90 ? -90 : lat;
    double lonN = lon;
    while (lonN >= 180.0) lonN -= 360.0;
    while (lonN < -180.0) lonN += 360.0;
    putBe32(tok, (uint32_t)(int32_t)std::llround(latN * 2147483648.0 / 90.0));
    putBe32(tok, (uint32_t)std::llround((lonN < 0 ? lonN + 360.0 : lonN) * 4294967296.0 / 360.0));
    putUfloatvar(tok, radiusM);
    if (hasSpeed) { tok.push_back(0x6C); putUfloatvar(tok, speedMs); }
    if (hasHeading) { tok.push_back(0x56); tok.push_back((uint8_t)std::lround(headingDeg / 2.0) & 0xFF); }
    std::vector<uint8_t> doc;
    putUintvar(doc, 0x0D);                 // Triggered-Location-Report
    putUintvar(doc, (uint32_t)tok.size());
    doc.insert(doc.end(), tok.begin(), tok.end());
    return wrapIpUdp(doc);
}

std::vector<uint8_t> dmrLrrpEncodeNoFix(uint16_t requestId) {
    std::vector<uint8_t> tok;
    tok.push_back(0x22); putUintvar(tok, 2); tok.push_back((uint8_t)(requestId >> 8)); tok.push_back((uint8_t)requestId);
    tok.push_back(0x37); putUintvar(tok, 16);     // result: no fix
    std::vector<uint8_t> doc;
    putUintvar(doc, 0x0D);
    putUintvar(doc, (uint32_t)tok.size());
    doc.insert(doc.end(), tok.begin(), tok.end());
    return wrapIpUdp(doc);
}

bool parseDmrLrrpCompressed(const uint8_t* data, size_t size, DmrLocation& out) {
    out = DmrLocation();
    // the document starts after a compressed IP/UDP header of a few octets: it is taken only where its length ends exactly at the end of
    // the data and every token in it is known, so that a chance match in other data is not taken for a position
    for (size_t off = 0; off <= 8 && off + 4 <= size; off++) {
        const uint8_t* p = data + off;
        if (p[0] & 0x80u || p[1] & 0x80u) continue;          // one-byte document id and length
        if (off + 2 + (size_t)p[1] != size) continue;
        DmrLocation loc;
        if (parseDoc(p, data + size, loc, true)) { out = loc; return true; }
    }
    return false;
}

} // namespace dect2
