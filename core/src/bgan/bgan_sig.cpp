// BGAN forward-bearer signalling parser and test builder (bgan_sig.h). ETSI TS 102 744-3-1.
#include "dect2/bgan/bgan_sig.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dect2::bgan {

uint16_t crcX25(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return uint16_t(~c);
}

double forwardMhz(int offsetBit, int channelIndex) { return channelIndex * 0.0025 + offsetBit * 0.00125 + 1510.0; }

namespace {

bool crcGood(const uint8_t* d, size_t n) {   // n includes the two CRC octets
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return c == 0xF0B8;
}

int sign12(int v) { return v >= 2048 ? v - 4096 : v; }
int sext(int v, int bits) { const int m = 1 << (bits - 1); return (v ^ m) - m; }

void format(std::string& s, const char* f, ...) __attribute__((format(printf, 2, 3)));
void format(std::string& s, const char* f, ...) {
    char b[300];
    va_list ap;
    va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    s += b;
}

void parseAvpList(const uint8_t* d, size_t n, CarrierSig& s, std::string* line) {
    size_t p = 0;
    while (p < n) {
        const int t = d[p];
        const int len = (t & 7) + 1;
        if (p + 1 + len > n) break;
        const uint8_t* v = d + p + 1;
        switch (t) {
        case 0xA9: case 0xAD: {                    // SatelliteLocation
            SatLoc sl;
            sl.id = v[0] >> 4;
            sl.lonDeg = sign12((v[0] & 0xF) << 8 | v[1]) / 10.0;
            if (t == 0xAD) {
                const int a = v[2] << 8 | v[3], b = v[4] << 8 | v[5];
                sl.primaryMhz = forwardMhz(a >> 15, a & 0x7FFF);
                sl.alternateMhz = forwardMhz(b >> 15, b & 0x7FFF);
            }
            if (s.sats.empty() || s.sats[0].id == sl.id) { if (s.sats.empty()) s.sats.push_back(sl); else if (t == 0xAD) s.sats[0] = sl; }
            else { bool f = false; for (auto& o : s.sats) if (o.id == sl.id) { o = sl; f = true; } if (!f) s.sats.push_back(sl); }
            if (line) format(*line, " satellite %d at %.1f %s", sl.id, std::fabs(sl.lonDeg), sl.lonDeg >= 0 ? "E" : "W");
            break;
        }
        case 0x21: {                              // BeamInfo
            s.beamType = v[0] & 7;
            s.beamSpot = v[1];
            if (line) format(*line, " beam %s %d", describeBeamType(s.beamType).c_str(), s.beamSpot);
            break;
        }
        case 0xF2: case 0xF3: {                   // PLMNInfo
            Plmn pl;
            const int a = v[0] >> 4, b = v[0] & 15, c = v[1] >> 4, m1 = v[1] & 15, m2 = v[2] >> 4, m3 = v[2] & 15;
            pl.mcc = a * 100 + b * 10 + c;
            if (m3 == 15) { pl.mnc = m1 * 10 + m2; pl.mncDigits = 2; }
            else { pl.mnc = m1 * 100 + m2 * 10 + m3; pl.mncDigits = 3; }
            bool f = false;
            for (auto& o : s.plmns) if (o.mcc == pl.mcc && o.mnc == pl.mnc) f = true;
            if (!f) s.plmns.push_back(pl);
            if (line) format(*line, " network %03d-%02d", pl.mcc, pl.mnc);
            break;
        }
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: {   // PrimaryBearer
            if (len < 4) break;
            PrimaryBearer b;
            b.spot = v[0];
            const int ch = v[1] << 8 | v[2];
            b.mhz = forwardMhz(ch >> 15, ch & 0x7FFF);
            b.fBearer = (v[3] >> 4) & 7;
            b.fbType = v[3] & 15;
            s.primaries[b.spot] = b;
            break;
        }
        case 0xD4: {                              // UTCDateAndTime
            s.haveUtc = true;
            s.utcY = 2000 + (v[0] >> 1);
            s.utcMo = (v[0] & 1) << 3 | v[1] >> 5;
            s.utcD = v[1] & 31;
            s.utcH = v[2] >> 3;
            s.utcMi = (v[2] & 7) << 3 | v[3] >> 5;
            s.utcS = (v[3] & 31) << 1 | v[4] >> 7;
            s.utcMs = (v[4] & 127) * 10;
            if (line) format(*line, " GPS time %04d-%02d-%02d %02d:%02d:%02d", s.utcY, s.utcMo, s.utcD, s.utcH, s.utcMi, s.utcS);
            break;
        }
        case 0xAC: {                              // SatelliteStateVectors
            s.haveState = true;
            s.stateX = sext(v[0] << 3 | v[1] >> 5, 11);
            s.stateY = sext((v[1] & 31) << 6 | v[2] >> 2, 11);
            s.stateZ = sext((v[2] & 3) << 12 | v[3] << 4 | v[4] >> 4, 14);
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: {   // ForwardBearerCodeRate
            int base = 0, last = -1;
            s.haveNextCode = true;
            for (int i = 0; i < 16; i++) { s.nextCodeRate[i] = -1; s.nextCodeMod16[i] = 0; }
            int cur = -1, curM = 0, prevBlock = -1;
            std::vector<std::pair<int, std::pair<int,int>>> ch;
            for (int i = 0; i < len; i++) {
                const int mii = v[i] >> 7, bn = v[i] >> 4 & 7, cr = sext(v[i] & 15, 4);
                if (last >= 0 && bn <= last) base += 8;
                last = bn;
                ch.push_back({base + bn, {cr, mii}});
            }
            (void)cur; (void)curM; (void)prevBlock;
            for (size_t i = 0; i < ch.size(); i++) {
                const int from = std::clamp(ch[i].first, 0, 15), to = i + 1 < ch.size() ? std::clamp(ch[i + 1].first, 0, 16) : 16;
                for (int b = from; b < to; b++) { s.nextCodeRate[b] = ch[i].second.first + 8; s.nextCodeMod16[b] = ch[i].second.second; }
            }
            if (line) *line += " code-rate change";
            break;
        }
        case 0xAA: if (line) *line += " access control"; break;
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            if (line) format(*line, " NAS info (%d octets)", len);
            break;
        default: break;
        }
        p += 1 + len;
    }
}

const int kPlanReservedShort[16] = { 0, 8, 8, 7, 6, 5, 4, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
const int kPlanReservedCount[16] = { 0, 2, 5, 4, 3, 2, 1, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

void parseReturnSchedule(const uint8_t* d, size_t n, CarrierSig& s, std::string* line) {
    if (n < 3) return;
    const int ch = d[0] << 8 | d[1];
    const double mhz = (ch & 0x7FFF) * 0.0025 + (ch >> 15) * 0.00125 + 1611.5;
    const int flags = d[2] >> 4;
    const bool btIncl = flags & 8, resIncl = flags & 4;
    const int plans = (flags & 3) + 1;
    int nib = 5;                               // the 4-bit nibble index after the flags
    auto rd = [&](int bits) { int v = 0; for (int i = 0; i < bits; i++) { if (size_t(nib / 2) >= n) return -1; const int b = (d[nib / 2] >> (nib % 2 ? 0 : 4)) & 15; (void)b; } return v; };
    (void)rd;
    int nibble = 5;
    auto getN = [&]() -> int { if (size_t(nibble / 2) >= n) return -1; const int v = (d[nibble / 2] >> (nibble % 2 ? 0 : 4)) & 15; nibble++; return v; };
    (void)nib;
    int rbt = -1, spot = -1;
    if (btIncl) { rbt = getN(); const int a = getN(), b = getN(); if (a >= 0 && b >= 0) spot = a * 16 + b; }
    int reserved = 0, res = 0, slots = 0;
    for (int i = 0; i < plans; i++) { const int p = getN(); if (p < 0) break; reserved += kPlanReservedShort[p]; res += kPlanReservedCount[p]; slots += 8; }
    if (resIncl) { for (int i = 0; i < (res + 3) / 4; i++) getN(); }
    int ids = 0;
    while (true) { const int a = getN(), b = getN(), c = getN(); if (a < 0 || b < 0 || c < 0) break; ids++; }
    s.schedules++; s.schedReservedSum += reserved; s.schedSlotsSum += slots; s.connectionsListed += ids;
    if (ch != 0 && ch != 0xFFFF) s.returnMhz = mhz;
    if (rbt >= 0) s.returnBearer = rbt;
    if (line) {
        format(*line, " return schedule %.4f MHz %d/%d slots reserved, %d connection%s", mhz, reserved, slots, ids, ids == 1 ? "" : "s");
        if (spot >= 0 && spot != 255) format(*line, " spot %d", spot);
    }
}

} // namespace

std::string describeBeamType(int t) { return t == 0 ? "global" : t == 1 ? "regional" : t == 2 ? "narrow spot" : "unknown"; }
std::string fbearerTypeName(int t) {
    static const char* const n[] = { "?", "F80T1Q-4B", "F80T1X-4B", "F80T4.5X-8B", "F80T1Q-1B", "FR80T2.5X4/16-5B", "FR80T2.5X16-5B", "FR80T2.5X32-6B", "FR80T2.5X64-7B", "FR80T5X4/16-9B", "FR80T5X16-9B", "FR80T5X32-11B", "FR80T5X64-13B" };
    return t >= 0 && t <= 12 ? n[t] : "?";
}
std::string returnBearerTypeName(int t) {
    static const char* const n[] = { "R20/80T0.5Q", "R20T1Q", "R5/20T2Q", "R5/20T4.5Q", "R20/80T1Q", "R5/20T1X", "R5/20T2X", "R5/20T4.5X", "R80T2.5X4", "R80T5X4", "R80T2.5X16", "R80T5X16", "R80T2.5X32", "R80T5X32", "R80T2.5X64", "R80T5X64" };
    return t >= 0 && t < 16 ? n[t] : "?";
}
std::string describeSatellite(double lon) {
    static const struct { double l; const char* n; } k[] = { {-98.0, "Inmarsat-4 F3 / Alphasat"}, {-53.0, "Inmarsat-3 / Atlantic West"}, {24.9, "Alphasat (25 E)"}, {63.9, "Inmarsat-4 F1 (64 E)"}, {143.5, "Inmarsat-4 F2 (143.5 E)"}, {-15.0, "Inmarsat-5 region"} };
    for (const auto& e : k) if (std::fabs(lon - e.l) < 1.5) { char b[80]; snprintf(b, sizeof b, "%s (%.1f %s)", e.n, std::fabs(lon), lon >= 0 ? "E" : "W"); return b; }
    char b[48]; snprintf(b, sizeof b, "%.1f %s", std::fabs(lon), lon >= 0 ? "E" : "W");
    return b;
}

int parseBlock(const uint8_t* d, size_t n, CarrierSig& s, std::vector<std::string>* lines) {
    bool anyGood = false, allZero = true;
    for (size_t i = 0; i < n; i++) if (d[i]) { allZero = false; break; }
    if (allZero) return 2;
    size_t p = 0;
    while (p + 2 < n) {
        const int b0 = d[p];
        if (b0 == 0) {                             // padding: a broadcast header has bit 4 set, an all-zero header is not valid
            bool z = true;
            for (size_t k = p; k < n; k++) if (d[k]) { z = false; break; }
            if (z) break;
            s.badPdus++;
            return anyGood ? 1 : 0;
        }
        const bool follows = b0 & 0x80, lenFlag = b0 & 0x40;
        const int addr = b0 >> 4 & 3;
        size_t hdr = 0;
        if (addr == 0) hdr = 1;
        else if (addr == 1) hdr = 2;
        else if (addr == 2 && !(b0 & 8)) hdr = 4;
        else if (addr == 2) {                      // addressed common signalling (carries a subscriber identity): count it and stop reading this block; not parsed
            s.addressedPdus++;
            return anyGood ? 1 : 0;
        } else { s.badPdus++; return anyGood ? 1 : 0; }
        size_t lenField = 0;
        if (lenFlag) { if (p + hdr >= n) break; lenField = d[p + hdr]; hdr += 1; }
        size_t contentEnd;
        if (lenFlag) contentEnd = p + hdr + lenField;
        else contentEnd = std::min(n - 2, p + hdr + 256);
        if (contentEnd + 2 > n) { s.badPdus++; return anyGood ? 1 : 0; }
        if (!crcGood(d + p, contentEnd + 2 - p)) { s.badPdus++; return anyGood ? 1 : 0; }
        anyGood = true;
        if (addr != 0) { s.connectionPdus++; p = contentEnd + 2; continue; }   // a connection's traffic or control: counted only
        s.broadcastPdus++;
        std::string line;
        size_t q = p + hdr;
        bool more = follows;
        while (more && q < contentEnd) {
            const int sb = d[q];
            more = sb & 0x80;
            int type, slen;
            size_t pay;
            if (!(sb & 0x40)) { type = sb >> 3 & 7; slen = (sb & 7) + 1; pay = q + 1; }
            else { type = sb & 63; if (q + 1 >= contentEnd) break; slen = int(d[q + 1]) + 1; pay = q + 2; }
            if (pay + slen > contentEnd) break;
            const uint8_t* v = d + pay;
            switch (type) {
            case 1: if (slen >= 4) {                         // BulletinBoard
                s.haveBb = true;
                s.rnc = v[0]; s.netVer = v[1] >> 4; s.frameNo = (v[1] & 15) << 8 | v[2];
                const bool sp = v[3] & 0x80;
                s.fBearer = v[3] >> 4 & 7;
                s.bctId = v[3] & 15;
                size_t al = 4;
                if (sp) { if (slen >= 5) s.spotBeam = v[4]; al = 5; } else s.spotBeam = -1;
                format(line, "bulletin rnc %d frame %d bearer %d", s.rnc, s.frameNo, s.fBearer);
                if (s.spotBeam >= 0) format(line, " spot %d", s.spotBeam);
                if (slen > int(al)) parseAvpList(v + al, slen - al, s, &line);
                break; }
            case 2: parseReturnSchedule(v, slen, s, &line); break;
            case 7: parseAvpList(v, slen, s, &line); break;
            case 8: if (slen >= 3) { s.spotMapVersion = v[0]; s.spotMapTotal = v[1]; format(line, " spot beam map v%d (%d spots)", v[0], v[1]); } break;
            case 9: s.bearerTableEntries += slen; line += " bearer tables"; break;
            case 10: if (slen >= 2) { s.gpsEphPrn = v[0] & 31; format(line, " GPS ephemeris"); } break;
            case 12: {
                static const char* const p1[8] = { "primary bearers", "spot beam map", "GPS ephemeris", "PLMN", "satellite location", "state vectors", "UTC time", "timing interval" };
                static const char* const p2[8] = { "retry", "delays", "randomising", "access control", "access burst", "GPS policy", "NAS info", "return bearer" };
                std::string c;
                const int present = v[0];
                int off = 1;
                for (int part = 0; part < 7; part++) {
                    if (!(present >> (7 - part) & 1)) continue;
                    if (off >= slen) break;
                    const int bm = v[off];
                    if (part < 2) for (int i = 0; i < 8; i++) if (bm >> (7 - i) & 1) { c += c.empty() ? "" : ", "; c += part == 0 ? p1[i] : p2[i]; }
                    off += 1 + __builtin_popcount(bm) * 2;
                }
                if (!c.empty()) s.sysInfoIndex = c;
                break; }
            case 0: case 3: case 6: default: break;           // addressed or not useful for a display: skipped
            }
            q = pay + slen;
        }
        if (!line.empty() && lines) lines->push_back(line);
        p = contentEnd + 2;
    }
    return anyGood ? 1 : 0;
}

bool blockLooksValid(const uint8_t* d, size_t n) {
    bool z = true;
    for (size_t i = 0; i < n; i++) if (d[i]) { z = false; break; }
    if (z) return true;
    const int b0 = d[0];
    const bool lenFlag = b0 & 0x40;
    const int addr = b0 >> 4 & 3;
    size_t hdr = 0;
    if (addr == 0) hdr = 1; else if (addr == 1) hdr = 2; else if (addr == 2 && !(b0 & 8)) hdr = 4; else return false;
    size_t le = 0, cend;
    if (lenFlag) { if (hdr >= n) return false; le = d[hdr]; hdr += 1; }
    if (lenFlag) cend = hdr + le; else cend = std::min(n - 2, hdr + 256);
    if (cend + 2 > n) return false;
    return crcGood(d, cend + 2);
}

// ---- test signal ----
namespace {
void put(std::vector<uint8_t>& o, std::initializer_list<int> v) { for (int x : v) o.push_back(uint8_t(x)); }
void sdu(std::vector<uint8_t>& o, bool follows, int type, const std::vector<uint8_t>& pay) {
    if (pay.size() <= 8 && type < 8) o.push_back(uint8_t((follows ? 0x80 : 0) | (type & 7) << 3 | (pay.size() - 1)));
    else { o.push_back(uint8_t((follows ? 0x80 : 0) | 0x40 | (type & 63))); o.push_back(uint8_t(pay.size() - 1)); }
    o.insert(o.end(), pay.begin(), pay.end());
}
void pdu(std::vector<uint8_t>& o, const std::vector<uint8_t>& body, int sigType, int len) {   // broadcast, with a length field
    const size_t base = o.size();
    (void)len;
    o.push_back(uint8_t(0x80 | 0x40 | 0x08 | 1));   // follows, length, broadcast (addr 0), comsig flag, information (1)
    o.push_back(uint8_t(body.size()));
    o.insert(o.end(), body.begin(), body.end());
    (void)sigType;
    const uint16_t c = crcX25(o.data() + base, o.size() - base);
    o.push_back(uint8_t(c & 255)); o.push_back(uint8_t(c >> 8));
}
void connPdu(std::vector<uint8_t>& o, int tbcn, int contentBytes) {
    const size_t base = o.size();
    o.push_back(uint8_t(0x40 | 0x10 | (tbcn >> 8 & 15)));
    o.push_back(uint8_t(tbcn));
    o.push_back(uint8_t(contentBytes));
    for (int i = 0; i < contentBytes; i++) o.push_back(uint8_t((i * 37 + tbcn) & 255));
    const uint16_t c = crcX25(o.data() + base, o.size() - base);
    o.push_back(uint8_t(c & 255)); o.push_back(uint8_t(c >> 8));
}
}

void buildBlock(const TxSig& tx, int frameNo, int block, std::vector<uint8_t>& out, size_t bytes) {
    out.clear();
    if (block == 0) {
        std::vector<uint8_t> avps;
        const int locRaw = int(std::lround(tx.lonDeg * 10)) & 0xFFF;
        put(avps, { 0xA9, (tx.satId & 15) << 4 | (locRaw >> 8), locRaw & 255 });
        put(avps, { 0x21, tx.beamType & 7, tx.spotBeam });
        const int m1 = tx.mcc / 100, m2 = tx.mcc / 10 % 10, m3 = tx.mcc % 10, n1 = tx.mnc / 10, n2 = tx.mnc % 10;
        put(avps, { 0xF2, m1 << 4 | m2, m3 << 4 | n1, n2 << 4 | 15 });
        const int ch = int(std::lround((tx.primaryMhz - 1510.0) / 0.0025)) & 0x7FFF;
        put(avps, { 0x1C, tx.spotBeam, ch >> 8, ch & 255, (tx.fBearer & 7) << 4 | 3, 0x3F });   // PrimaryBearer with 8-bit class control
        put(avps, { 0xD4, (2026 - 2000) << 1 | (11 >> 3), (11 & 7) << 5 | 9, 12 << 3 | (20 >> 3), (20 & 7) << 5 | (30 >> 1), (30 & 1) << 7 | 0 });
        std::vector<uint8_t> bb;
        put(bb, { tx.rnc, tx.netVer << 4 | (frameNo >> 8 & 15), frameNo & 255, 0x80 | (tx.fBearer & 7) << 4 | (tx.bctId & 15), tx.spotBeam });
        bb.insert(bb.end(), avps.begin(), avps.end());
        std::vector<uint8_t> rs;
        const int rch = int(std::lround((1645.0 - 1611.5) / 0.0025)) & 0x7FFF;
        put(rs, { rch >> 8, rch & 255, 0x80 | 11, tx.spotBeam, 0xC1, 0x23, 0x45, 0x67, 0x89 });   // plan 12: 3 reserved slots, 3 connection slots
        std::vector<uint8_t> body;
        sdu(body, true, 1, bb);
        sdu(body, false, 2, rs);
        pdu(out, body, 1, 0);
    }
    if (!tx.emptyLaterBlocks && block % 2 == 1) {
        const int room = int(bytes > out.size() + 12 ? bytes - out.size() - 12 : 0);
        if (room > 0) connPdu(out, 100 + block, std::min(room, 80));
    }
    if (out.size() > bytes) out.resize(bytes);
    out.resize(bytes, 0);
}

} // namespace dect2::bgan
