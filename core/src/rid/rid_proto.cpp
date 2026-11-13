// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID messages (ASTM F3411-22a / ASD-STAN EN 4709-002): decoding, encoding (the test signal) and the BLE / Wi-Fi carriers.
#include "dect2/rid/rid_proto.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include "dect2/common/endian.h"

namespace dect2::rid {

namespace {

uint16_t get16(const uint8_t* p) { return (uint16_t)dect2::endian::loadLe16(p); }
uint32_t get32(const uint8_t* p) { return (uint32_t)dect2::endian::loadLe32(p); }
void put16(uint8_t* p, unsigned v) { dect2::endian::storeLe16(p, v); }
void put32(uint8_t* p, uint32_t v) { dect2::endian::storeLe32(p, v); }

double altDec(uint16_t v) { return v * 0.5 - 1000; }
uint16_t altEnc(double a) {
    if (a <= -1000) return 0;
    const double v = std::round((a + 1000) / 0.5);
    return (uint16_t)std::max(0.0, std::min(65535.0, v));
}
int32_t degEnc(double d) { return (int32_t)std::llround(d * 1e7); }

std::string text(const uint8_t* p, size_t n) {   // a null-padded ASCII field, printable characters only
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
void putText(uint8_t* p, size_t n, const std::string& s) { for (size_t i = 0; i < n; i++) p[i] = i < s.size() ? (uint8_t)s[i] : 0; }

} // namespace

bool decodeMessage(const uint8_t* m, Message& o) {
    o = Message();
    o.type = m[0] >> 4;
    o.version = m[0] & 15;
    switch (o.type) {
    case BasicId:
        o.idType = m[1] >> 4; o.uaType = m[1] & 15;
        if (o.idType == 3) {   // UTM assigned UUID: 16 binary bytes
            char b[4];
            for (int i = 0; i < 16; i++) { snprintf(b, sizeof b, "%02x", m[2 + i]); o.id += b; if (i == 3 || i == 5 || i == 7 || i == 9) o.id += '-'; }
        } else o.id = text(m + 2, 20);
        return true;
    case Location: {
        o.status = m[1] >> 4; o.heightType = (m[1] >> 2) & 1;
        const bool ew = (m[1] >> 1) & 1, mult = m[1] & 1;
        const int dir = m[2] + (ew ? 180 : 0);
        o.direction = dir <= 360 ? dir % 360 : -1;   // 361: unknown
        if (mult == 0) o.speed = m[3] * 0.25;
        else o.speed = m[3] == 255 ? -1 : m[3] * 0.75 + 255 * 0.25;
        const int8_t vs = (int8_t)m[4];
        o.vspeedValid = vs != 126;   // 63 m/s: unknown
        o.vspeed = o.vspeedValid ? vs * 0.5 : 0;
        const int32_t la = (int32_t)get32(m + 5), lo = (int32_t)get32(m + 9);
        o.lat = la * 1e-7; o.lon = lo * 1e-7;
        o.posValid = (la != 0 || lo != 0) && std::fabs(o.lat) <= 90 && std::fabs(o.lon) <= 180;
        o.altBaro = altDec(get16(m + 13)); o.altGeo = altDec(get16(m + 15)); o.height = altDec(get16(m + 17));
        o.horizAcc = m[19] & 15; o.vertAcc = m[19] >> 4;
        o.speedAcc = m[20] & 15; o.baroAcc = m[20] >> 4;
        const uint16_t ts = get16(m + 21);
        o.timestamp = ts == 0xFFFF || ts > 36000 ? -1 : ts;
        o.tsAcc = m[23] & 15;
        return true;
    }
    case Auth:
        o.authType = m[1] >> 4; o.page = m[1] & 15;
        if (o.page == 0) {
            o.lastPage = m[2] & 15; o.authLength = m[3]; o.authTime = get32(m + 4);
            o.authData.assign(m + 8, m + 25);
        } else o.authData.assign(m + 2, m + 25);
        return true;
    case SelfId:
        o.descType = m[1];
        o.text = text(m + 2, 23);
        return true;
    case System: {
        o.opLocType = m[1] & 3; o.classType = (m[1] >> 2) & 7;
        const int32_t la = (int32_t)get32(m + 2), lo = (int32_t)get32(m + 6);
        o.opLat = la * 1e-7; o.opLon = lo * 1e-7;
        o.opValid = (la != 0 || lo != 0) && std::fabs(o.opLat) <= 90 && std::fabs(o.opLon) <= 180;
        o.areaCount = get16(m + 10); o.areaRadius = m[12] * 10;
        o.areaCeiling = altDec(get16(m + 13)); o.areaFloor = altDec(get16(m + 15));
        o.category = m[17] >> 4; o.uaClass = m[17] & 15;
        o.opAlt = altDec(get16(m + 18));
        o.sysTime = get32(m + 20);
        return true;
    }
    case OperatorId:
        o.opIdType = m[1];
        o.text = text(m + 2, 20);
        return true;
    default:
        return false;
    }
}

int decodeBlock(const uint8_t* p, size_t n, std::vector<Message>& out) {
    if (n < (size_t)kMsgSize) return -1;
    if ((p[0] >> 4) != Pack) {
        Message m;
        if (!decodeMessage(p, m)) return -1;
        out.push_back(std::move(m));
        return 1;
    }
    if (n < 3 || p[1] != kMsgSize) return -1;
    const int cnt = p[2];
    if (cnt < 1 || cnt > 10 || n < 3 + (size_t)cnt * kMsgSize) return -1;
    int got = 0;
    for (int i = 0; i < cnt; i++) {
        Message m;
        const uint8_t* q = p + 3 + i * kMsgSize;
        if ((q[0] >> 4) == Pack) continue;   // no packs inside packs
        if (decodeMessage(q, m)) { out.push_back(std::move(m)); got++; }
    }
    return got;
}

void encodeMessage(const Message& m, uint8_t o[kMsgSize]) {
    memset(o, 0, kMsgSize);
    o[0] = (uint8_t)((m.type & 15) << 4 | (m.version & 15));
    switch (m.type) {
    case BasicId:
        o[1] = (uint8_t)((m.idType & 15) << 4 | (m.uaType & 15));
        putText(o + 2, 20, m.id);
        break;
    case Location: {
        int dir = m.direction < 0 ? 361 : (int)std::lround(m.direction) % 360;
        const bool ew = dir >= 180;
        uint8_t mult = 0, sp;
        if (m.speed < 0) { mult = 1; sp = 255; }
        else if (m.speed <= 255 * 0.25) sp = (uint8_t)std::lround(m.speed / 0.25);
        else { mult = 1; sp = (uint8_t)std::min(254L, std::lround((m.speed - 255 * 0.25) / 0.75)); }
        o[1] = (uint8_t)((m.status & 15) << 4 | (m.heightType & 1) << 2 | (ew ? 2 : 0) | mult);
        o[2] = (uint8_t)(ew ? dir - 180 : dir);
        o[3] = sp;
        o[4] = (uint8_t)(int8_t)(m.vspeedValid ? std::max(-62.0, std::min(62.0, m.vspeed)) / 0.5 : 126);
        put32(o + 5, (uint32_t)(m.posValid ? degEnc(m.lat) : 0));
        put32(o + 9, (uint32_t)(m.posValid ? degEnc(m.lon) : 0));
        put16(o + 13, altEnc(m.altBaro)); put16(o + 15, altEnc(m.altGeo)); put16(o + 17, altEnc(m.height));
        o[19] = (uint8_t)((m.vertAcc & 15) << 4 | (m.horizAcc & 15));
        o[20] = (uint8_t)((m.baroAcc & 15) << 4 | (m.speedAcc & 15));
        put16(o + 21, m.timestamp < 0 ? 0xFFFF : (unsigned)m.timestamp);
        o[23] = (uint8_t)(m.tsAcc & 15);
        break;
    }
    case Auth:
        o[1] = (uint8_t)((m.authType & 15) << 4 | (m.page & 15));
        if (m.page == 0) {
            o[2] = (uint8_t)m.lastPage; o[3] = (uint8_t)m.authLength; put32(o + 4, m.authTime);
            for (size_t i = 0; i < 17 && i < m.authData.size(); i++) o[8 + i] = m.authData[i];
        } else for (size_t i = 0; i < 23 && i < m.authData.size(); i++) o[2 + i] = m.authData[i];
        break;
    case SelfId:
        o[1] = (uint8_t)m.descType;
        putText(o + 2, 23, m.text);
        break;
    case System:
        o[1] = (uint8_t)((m.classType & 7) << 2 | (m.opLocType & 3));
        put32(o + 2, (uint32_t)(m.opValid ? degEnc(m.opLat) : 0));
        put32(o + 6, (uint32_t)(m.opValid ? degEnc(m.opLon) : 0));
        put16(o + 10, (unsigned)m.areaCount); o[12] = (uint8_t)std::min(255, m.areaRadius / 10);
        put16(o + 13, altEnc(m.areaCeiling)); put16(o + 15, altEnc(m.areaFloor));
        o[17] = (uint8_t)((m.category & 15) << 4 | (m.uaClass & 15));
        put16(o + 18, altEnc(m.opAlt));
        put32(o + 20, m.sysTime);
        break;
    case OperatorId:
        o[1] = (uint8_t)m.opIdType;
        putText(o + 2, 20, m.text);
        break;
    default: break;
    }
}

std::vector<uint8_t> encodePack(const std::vector<Message>& msgs) {
    std::vector<uint8_t> v = {(uint8_t)(Pack << 4 | 2), (uint8_t)kMsgSize, (uint8_t)msgs.size()};
    for (const auto& m : msgs) { uint8_t b[kMsgSize]; encodeMessage(m, b); v.insert(v.end(), b, b + kMsgSize); }
    return v;
}

bool fromBleAdvData(const uint8_t* ad, size_t n, int& counter, std::vector<Message>& out) {
    bool any = false;
    for (size_t i = 0; i + 1 < n;) {
        const size_t len = ad[i];
        if (len == 0 || i + 1 + len > n) break;
        const uint8_t* d = ad + i + 1;
        // service data, 16-bit UUID: 0x16, UUID (LE), app code, counter, the message (or, in an extended packet, a pack)
        if (d[0] == 0x16 && len >= 5 + (size_t)kMsgSize && get16(d + 1) == kBleUuid && d[3] == kAppCode) {
            counter = d[4];
            if (decodeBlock(d + 5, len - 5, out) > 0) any = true;
        }
        i += 1 + len;
    }
    return any;
}

bool fromBeacon(const uint8_t* f, size_t n, int& counter, std::vector<Message>& out) {
    if (n < 36) return false;
    const int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type != 0 || (sub != 8 && sub != 5)) return false;   // beacon or probe response
    bool any = false;
    for (size_t i = 36; i + 2 <= n;) {
        const size_t id = f[i], len = f[i + 1];
        if (i + 2 + len > n) break;
        const uint8_t* d = f + i + 2;
        if (id == 221 && len >= 5 + 3 && d[0] == kOui[0] && d[1] == kOui[1] && d[2] == kOui[2] && d[3] == kAppCode) {
            counter = d[4];
            if (decodeBlock(d + 5, len - 5, out) > 0) any = true;
        }
        i += 2 + len;
    }
    return any;
}

std::vector<uint8_t> bleServiceData(int counter, const Message& m) {
    std::vector<uint8_t> v = {(uint8_t)(4 + 1 + kMsgSize), 0x16, (uint8_t)(kBleUuid & 0xFF), (uint8_t)(kBleUuid >> 8), kAppCode, (uint8_t)counter};
    uint8_t b[kMsgSize]; encodeMessage(m, b);
    v.insert(v.end(), b, b + kMsgSize);
    return v;
}

std::vector<uint8_t> beaconElement(int counter, const std::vector<Message>& msgs) {
    const std::vector<uint8_t> pack = encodePack(msgs);
    std::vector<uint8_t> v = {221, (uint8_t)(5 + pack.size()), kOui[0], kOui[1], kOui[2], kAppCode, (uint8_t)counter};
    v.insert(v.end(), pack.begin(), pack.end());
    return v;
}

const char* msgTypeName(int t) {
    static const char* n[] = {"Basic ID", "Location", "Authentication", "Self-ID", "System", "Operator ID"};
    if (t >= 0 && t <= 5) return n[t];
    return t == Pack ? "Message pack" : "reserved";
}
const char* uaTypeName(int t) {
    static const char* n[] = {"not declared", "aeroplane", "multirotor / helicopter", "gyroplane", "hybrid lift (VTOL)", "ornithopter", "glider", "kite",
                              "free balloon", "captive balloon", "airship", "parachute", "rocket", "tethered powered", "ground obstacle", "other"};
    return t >= 0 && t < 16 ? n[t] : "?";
}
const char* idTypeName(int t) {
    static const char* n[] = {"none", "serial number", "CAA registration", "UTM UUID", "session ID"};
    return t >= 0 && t < 5 ? n[t] : "reserved";
}
const char* statusName(int s) {
    static const char* n[] = {"undeclared", "on the ground", "airborne", "emergency", "RID failure"};
    return s >= 0 && s < 5 ? n[s] : "reserved";
}
const char* opLocTypeName(int t) {
    static const char* n[] = {"take-off", "live GNSS", "fixed"};
    return t >= 0 && t < 3 ? n[t] : "reserved";
}
const char* authTypeName(int t) {
    static const char* n[] = {"none", "UAS ID signature", "operator ID signature", "message set signature", "network remote ID", "specific", "reserved", "reserved"};
    if (t >= 0 && t < 8) return n[t];
    return t >= 10 ? "private" : "reserved";
}
const char* categoryName(int c) {
    static const char* n[] = {"undefined", "open", "specific", "certified"};
    return c >= 0 && c < 4 ? n[c] : "reserved";
}
std::string className(int c) {
    if (c >= 1 && c <= 7) return "C" + std::to_string(c - 1);
    return "-";
}
std::string horizAccText(int a) {
    static const char* n[] = {"unknown", "< 18.5 km", "< 7.4 km", "< 3.7 km", "< 1.9 km", "< 926 m", "< 556 m", "< 185 m", "< 93 m", "< 30 m", "< 10 m", "< 3 m", "< 1 m"};
    return a >= 0 && a < 13 ? n[a] : "reserved";
}
std::string vertAccText(int a) {
    static const char* n[] = {"unknown", "< 150 m", "< 45 m", "< 25 m", "< 10 m", "< 3 m", "< 1 m"};
    return a >= 0 && a < 7 ? n[a] : "reserved";
}
std::string speedAccText(int a) {
    static const char* n[] = {"unknown", "< 10 m/s", "< 3 m/s", "< 1 m/s", "< 0.3 m/s"};
    return a >= 0 && a < 5 ? n[a] : "reserved";
}

std::string summary(const Message& m) {
    char b[200];
    switch (m.type) {
    case BasicId: snprintf(b, sizeof b, "Basic ID: %s %s, %s", idTypeName(m.idType), m.id.c_str(), uaTypeName(m.uaType)); break;
    case Location:
        if (m.posValid)
            snprintf(b, sizeof b, "Location: %.6f %.6f, %.0f m geo, height %.0f m, %.1f m/s, %s, %s", m.lat, m.lon, m.altGeo, m.height, std::max(0.0, m.speed),
                     m.direction < 0 ? "track unknown" : (std::to_string((int)m.direction) + " deg").c_str(), statusName(m.status));
        else snprintf(b, sizeof b, "Location: no position, %s", statusName(m.status));
        break;
    case Auth: snprintf(b, sizeof b, "Authentication: %s, page %d", authTypeName(m.authType), m.page); break;
    case SelfId: snprintf(b, sizeof b, "Self-ID: %s", m.text.c_str()); break;
    case System:
        if (m.opValid) snprintf(b, sizeof b, "System: operator %.6f %.6f (%s), %s %s", m.opLat, m.opLon, opLocTypeName(m.opLocType), categoryName(m.category), className(m.uaClass).c_str());
        else snprintf(b, sizeof b, "System: no operator position");
        break;
    case OperatorId: snprintf(b, sizeof b, "Operator ID: %s", m.text.c_str()); break;
    default: snprintf(b, sizeof b, "%s", msgTypeName(m.type)); break;
    }
    return b;
}

} // namespace dect2::rid
