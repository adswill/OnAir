// Wireless M-Bus link layer and data records (see wmbus_proto.h). Clean room from the public EN 13757-3 / -4 descriptions.
#include "dect2/wmbus/wmbus_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace dect2::wmbus {

const char* phyName(Phy p) {
    switch (p) {
    case Phy::S1: return "S1";
    case Phy::T1: return "T1";
    case Phy::C1A: return "C1 (A)";
    case Phy::C1B: return "C1 (B)";
    case Phy::R2: return "R2";
    case Phy::N1: return "N1";
    default: return "?";
    }
}

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x3D65) : (uint16_t)(c << 1);
    }
    return (uint16_t)~c;
}

namespace {
const uint8_t kT36[16] = {0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13, 0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29};   // EN 13757-4 3-out-of-6 table
}
int encode36(unsigned n) { return kT36[n & 15]; }
int decode36(unsigned six) {
    for (int i = 0; i < 16; i++) if (kT36[i] == (six & 63)) return i;
    return -1;
}

size_t airLength(uint8_t L, bool formatB) {
    const size_t n = (size_t)L + 1;
    if (formatB) return n;
    if (n <= 10) return n + 2;
    return n + 2 + 2 * ((n - 10 + 15) / 16);
}

std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& body, bool formatB) {
    std::vector<uint8_t> out, plain;
    auto putCrc = [&](const uint8_t* p, size_t n) { const uint16_t c = crc16(p, n); out.insert(out.end(), p, p + n); out.push_back((uint8_t)(c >> 8)); out.push_back((uint8_t)c); };
    if (!formatB) {
        plain.push_back((uint8_t)body.size());
        plain.insert(plain.end(), body.begin(), body.end());
        size_t pos = 0;
        size_t blk = std::min<size_t>(10, plain.size());
        putCrc(plain.data(), blk); pos = blk;
        while (pos < plain.size()) { blk = std::min<size_t>(16, plain.size() - pos); putCrc(plain.data() + pos, blk); pos += blk; }
    } else {
        const size_t ncrc = (1 + body.size() + 2 <= 128) ? 1 : 2;
        plain.push_back((uint8_t)(body.size() + ncrc * 2));
        plain.insert(plain.end(), body.begin(), body.end());
        if (ncrc == 1) putCrc(plain.data(), plain.size());
        else { putCrc(plain.data(), 126); putCrc(plain.data() + 126, plain.size() - 126); }
    }
    return out;
}

std::string mfrCodeToString(uint16_t m) {
    char s[4] = {(char)(((m >> 10) & 31) + 64), (char)(((m >> 5) & 31) + 64), (char)((m & 31) + 64), 0};
    return s;
}
uint16_t mfrStringToCode(const char* t) { return (uint16_t)((((t[0] - 64) & 31) << 10) | (((t[1] - 64) & 31) << 5) | ((t[2] - 64) & 31)); }

const char* deviceTypeName(uint8_t t) {
    switch (t) {
    case 0x00: return "other"; case 0x01: return "oil"; case 0x02: return "electricity"; case 0x03: return "gas";
    case 0x04: return "heat (outlet)"; case 0x05: return "steam"; case 0x06: return "warm water"; case 0x07: return "water";
    case 0x08: return "heat cost allocator"; case 0x09: return "compressed air"; case 0x0A: return "cooling (outlet)"; case 0x0B: return "cooling (inlet)";
    case 0x0C: return "heat (inlet)"; case 0x0D: return "heat / cooling"; case 0x0E: return "bus / system"; case 0x0F: return "unknown";
    case 0x15: return "hot water"; case 0x16: return "cold water"; case 0x17: return "dual water"; case 0x18: return "pressure";
    case 0x19: return "A/D converter"; case 0x1A: return "smoke detector"; case 0x1B: return "room sensor"; case 0x1C: return "gas detector";
    case 0x20: return "breaker (electricity)"; case 0x21: return "valve (gas or water)"; case 0x25: return "customer unit"; case 0x28: return "waste water";
    case 0x29: return "garbage"; case 0x2A: return "carbon dioxide"; case 0x31: return "communication controller"; case 0x32: return "unidirectional repeater";
    case 0x33: return "bidirectional repeater"; case 0x36: return "radio converter (system)"; case 0x37: return "radio converter (meter)";
    default: return "reserved";
    }
}

std::string bcdId(const uint8_t* b) {
    char s[9];
    for (int i = 0; i < 4; i++) { s[2 * i] = (char)('0' + ((b[3 - i] >> 4) & 15) % 10); s[2 * i + 1] = (char)('0' + (b[3 - i] & 15) % 10); }
    s[8] = 0;
    return s;
}

namespace {
const char* cName(uint8_t c) {
    switch (c) {
    case 0x44: return "SND-NR (send, no reply)"; case 0x46: return "SND-IR (installation)"; case 0x47: return "ACC-NR (access)"; case 0x48: return "ACC-DMD (demand)";
    case 0x08: return "RSP-UD (response)"; case 0x40: return "SND-NKE"; case 0x43: case 0x53: case 0x73: return "SND-UD (send user data)";
    case 0x5A: case 0x7A: return "REQ-UD1"; case 0x5B: case 0x7B: return "REQ-UD2"; default: return "other";
    }
}
const char* ciName(uint8_t ci) {
    switch (ci) {
    case 0x72: return "long header, variable data"; case 0x7A: return "short header, variable data"; case 0x78: return "variable data, no header";
    case 0x7B: return "compact data, short header"; case 0x79: return "compact data, no header"; case 0x8C: return "extended link layer (short)";
    case 0x8D: return "extended link layer (with payload CRC)"; case 0x51: return "data send"; case 0x52: return "selection of slaves";
    case 0x70: return "application error"; case 0x71: return "alarm"; case 0x69: case 0x6A: case 0x6B: return "reduced format"; case 0x90: return "authentication / fragmentation";
    default: return (ci >= 0xA0 && ci <= 0xB7) ? "manufacturer specific" : "other";
    }
}

struct Vif { const char* name; const char* unit; int exp; };

bool vifLookup(int v, Vif& o) {
    v &= 0x7F;
    const int n = v & 7;
    if (v <= 0x07) { o = {"Energy", "Wh", n - 3}; return true; }
    if (v <= 0x0F) { o = {"Energy", "J", n}; return true; }
    if (v <= 0x17) { o = {"Volume", "m3", n - 6}; return true; }
    if (v <= 0x1F) { o = {"Mass", "kg", n - 3}; return true; }
    if (v <= 0x23) { static const char* u[] = {"s", "min", "h", "d"}; o = {"On time", u[v & 3], 0}; return true; }
    if (v <= 0x27) { static const char* u[] = {"s", "min", "h", "d"}; o = {"Operating time", u[v & 3], 0}; return true; }
    if (v <= 0x2F) { o = {"Power", "W", n - 3}; return true; }
    if (v <= 0x37) { o = {"Power", "J/h", n}; return true; }
    if (v <= 0x3F) { o = {"Volume flow", "m3/h", n - 6}; return true; }
    if (v <= 0x47) { o = {"Volume flow", "m3/min", n - 7}; return true; }
    if (v <= 0x4F) { o = {"Volume flow", "m3/s", n - 9}; return true; }
    if (v <= 0x57) { o = {"Mass flow", "kg/h", n - 3}; return true; }
    if (v <= 0x5B) { o = {"Flow temperature", "C", (v & 3) - 3}; return true; }
    if (v <= 0x5F) { o = {"Return temperature", "C", (v & 3) - 3}; return true; }
    if (v <= 0x63) { o = {"Temperature difference", "K", (v & 3) - 3}; return true; }
    if (v <= 0x67) { o = {"External temperature", "C", (v & 3) - 3}; return true; }
    if (v <= 0x6B) { o = {"Pressure", "bar", (v & 3) - 3}; return true; }
    if (v == 0x6C) { o = {"Date", "", 0}; return true; }
    if (v == 0x6D) { o = {"Date and time", "", 0}; return true; }
    if (v == 0x6E) { o = {"Units for heat cost allocators", "", 0}; return true; }
    if (v == 0x6F) return false;
    if (v <= 0x73) { static const char* u[] = {"s", "min", "h", "d"}; o = {"Averaging duration", u[v & 3], 0}; return true; }
    if (v <= 0x77) { static const char* u[] = {"s", "min", "h", "d"}; o = {"Actuality duration", u[v & 3], 0}; return true; }
    if (v == 0x78) { o = {"Fabrication number", "", 0}; return true; }
    if (v == 0x79) { o = {"Enhanced identification", "", 0}; return true; }
    if (v == 0x7A) { o = {"Bus address", "", 0}; return true; }
    return false;
}

const char* fdName(int c) {
    switch (c & 0x7F) {
    case 0x08: return "Access number"; case 0x09: return "Medium"; case 0x0A: return "Manufacturer"; case 0x0B: return "Parameter set identification";
    case 0x0C: return "Model / version"; case 0x0D: return "Hardware version"; case 0x0E: return "Firmware version"; case 0x0F: return "Software version";
    case 0x10: return "Customer location"; case 0x11: return "Customer"; case 0x17: return "Error flags"; case 0x74: return "Remaining battery life (days)";
    default: return nullptr;
    }
}

int dataLen(int code) {
    static const int len[16] = {0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, -1, 6, 0};
    return len[code & 15];
}

std::string fmtNum(double v, int exp) {
    char b[64];
    const double s = v * std::pow(10.0, exp);
    if (exp < 0) snprintf(b, sizeof b, "%.*f", std::min(-exp, 9), s); else snprintf(b, sizeof b, "%.0f", s);
    return b;
}
} // namespace

bool parseRecords(const uint8_t* d, size_t n, std::vector<Record>& out, bool& mfrData) {
    size_t i = 0;
    mfrData = false;
    while (i < n) {
        const int dif = d[i++];
        if (dif == 0x2F) continue;                       // idle filler
        if (dif == 0x0F || dif == 0x1F || dif == 0x7F) { mfrData = true; return true; }
        Record r;
        r.dif = dif;
        r.func = (dif >> 4) & 3;
        r.storage = (dif >> 6) & 1;
        int sh = 1, ext = dif & 0x80;
        for (int k = 0; ext && k < 10; k++) {
            if (i >= n) return false;
            const int e = d[i++];
            if (sh <= 27) r.storage |= (e & 15) << sh;   // storage fits 41 bits; keep the int shift defined
            sh += 4;
            r.tariff |= ((e >> 4) & 3) << (2 * k);
            r.subunit |= ((e >> 6) & 1) << k;
            ext = e & 0x80;
        }
        if (i >= n) return false;
        int vif = d[i++];
        Vif vi{"", "", 0};
        std::string qty;
        bool known = false;
        if (vif == 0xFD || vif == 0xFB) {
            if (i >= n) return false;
            const int c = d[i++];
            const char* nm = vif == 0xFD ? fdName(c) : nullptr;
            char b[32]; snprintf(b, sizeof b, "VIF %02X %02X", vif, c);
            qty = nm ? nm : b; known = nm != nullptr;
            ext = c & 0x80;
        } else if (vif == 0x7C) {
            if (i >= n) return false;
            const size_t l = d[i++];
            if (i + l > n) return false;
            std::string s; for (size_t k = 0; k < l; k++) s += (char)d[i + l - 1 - k];
            i += l; qty = s; known = true; ext = 0;
        } else {
            known = vifLookup(vif, vi);
            if (known) qty = vi.name;
            else { char b[16]; snprintf(b, sizeof b, "VIF %02X", vif & 0x7F); qty = b; }
            ext = vif & 0x80;
            if ((vif & 0x7F) == 0x7F) { qty = "manufacturer specific"; known = true; }
        }
        for (int k = 0; ext && k < 10; k++) {   // VIFE: combinable extensions, skipped
            if (i >= n) return false;
            ext = d[i++] & 0x80;
        }
        r.quantity = qty;
        int len = dataLen(dif);
        int code = dif & 15;
        if (len < 0) {
            if (i >= n) return false;
            const int lv = d[i++];
            if (lv < 0xC0) { len = lv; code = 13; } else { mfrData = true; return true; }   // binary variable length: shown raw below
        }
        if (i + (size_t)len > n) return false;
        const uint8_t* p = d + i;
        i += (size_t)len;
        char b[96];
        if (code == 0 || code == 8 || code == 15) r.value = "-";
        else if (code == 13) { std::string s; for (int k = 0; k < len; k++) s += (char)(p[len - 1 - k] >= 32 && p[len - 1 - k] < 127 ? p[len - 1 - k] : '.'); r.value = s; }
        else if (code == 5) { float f; uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; memcpy(&f, &u, 4); r.number = f; r.numeric = true; r.value = fmtNum(f, vi.exp); }
        else if ((code >= 9 && code <= 12) || code == 14) {
            double v = 0; bool neg = false;
            for (int k = len - 1; k >= 0; k--) {
                int hi = p[k] >> 4, lo = p[k] & 15;
                if (k == len - 1 && hi == 0xF) { neg = true; hi = 0; }
                v = v * 100 + hi * 10 + lo;
            }
            if (neg) v = -v;
            r.number = v; r.numeric = true; r.value = fmtNum(v, vi.exp);
        } else {   // 1..4, 6, 7: two's complement integer
            uint64_t u = 0;
            for (int k = len - 1; k >= 0; k--) u = (u << 8) | p[k];
            int64_t sv = (int64_t)u;
            if (len < 8 && (u >> (8 * len - 1)) & 1) sv = (int64_t)(u - (1ULL << (8 * len)));
            const int vc = vif & 0x7F;
            if (vif == 0x6C && len == 2) {
                snprintf(b, sizeof b, "%04d-%02d-%02d", 2000 + (((p[0] & 0xE0) >> 5) | ((p[1] & 0xF0) >> 1)), p[1] & 15, p[0] & 31);
                r.value = b;
            } else if (vif == 0x6D && len == 4) {
                snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", 2000 + (((p[2] & 0xE0) >> 5) | ((p[3] & 0xF0) >> 1)), p[3] & 15, p[2] & 31, p[1] & 31, p[0] & 63);
                r.value = b;
            } else if (known && vc != 0x7F && vc != 0x7C) {
                r.number = (double)sv; r.numeric = true; r.value = fmtNum((double)sv, vi.exp);
            } else {
                snprintf(b, sizeof b, "%lld", (long long)sv); r.value = b; r.number = (double)sv; r.numeric = true;
            }
        }
        if (vi.unit && vi.unit[0] && r.value != "-") r.value += std::string(" ") + vi.unit;
        static const char* fn[] = {"", "max", "min", "error"};
        std::string note;
        if (r.storage) { snprintf(b, sizeof b, "storage %d", r.storage); note = b; }
        if (r.tariff) { snprintf(b, sizeof b, "%stariff %d", note.empty() ? "" : ", ", r.tariff); note += b; }
        if (r.subunit) { snprintf(b, sizeof b, "%ssubunit %d", note.empty() ? "" : ", ", r.subunit); note += b; }
        if (r.func) note += std::string(note.empty() ? "" : ", ") + fn[r.func];
        r.note = note;
        out.push_back(r);
    }
    return true;
}

std::string Frame::hex() const {
    std::string s; char b[4];
    for (uint8_t c : data) { snprintf(b, sizeof b, "%02X", c); s += b; }
    return s;
}

bool parseFrame(const uint8_t* b, size_t n, bool formatB, Frame& f) {
    f = Frame();
    f.formatB = formatB;
    if (n < 3) { f.error = "too short"; return false; }
    f.L = b[0];
    if (n < airLength(f.L, formatB)) { f.error = "incomplete"; return false; }
    size_t pos = 0;
    bool ok = true;
    auto block = [&](size_t dataLen) {
        const uint16_t c = crc16(b + pos, dataLen);
        const uint16_t g = (uint16_t)((b[pos + dataLen] << 8) | b[pos + dataLen + 1]);
        if (c != g) ok = false;
        f.data.insert(f.data.end(), b + pos, b + pos + dataLen);
        pos += dataLen + 2;
    };
    const size_t N = (size_t)f.L + 1;
    if (!formatB) {
        size_t left = N;
        size_t blk = std::min<size_t>(10, left); block(blk); left -= blk;
        while (left) { blk = std::min<size_t>(16, left); block(blk); left -= blk; }
    } else {
        if (N < 3 || (N > 128 && N < 131)) { f.error = "bad length"; return false; }
        if (N <= 128) block(N - 2);
        else { block(126); block(N - 128 - 2); }
    }
    f.crcOk = ok;
    if (f.data.size() < 11) { f.error = "too short for a link layer"; return false; }
    f.C = f.data[1];
    f.cName = cName(f.C);
    f.mfrCode = (uint16_t)(f.data[2] | f.data[3] << 8);
    f.mfr = mfrCodeToString(f.mfrCode);
    f.id = bcdId(&f.data[4]);
    f.version = f.data[8];
    f.type = f.data[9];
    f.typeName = deviceTypeName(f.type);
    f.ci = f.data[10];
    f.ciName = ciName(f.ci);
    size_t p = 11;
    int ci = f.ci;
    const size_t sz = f.data.size();
    if (ci == 0x8C) { if (p + 3 > sz) return ok; p += 2; ci = f.data[p++]; }
    else if (ci == 0x8D) {
        if (p + 9 > sz) return ok;
        // the session number is 4 bytes little-endian after CC and ACC; ENC is its bits 29-31
        if ((f.data[p + 5] >> 5) & 7) { f.encrypted = true; f.payload.assign(f.data.begin() + (long)p, f.data.end()); return ok; }
        p += 8; ci = f.data[p++];
    }
    if (ci == 0x7A || ci == 0x72 || ci == 0x7B) {
        size_t hl = ci == 0x72 ? 12 : 4;
        if (ci == 0x7B) hl = 4;
        if (p + hl > sz) return ok;
        if (ci == 0x72) { f.longHeader = true; p += 8; } else f.shortHeader = true;
        f.acc = f.data[p]; f.status = f.data[p + 1];
        f.config = (uint16_t)(f.data[p + 2] | f.data[p + 3] << 8);
        p += 4;
        const int st = f.status & 3;
        f.statusText = st == 0 ? "ok" : st == 1 ? "busy" : st == 2 ? "application error" : "alarm";
        if (f.status & 4) f.statusText += ", low power";
        if (f.status & 8) f.statusText += ", permanent error";
        if (f.status & 16) f.statusText += ", temporary error";
        if (((f.config >> 8) & 0x1F) != 0) f.encrypted = true;
    }
    f.payload.assign(f.data.begin() + (long)p, f.data.end());
    if (!f.encrypted && (ci == 0x7A || ci == 0x72 || ci == 0x78)) {
        if (!parseRecords(f.payload.data(), f.payload.size(), f.records, f.hasManufacturerData)) f.error = "record cut off";
    }
    return ok;
}

} // namespace dect2::wmbus

namespace dect2::wmbus {

const PhySpec& phySpec(Phy p) {
    static const PhySpec t[] = {
        {Phy::S1, 32768, 50000, Coding::Manchester, 0x7696},
        {Phy::T1, 100000, 50000, Coding::ThreeOfSix, 0x543D},
        {Phy::C1A, 100000, 50000, Coding::Nrz, 0x543D},
        {Phy::C1B, 100000, 50000, Coding::Nrz, 0x543D},
        {Phy::R2, 4800, 6000, Coding::Manchester, 0x7696},
        {Phy::N1, 4800, 2400, Coding::Nrz, 0xF68D},
    };
    return t[(int)p < 6 ? (int)p : 0];
}

std::vector<uint8_t> encodeChips(Phy p, const std::vector<uint8_t>& air, int pre) {
    const PhySpec& sp = phySpec(p);
    std::vector<uint8_t> c;
    for (int i = 0; i < pre; i++) { c.push_back(0); c.push_back(1); }
    for (int i = 15; i >= 0; i--) c.push_back((sp.sync >> i) & 1);
    auto nrz = [&](unsigned v, int bits) { for (int i = bits - 1; i >= 0; i--) c.push_back((v >> i) & 1); };
    if (p == Phy::C1A) nrz(0x54CD, 16);   // mode C sends 0x54 then the frame-format byte
    if (p == Phy::C1B) nrz(0x543D, 16);
    for (uint8_t b : air) {
        if (sp.coding == Coding::Manchester) { for (int i = 7; i >= 0; i--) { const int bit = (b >> i) & 1; c.push_back((uint8_t)bit); c.push_back((uint8_t)!bit); } }
        else if (sp.coding == Coding::ThreeOfSix) { nrz((unsigned)encode36(b >> 4), 6); nrz((unsigned)encode36(b & 15), 6); }
        else nrz(b, 8);
    }
    for (int i = 0; i < 4; i++) { c.push_back(0); c.push_back(1); }   // a short tail
    return c;
}

std::vector<ChannelDef> channelPlan(double centre, double rate) {
    std::vector<ChannelDef> all;
    auto lab = [](double f) { char b[40]; snprintf(b, sizeof b, "%.4f MHz", f / 1e6); return std::string(b); };
    if (centre > 0 && centre < 300e6) {
        for (int i = 0; i < 6; i++) { const double f = 169.40625e6 + 12.5e3 * i; all.push_back({f, false, 48000, 5500, {Phy::N1}, lab(f)}); }
    } else {
        all.push_back({868.300e6, true, 400000, 140000, {Phy::S1}, lab(868.3e6)});
        all.push_back({868.950e6, true, 400000, 140000, {Phy::T1, Phy::C1A}, lab(868.95e6)});
        for (double f : {868.030e6, 868.090e6, 868.150e6}) all.push_back({f, false, 48000, 14000, {Phy::R2}, lab(f)});
    }
    if (centre <= 0) centre = all.empty() ? 868.625e6 : (all[0].freqHz < 300e6 ? 169.4375e6 : 868.625e6);
    std::vector<ChannelDef> out;
    for (const auto& c : all) {
        const double half = c.wide ? 150e3 : 12e3;
        if (std::fabs(c.freqHz - centre) + half <= 0.46 * rate) out.push_back(c);
    }
    return out;
}

} // namespace dect2::wmbus
