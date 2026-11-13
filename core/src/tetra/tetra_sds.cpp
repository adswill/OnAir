// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tetra/tetra_sds.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::tetra {

namespace {
uint64_t take(const Bits& b, size_t& p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 1 | (b[p++] & 1); return v; }
void put(Bits& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
int64_t signedOf(uint64_t v, int n) { return (v & (1ull << (n - 1))) ? (int64_t)v - (int64_t)(1ull << n) : (int64_t)v; }
const int kErrM[8] = {2, 20, 200, 2000, 20000, 200000, 1000000, -1};   // position error upper bounds (code 7 unknown)
double speedOf(int k) { return k <= 28 ? (double)k : k == 127 ? -1.0 : 16.0 * std::pow(1.038, k - 13); }

// bounds-checked bit reader for the long report: any read past `end` marks it failed and returns 0
struct Rd {
    const Bits& b; size_t p, end; bool bad = false;
    uint64_t get(int n) { if (bad || p + (size_t)n > end) { bad = true; return 0; } return take(b, p, n); }
    size_t left() const { return bad || p > end ? 0 : end - p; }
};
const double kConf[8] = {50, 68, 80, 90, 95, 99, 99.9, -1};
}

const char* lipReasonText(int r) {
    static const char* t[33] = {"powered on", "powered off", "emergency", "push-to-talk", "status", "transmit inhibit on", "transmit inhibit off", "system access (TMO)",
        "DMO on", "enter service", "service loss", "cell reselection", "low battery", "connected to car kit", "disconnected from car kit", "transfer initialisation",
        "arrival at destination", "arrival at a defined location", "approaching a defined location", "SDS type 1 entered", "user application", "lost location",
        "regained location", "leaving point", "ambience listening", "start of temporary reporting", "return to normal reporting", "call setup type 1", "call setup type 2",
        "positioning device on", "positioning device off", "", "response to immediate request"};
    if (r >= 0 && r <= 32) return t[r];
    if (r == 129) return "maximum reporting interval";
    if (r == 130) return "maximum reporting distance";
    return "";
}

double lipHorizUncM(int k) { return k >= 63 || k < 0 ? -1.0 : 5.0 * std::pow(1.2, k) - 4.0; }

double lipAltitudeM(int v) {
    if (v <= 0 || v > 2047) return -1e9;
    if (v <= 1201) return -200.0 + (v - 1);
    if (v <= 1926) return 1000.0 + 2.0 * (v - 1201);
    return 2450.0 + 75.0 * (v - 1926);
}

bool parseLipShort(const Bits& b, size_t p, SdsMsg& o) {
    if (b.size() < p + 76) return false;
    if (take(b, p, 2) != 0) return false;                 // PDU type 0: short location report
    o.timeElapsed = (int)take(b, p, 2);
    const int64_t lo = signedOf(take(b, p, 25), 25), la = signedOf(take(b, p, 24), 24);
    o.lon = (double)lo * 360.0 / (double)(1 << 25);
    o.lat = (double)la * 180.0 / (double)(1 << 24);
    o.posErrorM = kErrM[take(b, p, 3)];
    o.speedKmh = speedOf((int)take(b, p, 7));
    const int d = (int)take(b, p, 4);
    o.headingDeg = d == 15 ? -1 : (int)std::lround(d * 22.5);
    const int addType = (int)take(b, p, 1);
    const int data = (int)take(b, p, 8);
    if (addType == 0) o.reason = data;
    o.hasPos = std::fabs(o.lat) <= 90 && std::fabs(o.lon) <= 180;
    o.lipPdu = 0;
    o.horizUncM = o.posErrorM;
    if (o.hasPos) {
        o.source = "LIP short location report";
        if (addType == 0 && lipReasonText(o.reason)[0]) { o.source += ", reason: "; o.source += lipReasonText(o.reason); }
    }
    return o.hasPos;
}


bool parseLipLong(const Bits& b, size_t p, size_t end, SdsMsg& o) {
    if (end > b.size()) end = b.size();
    Rd r{b, p, end};
    if (r.get(2) != 1 || r.get(4) != 3) return false;     // PDU type 1 (long location message), extension 3 (long location report)
    SdsMsg m = o;
    m.lipPdu = 1;
    const int timeType = (int)r.get(2);
    if (timeType == 1) m.timeElapsed = (int)r.get(2);
    else if (timeType == 2) {
        m.tpDay = (int)r.get(5); m.tpHour = (int)r.get(5); m.tpMin = (int)r.get(6); m.tpSec = (int)r.get(6);
        m.hasTimePos = !r.bad && m.tpDay >= 1 && m.tpHour <= 23 && m.tpMin <= 59 && m.tpSec <= 59;
    } else if (timeType == 3) { DIAG_DEBUG_EVERY(767779, 5, "A LIP long location report has the reserved time type 3, so OnAir ignores it."); return false; }
    const int shape = (int)r.get(4);
    m.shape = shape;
    auto point = [&] {
        const int64_t lo = signedOf(r.get(25), 25), la = signedOf(r.get(24), 24);
        m.lon = (double)lo * 360.0 / (double)(1 << 25);
        m.lat = (double)la * 180.0 / (double)(1 << 24);
    };
    auto altitude = [&] {
        m.altRef = (int)r.get(1);
        const double a = lipAltitudeM((int)r.get(11));
        if (a > -1e8) { m.hasAlt = true; m.altM = a; }
    };
    static const char* shapeName[11] = {"no location", "point", "circle", "ellipse", "point with altitude", "circle with altitude", "ellipse with altitude",
                                        "circle with altitude and uncertainty", "ellipse with altitude and uncertainty", "arc", "point and position error"};
    bool pos = true;
    switch (shape) {
    case 0: pos = false; break;
    case 1: point(); break;
    case 2: point(); m.horizUncM = lipHorizUncM((int)r.get(6)); break;
    case 3: case 6: case 8:
        point(); m.horizUncM = lipHorizUncM((int)r.get(6)); r.get(6); r.get(8);
        if (shape >= 6) altitude();
        if (shape == 8) r.get(3);
        m.confidencePct = (int)kConf[r.get(3)];
        break;
    case 4: point(); altitude(); break;
    case 5: point(); m.horizUncM = lipHorizUncM((int)r.get(6)); altitude(); break;
    case 7: point(); m.horizUncM = lipHorizUncM((int)r.get(6)); altitude(); r.get(3); break;
    case 9: point(); r.get(16); m.horizUncM = 2.0 * (double)r.get(16); r.get(8); r.get(8); m.confidencePct = (int)kConf[r.get(3)]; break;
    case 10: point(); m.horizUncM = kErrM[r.get(3)]; m.posErrorM = (int)m.horizUncM; break;
    default: DIAG_DEBUG_EVERY(609222, 5, "A LIP long location report has the reserved location shape %d, so OnAir ignores it.", shape); return false;
    }
    const int vt = (int)r.get(3);
    static const int vbits[8] = {0, 7, 10, 15, 21, 15, 21, 32};
    if (r.left() < (size_t)vbits[vt]) r.bad = true;
    if (!r.bad && vt) {
        const int hv = (int)r.get(7);
        m.speedKmh = speedOf(hv);
        if (vt == 2 || vt == 4 || vt == 6 || vt == 7) r.get(3);
        if (vt == 3 || vt == 4 || vt == 7) {
            const int sign = (int)r.get(1), vv = (int)r.get(7);
            const double v = speedOf(vv);
            if (v >= 0) { m.hasVert = true; m.vertKmh = sign ? -v : v; }
            if (vt == 4 || vt == 7) r.get(3);
        }
        if (vt >= 5) { m.headingDeg = (int)std::lround((double)r.get(8) * 360.0 / 256.0) % 360; if (vt >= 6) r.get(3); }
    }
    m.ackRequested = (int)r.get(1);
    const int addType = (int)r.get(1);
    const int add = (int)r.get(8);
    if (r.bad) { DIAG_DEBUG_EVERY(367383, 5, "A LIP long location report ends before its fixed part is complete, so OnAir drops it."); return false; }
    if (addType == 0) m.reason = add;
    // type 5 elements: identifier 5 bits, length 6 bits (0: a 7 bit extension counts octets from 8), data; fill bits shorter than an element end it
    while (r.left() >= 11) {
        const int id = (int)r.get(5);
        size_t len = (size_t)r.get(6);
        if (len == 0) len = ((size_t)r.get(7) + 7) * 8;
        if (r.bad || len > r.left()) break;
        const size_t next = r.p + len;
        Rd e{b, r.p, next};
        if (id == 17 && len >= 28 && e.get(4) == 1) m.terminalIssi = (int)e.get(24);
        else if (id == 6 && len >= 8) m.msgRef = (int)e.get(8);
        else if (id == 12 && len >= 8) m.resultCode = (int)e.get(8);
        else if (id == 15 && len >= 16) m.statusValue = (int)e.get(16);
        r.p = next;
    }
    m.hasPos = pos && !r.bad && std::fabs(m.lat) <= 90 && std::fabs(m.lon) <= 180;
    m.source = std::string("LIP long location report, ") + shapeName[shape];
    if (addType == 0 && lipReasonText(m.reason)[0]) { m.source += ", reason: "; m.source += lipReasonText(m.reason); }
    if (m.statusValue >= 0) { char t[40]; snprintf(t, sizeof t, ", status %d", m.statusValue); m.source += t; }
    o = m;
    return true;
}

bool parseLip(const Bits& b, size_t p, size_t end, SdsMsg& o) {
    if (end > b.size()) end = b.size();
    if (p + 2 > end) return false;
    const int type = (int)(((b[p] & 1) << 1) | (b[p + 1] & 1));
    if (type == 0) { if (p + 76 > end) return false; return parseLipShort(b, p, o); }
    if (type == 1) return parseLipLong(b, p, end, o);
    return false;
}

bool parseStatus(const Bits& b, SdsMsg& o) {
    o = SdsMsg();
    size_t p = 0;
    if (b.size() < 5 + 2 + 16) return false;
    if (take(b, p, 5) != 8) return false;                 // D-STATUS
    const int cpti = (int)take(b, p, 2);
    if (cpti == 1 || cpti == 2) { if (b.size() < p + 24) return false; o.callingIssi = (int)take(b, p, 24); }
    if (cpti == 2) { if (b.size() < p + 24) return false; take(b, p, 24); }
    if (b.size() < p + 16) return false;
    const int st = (int)take(b, p, 16);
    char t[32]; snprintf(t, sizeof t, "status %d (0x%04X)", st, st);
    o.kind = "status"; o.text = t; o.valid = true;
    return true;
}

bool parseSdsData(const Bits& b, SdsMsg& o) {
    o = SdsMsg();
    size_t p = 0;
    if (b.size() < 5 + 2 + 2) return false;
    if (take(b, p, 5) != 15) return false;                // D-SDS-DATA
    const int cpti = (int)take(b, p, 2);
    if (cpti == 1 || cpti == 2) { if (b.size() < p + 24) return false; o.callingIssi = (int)take(b, p, 24); }
    if (cpti == 2) { if (b.size() < p + 24) return false; take(b, p, 24); }   // extension (country / network): not used
    if (b.size() < p + 2) return false;
    o.dataType = (int)take(b, p, 2);
    size_t len;
    if (o.dataType == 0) len = 16; else if (o.dataType == 1) len = 32; else if (o.dataType == 2) len = 64;
    else { if (b.size() < p + 11) return false; len = (size_t)take(b, p, 11); }
    if (b.size() < p + len) return false;
    o.valid = true;
    o.kind = "status / data";
    o.text = std::to_string(len) + " bits";
    if (o.dataType == 3 && len >= 8) {
        o.pid = (int)take(b, p, 8);
        const size_t rest = len - 8;
        if (o.pid == 0x0A && rest >= 2) {
            SdsMsg lip;
            if (parseLip(b, p, p + rest, lip) && (lip.hasPos || lip.lipPdu == 1)) {
                o.kind = lip.hasPos ? "location" : "status / data";
                o.lipPdu = lip.lipPdu; o.shape = lip.shape; o.hasPos = lip.hasPos; o.lat = lip.lat; o.lon = lip.lon; o.posErrorM = lip.posErrorM; o.speedKmh = lip.speedKmh;
                o.headingDeg = lip.headingDeg; o.timeElapsed = lip.timeElapsed; o.reason = lip.reason;
                o.hasAlt = lip.hasAlt; o.altM = lip.altM; o.altRef = lip.altRef; o.horizUncM = lip.horizUncM; o.confidencePct = lip.confidencePct;
                o.hasVert = lip.hasVert; o.vertKmh = lip.vertKmh; o.hasTimePos = lip.hasTimePos; o.tpDay = lip.tpDay; o.tpHour = lip.tpHour; o.tpMin = lip.tpMin; o.tpSec = lip.tpSec;
                o.terminalIssi = lip.terminalIssi; o.msgRef = lip.msgRef; o.resultCode = lip.resultCode; o.statusValue = lip.statusValue; o.ackRequested = lip.ackRequested;
                o.source = lip.source;
                if (lip.hasPos) { char t[96]; snprintf(t, sizeof t, "%.5f, %.5f", lip.lat, lip.lon); o.text = t; }
                else { o.text = lip.source; }
            }
        } else if ((o.pid == 0x02 || o.pid == 0x09) && rest >= 8) {
            take(b, p, 8);   // text coding scheme: only 8 bit ISO 8859-1 is read as text here
            std::string t;
            for (size_t i = 0; i + 8 <= rest - 8; i += 8) { const char c = (char)take(b, p, 8); t += c >= 32 || c == '\n' ? c : '.'; }
            o.kind = "text"; o.text = t;
        }
    }
    return true;
}

Bits buildLipShort(double lat, double lon, int err, int spd, int dir, int tel, int reason) {
    Bits b;
    put(b, 0, 2); put(b, (uint64_t)tel, 2);
    put(b, (uint64_t)((int64_t)std::llround(lon * (double)(1 << 25) / 360.0) & ((1 << 25) - 1)), 25);
    put(b, (uint64_t)((int64_t)std::llround(lat * (double)(1 << 24) / 180.0) & ((1 << 24) - 1)), 24);
    put(b, (uint64_t)err, 3); put(b, (uint64_t)spd, 7); put(b, (uint64_t)dir, 4);
    put(b, 0, 1); put(b, (uint64_t)reason, 8);
    return b;
}

Bits buildLipLong(const LipLongSpec& c) {
    Bits b;
    put(b, 1, 2); put(b, 3, 4);
    put(b, (uint64_t)c.timeType, 2);
    if (c.timeType == 1) put(b, (uint64_t)c.timeElapsed, 2);
    else if (c.timeType == 2) { put(b, (uint64_t)c.day, 5); put(b, (uint64_t)c.hour, 5); put(b, (uint64_t)c.minute, 6); put(b, (uint64_t)c.second, 6); }
    put(b, (uint64_t)c.shape, 4);
    auto point = [&] {
        put(b, (uint64_t)((int64_t)std::llround(c.lon * (double)(1 << 25) / 360.0) & ((1 << 25) - 1)), 25);
        put(b, (uint64_t)((int64_t)std::llround(c.lat * (double)(1 << 24) / 180.0) & ((1 << 24) - 1)), 24);
    };
    auto alt = [&] { put(b, (uint64_t)c.altRef, 1); put(b, (uint64_t)c.altValue, 11); };
    switch (c.shape) {
    case 0: break;
    case 1: point(); break;
    case 2: point(); put(b, (uint64_t)c.hpu, 6); break;
    case 3: case 6: case 8:
        point(); put(b, (uint64_t)c.hpu, 6); put(b, (uint64_t)c.minorAxis, 6); put(b, (uint64_t)c.angle, 8);
        if (c.shape >= 6) alt();
        if (c.shape == 8) put(b, (uint64_t)c.altUnc, 3);
        put(b, (uint64_t)c.confidence, 3);
        break;
    case 4: point(); alt(); break;
    case 5: point(); put(b, (uint64_t)c.hpu, 6); alt(); break;
    case 7: point(); put(b, (uint64_t)c.hpu, 6); alt(); put(b, (uint64_t)c.altUnc, 3); break;
    case 9: point(); put(b, (uint64_t)c.inner, 16); put(b, (uint64_t)c.outer, 16); put(b, (uint64_t)c.startAngle, 8); put(b, (uint64_t)c.stopAngle, 8); put(b, (uint64_t)c.confidence, 3); break;
    case 10: point(); put(b, (uint64_t)c.posErr, 3); break;
    default: break;
    }
    put(b, (uint64_t)c.velType, 3);
    const int vt = c.velType;
    if (vt) {
        put(b, (uint64_t)c.hv, 7);
        if (vt == 2 || vt == 4 || vt == 6 || vt == 7) put(b, (uint64_t)c.hvUnc, 3);
        if (vt == 3 || vt == 4 || vt == 7) { put(b, (uint64_t)c.vvSign, 1); put(b, (uint64_t)c.vv, 7); if (vt != 3) put(b, (uint64_t)c.vvUnc, 3); }
        if (vt >= 5) { put(b, (uint64_t)c.dirExt, 8); if (vt >= 6) put(b, (uint64_t)c.dirUnc, 3); }
    }
    put(b, (uint64_t)c.ack, 1); put(b, (uint64_t)c.addType, 1); put(b, (uint64_t)c.addData, 8);
    for (const LipType5& t : c.type5) {
        put(b, (uint64_t)t.id, 5);
        if (t.data.size() <= 63 && !t.data.empty()) put(b, t.data.size(), 6);
        else { put(b, 0, 6); put(b, (t.data.size() + 7) / 8 - 7, 7); }
        b.insert(b.end(), t.data.begin(), t.data.end());
        if (t.data.size() > 63) for (size_t i = t.data.size(); i % 8; i++) b.push_back(1);   // fill bits are ones
    }
    return b;
}

Bits buildSdsData(int issi, const Bits& ud, int pid) {
    Bits b;
    put(b, 15, 5); put(b, 1, 2); put(b, (uint64_t)issi, 24); put(b, 3, 2);
    put(b, 8 + ud.size(), 11); put(b, (uint64_t)pid, 8);
    b.insert(b.end(), ud.begin(), ud.end());
    return b;
}

Bits buildSdsText(int issi, const std::string& s) {
    Bits ud;
    put(ud, 1, 8);   // coding scheme: ISO 8859-1
    for (char c : s) put(ud, (uint8_t)c, 8);
    return buildSdsData(issi, ud, 0x02);
}

} // namespace dect2::tetra
