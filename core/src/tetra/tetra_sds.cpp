// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tetra/tetra_sds.h"
#include <cmath>

namespace dect2::tetra {

namespace {
uint64_t take(const Bits& b, size_t& p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 1 | (b[p++] & 1); return v; }
void put(Bits& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
int64_t signedOf(uint64_t v, int n) { return (v & (1ull << (n - 1))) ? (int64_t)v - (int64_t)(1ull << n) : (int64_t)v; }
const int kErrM[8] = {2, 20, 200, 2000, 20000, 200000, 1000000, -1};   // position error upper bounds (code 7 unknown)
double speedOf(int k) { return k <= 28 ? (double)k : k == 127 ? -1.0 : 16.0 * std::pow(1.038, k - 13); }
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
    return o.hasPos;
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
        if (o.pid == 0x0A && rest >= 76) {
            SdsMsg lip;
            if (parseLipShort(b, p, lip)) {
                o.kind = "location"; o.hasPos = true; o.lat = lip.lat; o.lon = lip.lon; o.posErrorM = lip.posErrorM; o.speedKmh = lip.speedKmh;
                o.headingDeg = lip.headingDeg; o.timeElapsed = lip.timeElapsed; o.reason = lip.reason;
                char t[96]; snprintf(t, sizeof t, "%.5f, %.5f", lip.lat, lip.lon); o.text = t;
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
