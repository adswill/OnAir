// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// See scada_proto.h. Written from the public descriptions of the protocols (Modbus Application Protocol Specification V1.1b3 and Modbus over
// Serial Line V1.02 of the Modbus Organization; IEEE 1815 / DNP3 data link, transport and application layers). No code was taken from
// other projects. Receive only: nothing here builds a frame.
#include "dect2/x40/scada_proto.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dect2::scada {

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[600];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

int le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// days since 1970-01-01 to a civil date (H. Hinnant's algorithm)
void civil(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}

std::string utcMs(int64_t ms) {
    if (ms < 0) return "?";
    const int64_t s = ms / 1000, days = s / 86400, sod = s % 86400;
    int y, m, d; civil(days, y, m, d);
    return fmt("%04d-%02d-%02d %02d:%02d:%02d.%03d UTC", y, m, d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60), (int)(ms % 1000));
}

bool dnpBroadcast(int a) { return a >= 0xFFFD; }

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) o += fmt("\\u%04x", c);
        else o += (char)c;
    }
    return o;
}
std::string csvEsc(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

// ---- DNP3 objects ----
// Value layout of the point objects: table, a flag octet first, the value type (1 u32, 2 u16, 3 i32, 4 i16, 5 float, 6 double, 7 bit in the flag octet, 8 double-bit in the flag octet, 9 CROB), a status octet after (commands)
struct ObjVal { int table = -1, flag = 0, type = 0, status = 0; };
ObjVal objVal(int g, int v) {
    ObjVal o;
    auto cnt = [&](int tab) { o.table = tab; const bool odd = v % 2 == 1; o.type = odd ? 1 : 2; o.flag = 1; };
    switch (g) {
    case 1: if (v == 2) { o.table = 10; o.type = 7; } break;
    case 2: o.table = 10; o.type = 7; break;
    case 3: if (v == 2) { o.table = 16; o.type = 8; } break;
    case 4: o.table = 16; o.type = 8; break;
    case 10: if (v == 2) { o.table = 11; o.type = 7; } break;
    case 11: o.table = 11; o.type = 7; break;
    case 12: if (v == 1) { o.table = 11; o.type = 9; } break;
    case 20: cnt(12); if (v >= 5) o.flag = 0; break;
    case 21: cnt(13); if (v >= 9) o.flag = 0; break;
    case 22: case 23: cnt(12); break;
    case 30: o.table = 14; switch (v) { case 1: o.flag = 1; o.type = 3; break; case 2: o.flag = 1; o.type = 4; break; case 3: o.type = 3; break; case 4: o.type = 4; break; case 5: o.flag = 1; o.type = 5; break; case 6: o.flag = 1; o.type = 6; break; default: o.table = -1; } break;
    case 32: case 42: o.table = g == 32 ? 14 : 15; o.flag = 1;
        switch (v) { case 1: case 3: o.type = 3; break; case 2: case 4: o.type = 4; break; case 5: case 7: o.type = 5; break; case 6: case 8: o.type = 6; break; default: o.table = -1; } break;
    case 40: o.table = 15; o.flag = 1; switch (v) { case 1: o.type = 3; break; case 2: o.type = 4; break; case 3: o.type = 5; break; case 4: o.type = 6; break; default: o.table = -1; } break;
    case 41: o.table = 15; o.status = 1; switch (v) { case 1: o.type = 3; break; case 2: o.type = 4; break; case 3: o.type = 5; break; case 4: o.type = 6; break; default: o.table = -1; } break;
    default: break;
    }
    return o;
}

// size of one object (octets); bits > 0 for packed objects; -1 when not known
int objSize(int g, int v, int& bits) {
    bits = 0;
    static const int k20[] = {0, 5, 3, 5, 3, 4, 2, 4, 2};
    static const int k21[] = {0, 5, 3, 5, 3, 11, 9, 11, 9, 4, 2, 4, 2};
    static const int k22[] = {0, 5, 3, 5, 3, 11, 9, 11, 9};
    static const int k30[] = {0, 5, 3, 4, 2, 5, 9};
    static const int k31[] = {0, 5, 3, 11, 9, 4, 2, 5, 9};
    static const int k32[] = {0, 5, 3, 11, 9, 5, 9, 11, 15};
    static const int k43[] = {0, 6, 4, 12, 10, 6, 10, 12, 16};
    switch (g) {
    case 1: if (v == 1) { bits = 1; return 0; } return v == 2 ? 1 : -1;
    case 2: case 4: return v == 1 ? 1 : v == 2 ? 7 : v == 3 ? 3 : -1;
    case 3: if (v == 1) { bits = 2; return 0; } return v == 2 ? 1 : -1;
    case 10: if (v == 1) { bits = 1; return 0; } return v == 2 ? 1 : -1;
    case 11: case 13: return v == 1 ? 1 : v == 2 ? 7 : -1;
    case 12: if (v == 3) { bits = 1; return 0; } return v == 1 || v == 2 ? 11 : -1;
    case 20: return v >= 1 && v <= 8 ? k20[v] : -1;
    case 21: return v >= 1 && v <= 12 ? k21[v] : -1;
    case 22: case 23: return v >= 1 && v <= 8 ? k22[v] : -1;
    case 30: return v >= 1 && v <= 6 ? k30[v] : -1;
    case 31: return v >= 1 && v <= 8 ? k31[v] : -1;
    case 32: case 33: case 42: return v >= 1 && v <= 8 ? k32[v] : -1;
    case 34: return v == 1 ? 2 : v == 2 || v == 3 ? 4 : -1;
    case 40: case 41: return v == 1 ? 5 : v == 2 ? 3 : v == 3 ? 5 : v == 4 ? 9 : -1;
    case 43: return v >= 1 && v <= 8 ? k43[v] : -1;
    case 50: return v == 1 || v == 3 ? 6 : v == 2 ? 10 : v == 4 ? 11 : -1;
    case 51: return v == 1 || v == 2 ? 6 : -1;
    case 52: return v == 1 || v == 2 ? 2 : -1;
    case 80: if (v == 1) { bits = 1; return 0; } return -1;
    case 110: case 111: case 112: case 113: return v > 0 ? v : -1;
    default: return -1;
    }
}

std::string decodeVal(const ObjVal& o, const uint8_t* d) {
    const uint8_t* p = d + o.flag;
    std::string s;
    switch (o.type) {
    case 1: s = fmt("%u", le32(p)); break;
    case 2: s = fmt("%u", (unsigned)le16(p)); break;
    case 3: s = fmt("%d", (int32_t)le32(p)); break;
    case 4: s = fmt("%d", (int)(int16_t)le16(p)); break;
    case 5: { float f; uint32_t u = le32(p); std::memcpy(&f, &u, 4); s = fmt("%g", (double)f); break; }
    case 6: { double f; uint64_t u = (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); std::memcpy(&f, &u, 8); s = fmt("%g", f); break; }
    case 7: return (d[0] & 0x80) ? "1" : "0";
    case 8: { static const char* const k[4] = {"intermediate", "off", "on", "indeterminate"}; return k[(d[0] >> 6) & 3]; }
    case 9: {
        static const char* const op[5] = {"null", "pulse on", "pulse off", "latch on", "latch off"};
        const int code = d[0] & 0x0F, tc = d[0] >> 6;
        return fmt("%s%s, count %d, on %u ms, off %u ms", code <= 4 ? op[code] : "operation ?", tc == 1 ? " close" : tc == 2 ? " trip" : "", d[1], le32(d + 2), le32(d + 6));
    }
    default: return "";
    }
    if (o.flag && (d[0] & 0x01) == 0) s += " (offline)";
    return s;
}

bool reqCarriesData(int fc) { return fc == 2 || fc == 3 || fc == 4 || fc == 5 || fc == 6 || fc == 11 || fc == 12 || fc == 24 || fc == 31; }

} // namespace

uint16_t crcModbus(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1); }
    return c;
}
uint16_t crcDnp(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA6BC) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}
uint16_t crcHdlc(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}

std::string hex(const uint8_t* p, size_t n, size_t maxBytes) {
    std::string s;
    const size_t m = std::min(n, maxBytes);
    for (size_t i = 0; i < m; i++) { if (i) s += ' '; s += fmt("%02X", p[i]); }
    if (n > m) s += fmt(" ... (%zu bytes)", n);
    return s;
}

bool modbusKnownFunction(int fc) {
    switch (fc) { case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 12: case 15: case 16: case 17: case 20: case 21: case 22: case 23: case 24: case 43: return true; default: return false; }
}

const char* modbusFunctionName(int fc) {
    switch (fc) {
    case 1: return "read coils"; case 2: return "read discrete inputs"; case 3: return "read holding registers"; case 4: return "read input registers";
    case 5: return "write single coil"; case 6: return "write single register"; case 7: return "read exception status"; case 8: return "diagnostics";
    case 11: return "get comm event counter"; case 12: return "get comm event log"; case 15: return "write multiple coils"; case 16: return "write multiple registers";
    case 17: return "report server ID"; case 20: return "read file record"; case 21: return "write file record"; case 22: return "mask write register";
    case 23: return "read/write multiple registers"; case 24: return "read FIFO queue"; case 43: return "encapsulated interface (MEI)";
    default: return "function not known";
    }
}

const char* modbusExceptionName(int c) {
    switch (c) {
    case 1: return "illegal function"; case 2: return "illegal data address"; case 3: return "illegal data value"; case 4: return "server device failure";
    case 5: return "acknowledge"; case 6: return "server device busy"; case 7: return "negative acknowledge"; case 8: return "memory parity error";
    case 10: return "gateway path unavailable"; case 11: return "gateway target failed to respond";
    default: return "exception code not known";
    }
}

const char* dnpFunctionName(int fc) {
    static const char* const k[34] = {"confirm", "read", "write", "select", "operate", "direct operate", "direct operate, no ack", "immediate freeze", "immediate freeze, no ack",
        "freeze and clear", "freeze and clear, no ack", "freeze at time", "freeze at time, no ack", "cold restart", "warm restart", "initialize data", "initialize application",
        "start application", "stop application", "save configuration", "enable unsolicited", "disable unsolicited", "assign class", "delay measurement", "record current time",
        "open file", "close file", "delete file", "get file info", "authenticate file", "abort file", "activate config", "authentication request", "authentication request, no ack"};
    if (fc >= 0 && fc < 34) return k[fc];
    if (fc == 129) return "response";
    if (fc == 130) return "unsolicited response";
    if (fc == 131) return "authentication response";
    return "function not known";
}

const char* dnpLinkFunctionName(bool primary, int fc) {
    if (primary) {
        switch (fc) { case 0: return "reset link states"; case 2: return "test link states"; case 3: return "confirmed user data"; case 4: return "unconfirmed user data"; case 9: return "request link status"; default: return "link function not known"; }
    }
    switch (fc) { case 0: return "ack"; case 1: return "nack"; case 11: return "link status"; case 15: return "not supported"; default: return "link function not known"; }
}

std::string dnpIinText(int i1, int i2) {
    static const char* const a[8] = {"all stations", "class 1 events", "class 2 events", "class 3 events", "need time", "local control", "device trouble", "device restart"};
    static const char* const b[8] = {"function not supported", "object unknown", "parameter error", "event buffer overflow", "already executing", "config corrupt", "reserved 2.6", "reserved 2.7"};
    std::string s;
    for (int k = 0; k < 8; k++) if (i1 & (1 << k)) { if (!s.empty()) s += ", "; s += a[k]; }
    for (int k = 0; k < 8; k++) if (i2 & (1 << k)) { if (!s.empty()) s += ", "; s += b[k]; }
    return s.empty() ? "none" : s;
}

const char* tableName(int t) {
    switch (t) {
    case 0: return "coil"; case 1: return "discrete input"; case 3: return "input register"; case 4: return "holding register";
    case 10: return "BI"; case 11: return "BO"; case 12: return "counter"; case 13: return "frozen counter"; case 14: return "AI"; case 15: return "AO"; case 16: return "double-bit input";
    default: return "?";
    }
}

std::string pointLabel(int proto, int table, int index) {
    if (proto == Modbus) {
        const int ref = table == 0 ? 1 : table == 1 ? 10001 : table == 3 ? 30001 : 40001;
        return fmt("%05d (%s %d)", ref + index, tableName(table), index);
    }
    return fmt("%s %d", tableName(table), index);
}

std::vector<size_t> modbusLengths(const uint8_t* p, size_t n) {
    std::vector<size_t> L;
    if (n < 2) return L;
    const int fc = p[1];
    if (fc & 0x80) { L.push_back(5); return L; }
    switch (fc) {
    case 1: case 2: case 3: case 4:
        L.push_back(8);
        if (n >= 3 && p[2] > 0 && (fc <= 2 || p[2] % 2 == 0)) L.push_back(5 + (size_t)p[2]);
        break;
    case 5: case 6: case 8: L.push_back(8); break;
    case 7: L.push_back(4); L.push_back(5); break;
    case 11: L.push_back(4); L.push_back(8); break;
    case 12: case 17: L.push_back(4); if (n >= 3) L.push_back(5 + (size_t)p[2]); break;
    case 15: case 16:
        L.push_back(8);
        if (n >= 7) {
            const int qty = be16(p + 4), bc = p[6];
            if (qty > 0 && bc == (fc == 15 ? (qty + 7) / 8 : 2 * qty)) L.push_back(9 + (size_t)bc);
        }
        break;
    case 20: case 21: if (n >= 3 && p[2] > 0) L.push_back(5 + (size_t)p[2]); break;
    case 22: L.push_back(10); break;
    case 23:
        if (n >= 11 && p[10] > 0 && p[10] == 2 * be16(p + 8)) L.push_back(13 + (size_t)p[10]);
        if (n >= 3 && p[2] > 0 && p[2] % 2 == 0) L.push_back(5 + (size_t)p[2]);
        break;
    case 24: L.push_back(6); if (n >= 4) L.push_back(6 + (size_t)be16(p + 2)); break;
    default: break;
    }
    std::sort(L.begin(), L.end());
    L.erase(std::unique(L.begin(), L.end()), L.end());
    return L;
}

std::vector<Found> findFrames(const uint8_t* p, size_t n) {
    std::vector<Found> out;
    size_t s = 0;
    while (s + 4 <= n) {
        // DNP3: start octets and the header CRC
        if (p[s] == 0x05 && p[s + 1] == 0x64 && s + 10 <= n && p[s + 2] >= 5 && crcDnp(p + s, 8) == le16(p + s + 8)) {
            const size_t ul = (size_t)p[s + 2] - 5, total = 10 + ul + 2 * ((ul + 15) / 16);
            if (s + total <= n) {
                Found f; f.proto = Dnp3; f.start = s; f.len = total; f.crcOk = true;
                size_t q = s + 10, left = ul;
                for (int blk = 0; left > 0; blk++) {
                    const size_t b = std::min<size_t>(16, left);
                    if (crcDnp(p + q, b) != le16(p + q + b)) { f.crcOk = false; f.badBlock = blk; break; }
                    q += b + 2; left -= b;
                }
                out.push_back(f); s += total; continue;
            }
        }
        const int a = p[s], fc = p[s + 1];
        bool hit = false;
        if (a <= 247) {
            if (fc & 0x80) {
                const int ex = s + 3 <= n ? p[s + 2] : 0;
                if (a > 0 && modbusKnownFunction(fc & 0x7F) && ((ex >= 1 && ex <= 8) || ex == 10 || ex == 11) && s + 5 <= n && crcModbus(p + s, 5) == 0) {
                    Found f; f.proto = Modbus; f.start = s; f.len = 5; f.crcOk = true; out.push_back(f); s += 5; hit = true;
                }
            } else if (modbusKnownFunction(fc) && (a > 0 || fc == 5 || fc == 6 || fc == 15 || fc == 16 || fc == 8)) {
                for (size_t L : modbusLengths(p + s, n - s)) {
                    if (L >= 4 && s + L <= n && crcModbus(p + s, L) == 0) {
                        Found f; f.proto = Modbus; f.start = s; f.len = L; f.crcOk = true; out.push_back(f); s += L; hit = true; break;
                    }
                }
            }
        }
        if (!hit) s++;
    }
    if (out.empty() && n >= 4 && n <= 256 && p[0] >= 1 && p[0] <= 247) {
        const int fc = p[1];
        if (crcModbus(p, n) == 0) { Found f; f.proto = Modbus; f.len = n; f.crcOk = true; out.push_back(f); }   // a function this reader does not know: the CRC covers the whole burst
        else if (!(fc & 0x80) && modbusKnownFunction(fc)) {
            const std::vector<size_t> L = modbusLengths(p, n);
            if (std::find(L.begin(), L.end(), n) != L.end()) { Found f; f.proto = Modbus; f.len = n; f.crcOk = false; out.push_back(f); }
        }
    }
    return out;
}

// ---------------------------------------------------------------- tracker

void Tracker::clear() {
    msgs_.clear(); dev_.clear(); mbPending_.clear(); dnpApp_.clear(); dnpLink_.clear(); dnpTransport_.clear(); dnpTseq_.clear();
    counts = Counts{};
}

Message& Tracker::push(Message m) {
    m.id = nextId_++;
    msgs_.push_back(std::move(m));
    while (msgs_.size() > kMaxMessages) msgs_.pop_front();
    return msgs_.back();
}

const Message* Tracker::find(uint64_t id) const {
    if (msgs_.empty() || id < msgs_.front().id || id > msgs_.back().id) return nullptr;
    const Message& m = msgs_[(size_t)(id - msgs_.front().id)];
    return m.id == id ? &m : nullptr;
}

void Tracker::relabel(uint64_t id, const std::string& link) {
    for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) if (it->id == id) { it->link = link; return; }
}

Device& Tracker::device(int proto, int addr, const char* role, double t) {
    Device& d = dev_[{proto, addr}];
    if (d.role.empty()) { d.proto = proto; d.addr = addr; d.role = role; d.firstSec = t; }
    else if (d.role != role && std::strcmp(role, "?") != 0 && d.role != "?") { if (d.role.find(role) == std::string::npos) d.role += std::string(" and ") + role; }
    else if (d.role == "?") d.role = role;
    d.lastSec = t;
    return d;
}

std::vector<Device> Tracker::devices() const {
    std::vector<Device> v;
    for (const auto& kv : dev_) v.push_back(kv.second);
    return v;
}

uint64_t Tracker::add(const Frame& f) {
    Message m;
    m.timeSec = f.timeSec; m.link = f.link; m.proto = f.proto; m.raw = f.bytes;
    m.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (f.proto == Unknown) {
        counts.unknown++;
        m.from = m.to = "?"; m.crc = -1;
        m.func = "not recognised";
        m.summary = fmt("%s%s%zu bytes: unknown, proprietary or encrypted framing (for example MDS or MOSCAD); shown raw, not decoded",
                        f.note.c_str(), f.note.empty() ? "" : ", ", f.bytes.size());
        return push(std::move(m)).id;
    }
    m.crc = f.crcOk ? 1 : 0;
    if (!f.crcOk) counts.crcBad++;
    if (f.proto == Modbus) { counts.modbus++; addModbus(f, m); }
    else { counts.dnp3++; addDnp(f, m); }
    if (m.func.empty()) return 0;
    const uint64_t pairWith = m.pair;
    Message& pm = push(std::move(m));
    if (pairWith) {
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) if (it->id == pairWith) { if (!it->pair) it->pair = pm.id; break; }   // a response keeps its request; its confirm points to it
        counts.paired++;
    }
    return pm.id;
}

void Tracker::addModbus(const Frame& f, Message& m) {
    const std::vector<uint8_t>& b = f.bytes;
    const size_t L = b.size();
    const int a = b[0], fc = b[1];
    const double t = f.timeSec;
    const std::string slave = a == 0 ? std::string("all (broadcast)") : fmt("slave %d", a);
    if (!f.crcOk) {
        m.from = "?"; m.to = slave; m.func = modbusFunctionName(fc & 0x7F);
        m.summary = "CRC failed: not decoded";
        if (a) device(Modbus, a, "slave", t).crcBad++;
        return;
    }
    auto pit = mbPending_.find(a);
    const PendingMb* pend = pit != mbPending_.end() && t - pit->second.t < 5.0 ? &pit->second : nullptr;
    const int fcBase = fc & 0x7F;
    bool resp = false;
    if (fc & 0x80) resp = true;
    else switch (fc) {
        case 1: case 2: case 3: case 4: resp = L == (size_t)b[2] + 5 && (L != 8 || (pend && pend->fc == fc)); break;
        case 5: case 6: case 8: case 22: resp = pend && pend->fc == fc && pend->bytes == b; break;
        case 7: case 11: case 12: case 17: resp = L != 4; break;
        case 15: case 16: resp = L == 8; break;
        case 23: resp = !(L >= 13 && L == (size_t)b[10] + 13) || (pend && pend->fc == 23 && L == (size_t)b[2] + 5); break;
        case 24: resp = L != 6; break;
        default: resp = pend && pend->fc == fc; break;
    }
    if (!modbusKnownFunction(fcBase)) m.func = fmt("function %d (not known)", fcBase);
    else m.func = modbusFunctionName(fcBase);
    auto pl = [](int table, int idx) { return pointLabel(Modbus, table, idx); };
    const int tableOf = fcBase == 1 ? 0 : fcBase == 2 ? 1 : fcBase == 4 ? 3 : 4;

    if (!resp) {
        m.dir = 1; m.from = "master"; m.to = slave;
        device(Modbus, -1, "master", t);
        Device* d = a ? &device(Modbus, a, "slave", t) : nullptr;
        if (d) { d->polls++; d->lastPollSec = t; if (pend) d->unanswered++; }
        PendingMb p; p.fc = fc; p.t = t; p.bytes = b;
        auto setPoint = [&](int table, int idx, const std::string& v) { if (d && d->points.size() < kMaxPoints) d->points[{table, idx}] = Point{v, t}; };
        switch (fc) {
        case 1: case 2: case 3: case 4:
            p.start = be16(&b[2]); p.qty = be16(&b[4]);
            m.summary = fmt("%d %s%s from %s", p.qty, tableName(tableOf), p.qty == 1 ? "" : "s", pl(tableOf, p.start).c_str());
            break;
        case 5: {
            const int v = be16(&b[4]);
            m.summary = fmt("%s = %s", pl(0, be16(&b[2])).c_str(), v == 0xFF00 ? "ON" : v == 0 ? "OFF" : "invalid value");
            setPoint(0, be16(&b[2]), v == 0xFF00 ? "ON (written)" : "OFF (written)");
            break;
        }
        case 6:
            m.summary = fmt("%s = %d (0x%04X)", pl(4, be16(&b[2])).c_str(), be16(&b[4]), be16(&b[4]));
            setPoint(4, be16(&b[2]), fmt("%d (0x%04X, written)", be16(&b[4]), be16(&b[4])));
            break;
        case 8: m.summary = fmt("sub-function %d, data 0x%04X", be16(&b[2]), be16(&b[4])); break;
        case 15: {
            const int st = be16(&b[2]), q = be16(&b[4]);
            std::string vs;
            for (int i = 0; i < q && 7 + i / 8 < (int)L - 2; i++) { const int v = (b[7 + i / 8] >> (i % 8)) & 1; if (i < 32) vs += v ? '1' : '0'; setPoint(0, st + i, v ? "ON (written)" : "OFF (written)"); }
            m.summary = fmt("%d coils from %s: %s%s", q, pl(0, st).c_str(), vs.c_str(), q > 32 ? "..." : "");
            break;
        }
        case 16: {
            const int st = be16(&b[2]), q = be16(&b[4]);
            std::string vs;
            for (int i = 0; i < q && 7 + 2 * i + 1 < (int)L - 2; i++) { const int v = be16(&b[7 + 2 * i]); if (i < 12) vs += fmt("%s%d", i ? ", " : "", v); setPoint(4, st + i, fmt("%d (0x%04X, written)", v, v)); }
            m.summary = fmt("%d registers from %s: %s%s", q, pl(4, st).c_str(), vs.c_str(), q > 12 ? ", ..." : "");
            break;
        }
        case 22: m.summary = fmt("%s AND 0x%04X OR 0x%04X", pl(4, be16(&b[2])).c_str(), be16(&b[4]), be16(&b[6])); break;
        case 23: {
            p.start = be16(&b[2]); p.qty = be16(&b[4]);
            const int ws = be16(&b[6]), wq = be16(&b[8]);
            for (int i = 0; i < wq && 11 + 2 * i + 1 < (int)L - 2; i++) setPoint(4, ws + i, fmt("%d (written)", be16(&b[11 + 2 * i])));
            m.summary = fmt("read %d from %s, write %d from %s", p.qty, pl(4, p.start).c_str(), wq, pl(4, ws).c_str());
            break;
        }
        case 24: m.summary = fmt("FIFO at %s", pl(4, be16(&b[2])).c_str()); break;
        default: m.summary = L > 4 ? fmt("data %s", hex(&b[2], L - 4, 32).c_str()) : std::string("no data"); break;
        }
        if (a) { p.id = nextId_; mbPending_[a] = p; }
        return;
    }

    // a response
    m.dir = 2; m.from = slave; m.to = "master";
    Device& d = device(Modbus, a, "slave", t);
    device(Modbus, -1, "master", t);
    d.replies++;
    const bool paired = pend && pend->fc == fcBase;
    if (paired) {
        m.pair = pend->id; m.rttMs = (t - pend->t) * 1000;
        d.rttSumMs += m.rttMs; d.rttN++;
    }
    auto setPoint = [&](int table, int idx, const std::string& v) { if (d.points.size() < kMaxPoints) d.points[{table, idx}] = Point{v, t}; };
    if (fc & 0x80) {
        d.exceptions++;
        m.func = "exception";
        m.summary = fmt("%s (code %d) to %s", modbusExceptionName(b[2]), b[2], modbusFunctionName(fcBase));
    } else switch (fc) {
        case 1: case 2: {
            const int bc = b[2], q = paired ? pend->qty : bc * 8;
            std::string vs;
            for (int i = 0; i < q && i < bc * 8; i++) {
                const int v = (b[3 + i / 8] >> (i % 8)) & 1;
                if (i < 32) vs += v ? '1' : '0';
                if (paired) setPoint(tableOf, pend->start + i, v ? "ON" : "OFF");
            }
            m.summary = paired ? fmt("%d %ss from %s: %s%s", q, tableName(tableOf), pl(tableOf, pend->start).c_str(), vs.c_str(), q > 32 ? "..." : "")
                               : fmt("%d bytes of bits (request not heard): %s", bc, vs.c_str());
            break;
        }
        case 3: case 4: {
            const int n = b[2] / 2;
            std::string vs;
            for (int i = 0; i < n; i++) {
                const int v = be16(&b[3 + 2 * i]);
                if (i < 12) vs += fmt("%s%d", i ? ", " : "", v);
                if (paired) setPoint(tableOf, pend->start + i, fmt("%d (0x%04X)", v, v));
            }
            m.summary = paired ? fmt("%d %ss from %s: %s%s", n, tableName(tableOf), pl(tableOf, pend->start).c_str(), vs.c_str(), n > 12 ? ", ..." : "")
                               : fmt("%d registers (request not heard): %s%s", n, vs.c_str(), n > 12 ? ", ..." : "");
            break;
        }
        case 5: m.summary = fmt("confirmed: %s = %s", pl(0, be16(&b[2])).c_str(), be16(&b[4]) == 0xFF00 ? "ON" : "OFF"); setPoint(0, be16(&b[2]), be16(&b[4]) == 0xFF00 ? "ON" : "OFF"); break;
        case 6: m.summary = fmt("confirmed: %s = %d", pl(4, be16(&b[2])).c_str(), be16(&b[4])); setPoint(4, be16(&b[2]), fmt("%d (0x%04X)", be16(&b[4]), be16(&b[4]))); break;
        case 7: m.summary = fmt("exception status 0x%02X", b[2]); break;
        case 8: m.summary = fmt("echo: sub-function %d, data 0x%04X", be16(&b[2]), be16(&b[4])); break;
        case 11: m.summary = fmt("status 0x%04X, %d events", be16(&b[2]), be16(&b[4])); break;
        case 12: m.summary = fmt("event log, %d bytes", b[2]); break;
        case 15: case 16: m.summary = fmt("confirmed: %d %s from %s", be16(&b[4]), fc == 15 ? "coils" : "registers", pl(fc == 15 ? 0 : 4, be16(&b[2])).c_str()); break;
        case 17: m.summary = fmt("server ID: %s", hex(&b[3], std::min<size_t>(b[2], L - 5), 32).c_str()); break;
        case 22: m.summary = fmt("confirmed: %s AND 0x%04X OR 0x%04X", pl(4, be16(&b[2])).c_str(), be16(&b[4]), be16(&b[6])); break;
        case 23: {
            const int n = b[2] / 2;
            std::string vs;
            for (int i = 0; i < n; i++) { const int v = be16(&b[3 + 2 * i]); if (i < 12) vs += fmt("%s%d", i ? ", " : "", v); if (paired) setPoint(4, pend->start + i, fmt("%d (0x%04X)", v, v)); }
            m.summary = fmt("%d registers read: %s", n, vs.c_str());
            break;
        }
        case 24: m.summary = fmt("FIFO: %d values", L >= 6 ? be16(&b[4]) : 0); break;
        default: m.summary = L > 4 ? fmt("data %s", hex(&b[2], L - 4, 32).c_str()) : std::string("no data"); break;
    }
    if (pend && pend->fc == fcBase) mbPending_.erase(a);
}

void Tracker::addDnp(const Frame& f, Message& m) {
    const std::vector<uint8_t>& b = f.bytes;
    const double t = f.timeSec;
    const int len = b[2], ctrl = b[3], dst = le16(&b[4]), src = le16(&b[6]);
    const bool fromMaster = (ctrl & 0x80) != 0, prm = (ctrl & 0x40) != 0;
    const int lfc = ctrl & 0x0F;
    m.from = fmt("%s %d", fromMaster ? "master" : "outstation", src);
    m.to = dnpBroadcast(dst) ? std::string("all (broadcast)") : fmt("%s %d", fromMaster ? "outstation" : "master", dst);
    Device& ds = device(Dnp3, src, fromMaster ? "master" : "outstation", t);
    if (!dnpBroadcast(dst)) {
        Device& dd = device(Dnp3, dst, fromMaster ? "outstation" : "master", t);
        if (std::find(dd.peers.begin(), dd.peers.end(), src) == dd.peers.end()) dd.peers.push_back(src);
    }
    if (std::find(ds.peers.begin(), ds.peers.end(), dst) == ds.peers.end()) ds.peers.push_back(dst);
    m.dir = fromMaster ? 1 : 2;
    if (!f.crcOk) {
        ds.crcBad++;
        m.func = dnpLinkFunctionName(prm, lfc);
        m.summary = fmt("CRC failed in data block %d: not decoded", f.badBlock);
        dnpTransport_.erase({src, dst});
        return;
    }
    // the link layer: requests that want a link reply, and the replies
    if (prm && (lfc == 0 || lfc == 2 || lfc == 3 || lfc == 9)) dnpLink_[{src, dst}] = PendingDnp{nextId_, t, lfc};
    if (!prm) {
        auto it = dnpLink_.find({dst, src});
        if (it != dnpLink_.end() && t - it->second.t < 5.0) { m.pair = it->second.id; m.rttMs = (t - it->second.t) * 1000; dnpLink_.erase(it); }
    }
    const bool userData = prm && (lfc == 3 || lfc == 4) && len > 5;
    if (!userData) {
        m.func = dnpLinkFunctionName(prm, lfc);
        m.summary = fmt("link layer%s%s", prm && (ctrl & 0x10) ? ((ctrl & 0x20) ? ", FCB 1" : ", FCB 0") : "", !prm && (ctrl & 0x10) ? ", data flow control" : "");
        return;
    }
    std::vector<uint8_t> ud;
    {
        size_t q = 10, left = (size_t)len - 5;
        while (left > 0) { const size_t k = std::min<size_t>(16, left); ud.insert(ud.end(), b.begin() + (ptrdiff_t)q, b.begin() + (ptrdiff_t)(q + k)); q += k + 2; left -= k; }
    }
    const int th = ud[0], seq = th & 0x3F;
    const bool fin = (th & 0x80) != 0, fir = (th & 0x40) != 0;
    const auto key = std::make_pair(src, dst);
    auto it = dnpTransport_.find(key);
    if (fir) { dnpTransport_[key].assign(ud.begin() + 1, ud.end()); dnpTseq_[key] = seq; }
    else if (it != dnpTransport_.end() && ((dnpTseq_[key] + 1) & 0x3F) == seq) {
        it->second.insert(it->second.end(), ud.begin() + 1, ud.end()); dnpTseq_[key] = seq;
        if (it->second.size() > 4096) { dnpTransport_.erase(key); m.func = "transport segment"; m.summary = "application fragment over 4096 bytes: dropped"; return; }
    } else {
        dnpTransport_.erase(key);
        m.func = "transport segment";
        m.summary = fmt("segment %d without its first segment: the fragment is dropped", seq);
        return;
    }
    if (!fin) {
        m.func = "transport segment";
        m.summary = fmt("segment %d%s, %zu bytes of the application fragment so far", seq, fir ? " (first)" : "", dnpTransport_[key].size());
        return;
    }
    std::vector<uint8_t> app = std::move(dnpTransport_[key]);
    dnpTransport_.erase(key);
    dnpApp(app, src, dst, fromMaster, m);
}

void Tracker::dnpApp(const std::vector<uint8_t>& a, int src, int dst, bool fromMaster, Message& m) {
    const double t = m.timeSec;
    if (a.size() < 2) { m.func = "application fragment"; m.summary = "too short to read"; return; }
    const int ac = a[0], fc = a[1], seq = ac & 0x0F;
    const bool con = (ac & 0x20) != 0, uns = (ac & 0x10) != 0;
    const int master = fromMaster ? src : dst, outst = fromMaster ? dst : src;
    m.func = dnpFunctionName(fc);
    std::vector<std::string> parts;
    if (fc == 32 || fc == 33 || fc == 131) {
        m.encrypted = true; counts.encrypted++;
        m.summary = "Secure Authentication message: shown raw, not decoded";
        return;
    }
    const bool resp = fc == 129 || fc == 130;
    size_t pos = 2;
    Device* od = dnpBroadcast(outst) ? nullptr : &device(Dnp3, outst, "outstation", t);
    if (resp) {
        if (a.size() < 4) { m.summary = "response too short to read"; return; }
        const std::string iin = dnpIinText(a[2], a[3]);
        if (od) od->lastIin = iin;
        if (a[2] || a[3]) parts.push_back("IIN: " + iin);
        pos = 4;
    }
    const bool data = resp || reqCarriesData(fc);
    bool classPoll[4] = {false, false, false, false};
    int nObj = 0;
    while (pos + 3 <= a.size()) {
        const int g = a[pos], v = a[pos + 1], q = a[pos + 2];
        pos += 3;
        if (g == 120) { m.encrypted = true; counts.encrypted++; parts.push_back("g120 Secure Authentication object: not decoded"); break; }
        const int pre = (q >> 4) & 7, rc = q & 0x0F;
        long start = -1, stop = -1, count = 0;
        auto need = [&](size_t k) { return pos + k <= a.size(); };
        bool ok = true, unknownQ = false;
        switch (rc) {
        case 0: if (!need(2)) { ok = false; break; } start = a[pos]; stop = a[pos + 1]; pos += 2; break;
        case 1: if (!need(4)) { ok = false; break; } start = le16(&a[pos]); stop = le16(&a[pos + 2]); pos += 4; break;
        case 2: if (!need(8)) { ok = false; break; } start = (long)le32(&a[pos]); stop = (long)le32(&a[pos + 4]); pos += 8; break;
        case 6: count = 0; break;
        case 7: if (!need(1)) { ok = false; break; } count = a[pos]; pos += 1; break;
        case 8: if (!need(2)) { ok = false; break; } count = le16(&a[pos]); pos += 2; break;
        case 9: if (!need(4)) { ok = false; break; } count = (long)le32(&a[pos]); pos += 4; break;
        case 0xB: if (!need(1)) { ok = false; break; } count = a[pos]; pos += 1; break;
        default: parts.push_back(fmt("g%dv%d qualifier 0x%02X not known: the rest is not read", g, v, q)); unknownQ = true; ok = false; break;
        }
        if (!ok) { if (!unknownQ) parts.push_back("object header cut short"); break; }
        if (rc <= 2) { if (stop < start) { parts.push_back(fmt("g%dv%d range %ld-%ld not valid: the rest is not read", g, v, start, stop)); break; } count = stop - start + 1; }
        nObj++;
        std::string desc = fmt("g%dv%d", g, v);
        if (rc <= 2) desc += fmt(" [%ld-%ld]", start, stop);
        else if (rc == 6) desc += " all";
        else desc += fmt(" x%ld", count);
        if (g == 60 && v >= 1 && v <= 4) { classPoll[v - 1] = true; continue; }
        if (!data || rc == 6) { parts.push_back(desc); continue; }
        int bits = 0;
        const int sz = objSize(g, v, bits);
        const ObjVal ov = objVal(g, v);
        const int psz = pre == 1 ? 1 : pre == 2 ? 2 : pre == 3 ? 4 : pre == 4 ? 1 : pre == 5 ? 2 : pre == 6 ? 4 : 0;
        if (bits > 0) {
            if (pre != 0) { parts.push_back(desc + ": packed objects with an index prefix: the rest is not read"); break; }
            const size_t nb = (size_t)((count * bits + 7) / 8);
            if (pos + nb > a.size()) { parts.push_back(desc + ": cut short"); break; }
            if (od && (g == 1 || g == 10 || g == 3)) {
                for (long i = 0; i < count; i++) {
                    const int bit = (int)(i * bits);
                    const int val = (a[pos + (size_t)bit / 8] >> (bit % 8)) & ((1 << bits) - 1);
                    const int table = g == 1 ? 10 : g == 10 ? 11 : 16;
                    static const char* const dbl[4] = {"intermediate", "off", "on", "indeterminate"};
                    if (od->points.size() < kMaxPoints) od->points[{table, (int)((start >= 0 ? start : 0) + i)}] = Point{bits == 2 ? dbl[val & 3] : val ? "1" : "0", t};
                }
            }
            if (g == 80 && fc == 2) desc += " (clear device restart)";
            pos += nb;
            parts.push_back(desc);
            continue;
        }
        if (sz < 0 && pre < 4) { parts.push_back(desc + ": object size not known, the rest is not read"); break; }
        bool cut = false;
        std::string extra;
        for (long i = 0; i < count; i++) {
            long idx = start >= 0 ? start + i : i;
            if (pos + (size_t)psz > a.size()) { cut = true; break; }
            if (pre >= 1 && pre <= 3) idx = pre == 1 ? a[pos] : pre == 2 ? le16(&a[pos]) : (long)le32(&a[pos]);
            const int osz = pre >= 4 ? (pre == 4 ? a[pos] : pre == 5 ? le16(&a[pos]) : (int)le32(&a[pos])) : sz;
            pos += (size_t)psz;
            if (pos + (size_t)osz > a.size()) { cut = true; break; }
            const uint8_t* d = &a[pos];
            if (g == 50 && (v == 1 || v == 3) && osz >= 6) {
                const int64_t ms = (int64_t)le32(d) | ((int64_t)le16(d + 4) << 32);
                extra = fmt("time %s", utcMs(ms).c_str());
            } else if (g == 52 && osz >= 2) {
                extra = fmt("delay %d %s", le16(d), v == 1 ? "s" : "ms");
            } else if (ov.table >= 0 && od && od->points.size() < kMaxPoints && osz >= 1 + ov.flag) {
                std::string val = decodeVal(ov, d);
                if (!resp) val += " (command)";
                if (!val.empty()) od->points[{ov.table, (int)idx}] = Point{val, t};
            }
            pos += (size_t)osz;
        }
        if (!extra.empty()) desc += " " + extra;
        parts.push_back(cut ? desc + ": cut short" : desc);
        if (cut) break;
    }
    std::string head;
    if (fc == 1 && (classPoll[0] || classPoll[1] || classPoll[2] || classPoll[3])) {
        head = classPoll[0] && classPoll[1] && classPoll[2] && classPoll[3] ? "integrity poll (class 0 1 2 3)" : "class poll (";
        if (head.back() == '(') { bool f = true; for (int k = 0; k < 4; k++) if (classPoll[k]) { head += fmt("%s%d", f ? "class " : " ", k); f = false; } head += ")"; }
    } else if ((fc == 20 || fc == 21 || fc == 22) && (classPoll[1] || classPoll[2] || classPoll[3])) {
        head = "classes"; for (int k = 1; k < 4; k++) if (classPoll[k]) head += fmt(" %d", k);
    } else if (fc == 2 && !parts.empty() && parts[0].rfind("g50v", 0) == 0) head = "time sync";
    else if (fc == 23) head = "time sync: delay measurement";
    else if (fc == 24) head = "time sync: record current time";
    if (resp && nObj == 0) parts.insert(parts.begin(), "no objects");
    std::string s = head;
    for (const std::string& p : parts) { if (!s.empty()) s += "; "; s += p; }
    if (con) s += s.empty() ? "confirm requested" : "; confirm requested";
    m.summary = s.empty() ? std::string("no objects") : s;

    const auto pk = std::make_pair(master, outst);
    if (fc == 0) {                                                      // application confirm of a response that asked for one
        m.dir = 1;
        auto it = dnpApp_.find({pk, seq + 16});
        if (it != dnpApp_.end() && t - it->second.t < 10.0) { m.pair = it->second.id; m.rttMs = (t - it->second.t) * 1000; dnpApp_.erase(it); }
        return;
    }
    if (!resp) {
        m.dir = 1;
        if (od) { od->polls++; od->lastPollSec = t; }
        auto old = dnpApp_.find({pk, seq});
        if (old != dnpApp_.end() && od) od->unanswered++;
        dnpApp_[{pk, seq}] = PendingDnp{nextId_, t, fc};
        return;
    }
    if (fc == 130 || uns) { m.dir = 3; if (od) od->unsolicited++; }
    else {
        m.dir = 2;
        if (od) od->replies++;
        auto it = dnpApp_.find({pk, seq});
        if (it != dnpApp_.end() && t - it->second.t < 10.0) {
            m.pair = it->second.id; m.rttMs = (t - it->second.t) * 1000;
            if (od) { od->rttSumMs += m.rttMs; od->rttN++; }
            dnpApp_.erase(it);
        }
    }
    if (con) dnpApp_[{pk, seq + 16}] = PendingDnp{nextId_, t, fc};
}

std::string Tracker::csv() const {
    std::string s = "id,time_s,wall_clock_utc,link,protocol,from,to,function,summary,crc,direction,pair_id,response_ms,encrypted,raw_hex\n";
    static const char* const pn[3] = {"Modbus RTU", "DNP3", "unknown"};
    static const char* const dn[4] = {"", "request", "response", "unsolicited"};
    for (const Message& m : msgs_) {
        s += fmt("%llu,%.3f,", (unsigned long long)m.id, m.timeSec) + csvEsc(utcMs(m.wallMs)) + "," + csvEsc(m.link) + "," + pn[m.proto] + "," + csvEsc(m.from) + "," +
             csvEsc(m.to) + "," + csvEsc(m.func) + "," + csvEsc(m.summary) + "," + (m.crc == 1 ? "ok" : m.crc == 0 ? "failed" : "") + "," + dn[m.dir & 3] + "," +
             (m.pair ? fmt("%llu", (unsigned long long)m.pair) : std::string()) + "," + (m.rttMs >= 0 ? fmt("%.0f", m.rttMs) : std::string()) + "," +
             (m.encrypted ? "yes" : "") + "," + hex(m.raw.data(), m.raw.size(), 1 << 20) + "\n";
    }
    return s;
}

std::string Tracker::json() const {
    static const char* const pn[3] = {"modbus", "dnp3", "unknown"};
    static const char* const dn[4] = {"", "request", "response", "unsolicited"};
    std::string s = "{\"mode\":\"SCADA radio telemetry\",\"receive_only\":true,\"verified_on_live_signal\":false,\n\"messages\":[";
    bool first = true;
    for (const Message& m : msgs_) {
        s += first ? "\n" : ",\n"; first = false;
        s += fmt("{\"id\":%llu,\"time_s\":%.3f,\"wall_clock_utc\":\"%s\",", (unsigned long long)m.id, m.timeSec, utcMs(m.wallMs).c_str());
        s += "\"link\":\"" + jsonEsc(m.link) + "\",\"protocol\":\"" + pn[m.proto] + "\",\"from\":\"" + jsonEsc(m.from) + "\",\"to\":\"" + jsonEsc(m.to) +
             "\",\"function\":\"" + jsonEsc(m.func) + "\",\"summary\":\"" + jsonEsc(m.summary) + "\",\"crc\":" + (m.crc == 1 ? "\"ok\"" : m.crc == 0 ? "\"failed\"" : "null") +
             ",\"direction\":\"" + dn[m.dir & 3] + "\"";
        if (m.pair) s += fmt(",\"pair_id\":%llu", (unsigned long long)m.pair);
        if (m.rttMs >= 0) s += fmt(",\"response_ms\":%.0f", m.rttMs);
        if (m.encrypted) s += ",\"encrypted\":true";
        s += ",\"raw_hex\":\"" + hex(m.raw.data(), m.raw.size(), 1 << 20) + "\"}";
    }
    s += "\n],\n\"devices\":[";
    first = true;
    for (const auto& kv : dev_) {
        const Device& d = kv.second;
        s += first ? "\n" : ",\n"; first = false;
        s += fmt("{\"protocol\":\"%s\",\"address\":%d,\"role\":\"%s\",\"first_s\":%.3f,\"last_s\":%.3f,\"last_poll_s\":%.3f,\"polls\":%d,\"replies\":%d,\"exceptions\":%d,\"crc_failed\":%d,\"unanswered\":%d,\"unsolicited\":%d",
                 pn[d.proto], d.addr, jsonEsc(d.role).c_str(), d.firstSec, d.lastSec, d.lastPollSec, d.polls, d.replies, d.exceptions, d.crcBad, d.unanswered, d.unsolicited);
        if (d.rttN) s += fmt(",\"mean_response_ms\":%.0f", d.rttSumMs / d.rttN);
        if (!d.lastIin.empty()) s += ",\"iin\":\"" + jsonEsc(d.lastIin) + "\"";
        s += ",\"points\":[";
        bool pf = true;
        for (const auto& p : d.points) {
            s += pf ? "" : ","; pf = false;
            s += "{\"point\":\"" + jsonEsc(pointLabel(d.proto, p.first.first, p.first.second)) + "\",\"table\":\"" + tableName(p.first.first) + fmt("\",\"index\":%d,", p.first.second) +
                 "\"value\":\"" + jsonEsc(p.second.value) + fmt("\",\"time_s\":%.3f}", p.second.timeSec);
        }
        s += "]}";
    }
    s += "\n]}\n";
    return s;
}

Store& store() { static Store s; return s; }

} // namespace dect2::scada
