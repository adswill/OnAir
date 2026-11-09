// Bluetooth LE advertising PDUs and advertising data (see ble_proto.h).
#include "dect2/ble/ble_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/bits.h"
#include "dect2/common/str.h"

namespace dect2 {
namespace ble {

double channelMhz(int ch) {
    if (ch == 37) return 2402;
    if (ch == 38) return 2426;
    if (ch == 39) return 2480;
    if (ch >= 0 && ch <= 10) return 2404 + 2.0 * ch;
    if (ch >= 11 && ch <= 36) return 2428 + 2.0 * (ch - 11);
    return 0;
}

int advChannelOf(double mhz) {
    for (int c = 37; c <= 39; c++) if (std::fabs(mhz - channelMhz(c)) < 0.1) return c;
    return 0;
}

// The LFSR of vol 6 part B 3.1.1 held reversed: bit 0 is position 23 (the bit that goes out first), so the init value is bit-reversed
uint32_t crc24(const uint8_t* d, size_t n, uint32_t init) {
    uint32_t s = 0;
    for (int i = 0; i < 24; i++) if (init >> i & 1) s |= 1u << (23 - i);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        for (int j = 0; j < 8; j++) {
            const uint32_t nb = (s ^ c) & 1;
            c >>= 1;
            s >>= 1;
            if (nb) { s |= 1u << 23; s ^= 0x5A6000; }
        }
    }
    return s;
}

static uint8_t rev8(uint8_t v) { return dect2::bits::reverse8(v); }

// x^7 + x^4 + 1, position 0 set to one and positions 1 to 6 to the channel index (MSB first); held shifted left by one
void whiten(uint8_t* d, size_t n, int channel) {
    uint8_t w = (uint8_t)(rev8((uint8_t)channel) | 2);
    for (size_t i = 0; i < n; i++)
        for (int m = 1; m < 256; m <<= 1) {
            if (w & 0x80) { w ^= 0x11; d[i] ^= (uint8_t)m; }
            w = (uint8_t)(w << 1);
        }
}

const char* pduTypeName(int t) {
    static const char* const n[] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_NONCONN_IND", "SCAN_REQ", "SCAN_RSP", "CONNECT_IND", "ADV_SCAN_IND", "ADV_EXT_IND"};
    return t >= 0 && t < 8 ? n[t] : "reserved";
}

const char* pduTypeText(int t) {
    static const char* const n[] = {"connectable", "directed", "not connectable", "scan request", "scan response", "connection request", "scannable", "extended"};
    return t >= 0 && t < 8 ? n[t] : "reserved";
}

const char* companyName(int id) {
    struct C { int id; const char* n; };
    static const C t[] = {
        {0x0000, "Ericsson"}, {0x0001, "Nokia"}, {0x0002, "Intel"}, {0x0003, "IBM"}, {0x0004, "Toshiba"}, {0x0006, "Microsoft"}, {0x0008, "Motorola"},
        {0x000D, "Texas Instruments"}, {0x000F, "Broadcom"}, {0x001D, "Qualcomm"}, {0x0030, "STMicroelectronics"}, {0x004C, "Apple"}, {0x0059, "Nordic Semiconductor"},
        {0x0075, "Samsung"}, {0x0087, "Garmin"}, {0x009E, "Bose"}, {0x00C4, "LG Electronics"}, {0x00E0, "Google"}, {0x012D, "Sony"}, {0x0171, "Amazon"},
        {0x027D, "Huawei"}, {0x02E5, "Espressif"}, {0x038F, "Xiaomi"}, {0x0499, "Ruuvi Innovations"}, {0xFFFF, "test / no company"},
    };
    for (const C& c : t) if (c.id == id) return c.n;
    return nullptr;
}

const char* uuid16Name(int u) {
    struct U { int u; const char* n; };
    static const U t[] = {
        {0x1800, "Generic Access"}, {0x1801, "Generic Attribute"}, {0x1802, "Immediate Alert"}, {0x1803, "Link Loss"}, {0x1804, "Tx Power"},
        {0x1805, "Current Time"}, {0x1808, "Glucose"}, {0x1809, "Health Thermometer"}, {0x180A, "Device Information"}, {0x180D, "Heart Rate"},
        {0x180F, "Battery"}, {0x1810, "Blood Pressure"}, {0x1812, "Human Interface Device"}, {0x1814, "Running Speed and Cadence"},
        {0x1816, "Cycling Speed and Cadence"}, {0x1818, "Cycling Power"}, {0x181A, "Environmental Sensing"}, {0x181B, "Body Composition"},
        {0x181C, "User Data"}, {0x181D, "Weight Scale"}, {0x1826, "Fitness Machine"},
        {0xFD6F, "Exposure Notification"}, {0xFE2C, "Google Fast Pair"}, {0xFE95, "Xiaomi"}, {0xFE9F, "Google"}, {0xFEAA, "Eddystone"}, {0xFEED, "Tile"},
    };
    for (const U& x : t) if (x.u == u) return x.n;
    return nullptr;
}

const char* appleTypeName(int t) {
    switch (t) {
    case 0x02: return "iBeacon";
    case 0x05: return "AirDrop";
    case 0x07: return "AirPods / Beats";
    case 0x09: return "AirPlay target";
    case 0x0C: return "Handoff";
    case 0x0F: return "Nearby Action";
    case 0x10: return "Nearby Info";
    case 0x12: return "Find My";
    default: return nullptr;
    }
}

int addrKind(bool random, const uint8_t a[6]) {
    if (!random) return 0;
    switch (a[5] >> 6) { case 3: return 1; case 1: return 2; case 0: return 3; default: return 4; }
}

const char* addrKindName(int k) {
    static const char* const n[] = {"public", "random static", "resolvable private", "non-resolvable private", "random"};
    return k >= 0 && k < 5 ? n[k] : "?";
}

std::string addrText(const uint8_t a[6]) {
    char b[24];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return b;
}

static std::string hex(const uint8_t* d, size_t n) { return dect2::str::toHex(d, n); }

static std::string uuid128(const uint8_t* d) {   // little-endian on the air
    char b[48];
    snprintf(b, sizeof b, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", d[15], d[14], d[13], d[12], d[11], d[10], d[9], d[8], d[7], d[6],
             d[5], d[4], d[3], d[2], d[1], d[0]);
    return b;
}

static std::string uuidBigEndian(const uint8_t* d) {   // iBeacon sends its UUID most significant byte first
    char b[48];
    snprintf(b, sizeof b, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9],
             d[10], d[11], d[12], d[13], d[14], d[15]);
    return b;
}

static std::string svc16(int u) {
    char b[64];
    const char* n = uuid16Name(u);
    if (n) snprintf(b, sizeof b, "%s (0x%04X)", n, u); else snprintf(b, sizeof b, "0x%04X", u);
    return b;
}

std::string eddystoneUrl(const uint8_t* d, size_t n) {
    static const char* const scheme[] = {"http://www.", "https://www.", "http://", "https://"};
    static const char* const exp[] = {".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/", ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"};
    if (n < 1) return "";
    std::string s = d[0] < 4 ? scheme[d[0]] : "?";
    for (size_t i = 1; i < n; i++) {
        if (d[i] < 14) s += exp[d[i]];
        else if (d[i] > 0x20 && d[i] < 0x7F) s += (char)d[i];
        else s += '?';
    }
    return s;
}

static void addBeacon(Adv& o, const std::string& s) { if (!o.beacon.empty()) o.beacon += "; "; o.beacon += s; }

// ----------------------------------------------------- sensor payloads in the clear
namespace {
struct BtHomeObj { uint8_t id; const char* name; int bytes; bool sgn; double factor; const char* unit; };
const BtHomeObj kBtHome[] = {   // bthome.io/format v2
    {0x51, "acceleration", 2, false, 0.001, "m/s2"}, {0x01, "battery", 1, false, 1, "%"}, {0x60, "channel", 1, false, 1, ""},
    {0x12, "CO2", 2, false, 1, "ppm"}, {0x56, "conductivity", 2, false, 1, "uS/cm"}, {0x09, "count", 1, false, 1, ""},
    {0x3D, "count", 2, false, 1, ""}, {0x3E, "count", 4, false, 1, ""}, {0x59, "count", 1, true, 1, ""}, {0x5A, "count", 2, true, 1, ""}, {0x5B, "count", 4, true, 1, ""},
    {0x43, "current", 2, false, 0.001, "A"}, {0x5D, "current", 2, true, 0.001, "A"}, {0x08, "dewpoint", 2, true, 0.01, "C"}, {0x5E, "direction", 2, false, 0.01, "deg"},
    {0x40, "distance", 2, false, 1, "mm"}, {0x41, "distance", 2, false, 0.1, "m"}, {0x42, "duration", 3, false, 0.001, "s"}, {0x4D, "energy", 4, false, 0.001, "kWh"}, {0x0A, "energy", 3, false, 0.001, "kWh"},
    {0x4B, "gas", 3, false, 0.001, "m3"}, {0x4C, "gas", 4, false, 0.001, "m3"}, {0x52, "gyroscope", 2, false, 0.001, "deg/s"}, {0x03, "humidity", 2, false, 0.01, "%"}, {0x2E, "humidity", 1, false, 1, "%"},
    {0x05, "illuminance", 3, false, 0.01, "lx"}, {0x64, "light level", 1, false, 1, ""}, {0x06, "mass", 2, false, 0.01, "kg"}, {0x07, "mass", 2, false, 0.01, "lb"},
    {0x14, "moisture", 2, false, 0.01, "%"}, {0x2F, "moisture", 1, false, 1, "%"}, {0x0D, "PM2.5", 2, false, 1, "ug/m3"}, {0x0E, "PM10", 2, false, 1, "ug/m3"},
    {0x0B, "power", 3, false, 0.01, "W"}, {0x5C, "power", 4, true, 0.01, "W"}, {0x5F, "precipitation", 2, false, 0.1, "mm"}, {0x04, "pressure", 3, false, 0.01, "hPa"},
    {0x3F, "rotation", 2, true, 0.1, "deg"}, {0x61, "rotational speed", 2, false, 1, "rpm"}, {0x44, "speed", 2, false, 0.01, "m/s"},
    {0x57, "temperature", 1, true, 1, "C"}, {0x58, "temperature", 1, true, 0.35, "C"}, {0x45, "temperature", 2, true, 0.1, "C"}, {0x02, "temperature", 2, true, 0.01, "C"},
    {0x50, "timestamp", 4, false, 1, ""}, {0x13, "TVOC", 2, false, 1, "ug/m3"}, {0x0C, "voltage", 2, false, 0.001, "V"}, {0x4A, "voltage", 2, false, 0.1, "V"},
    {0x4E, "volume", 4, false, 0.001, "L"}, {0x47, "volume", 2, false, 0.1, "L"}, {0x48, "volume", 2, false, 1, "mL"}, {0x55, "volume storage", 4, false, 0.001, "L"},
    {0x49, "flow", 2, false, 0.001, "m3/h"}, {0x46, "UV index", 1, false, 0.1, ""}, {0x4F, "water", 4, false, 0.001, "L"},
};

std::string numFmt(double v, const char* unit) {
    char b[48];
    if (std::fabs(v - std::lround(v)) < 1e-6) snprintf(b, sizeof b, "%.0f %s", v, unit); else snprintf(b, sizeof b, "%.2f %s", v, unit);
    std::string s = b;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool btHome(const uint8_t* d, size_t n, std::string& out) {
    if (n < 1) return false;
    if ((d[0] >> 5) != 2) return false;              // v2 only
    if (d[0] & 1) { out = "BTHome (encrypted)"; return true; }
    out = "BTHome";
    size_t i = 1;
    while (i < n) {
        const uint8_t id = d[i++];
        if (id == 0x53 || id == 0x54) {              // text / raw with a length byte
            if (i >= n) return true;
            const size_t len = d[i++];
            if (i + len > n) return true;
            if (id == 0x53) { std::string t((const char*)d + i, len); out += ", text " + t; }
            i += len;
            continue;
        }
        const BtHomeObj* f = nullptr;
        for (const auto& k : kBtHome) if (k.id == id) { f = &k; break; }
        if (!f || i + (size_t)f->bytes > n) return true;      // the spec: stop at the first object not understood
        uint32_t raw = 0;
        for (int b = 0; b < f->bytes; b++) raw |= (uint32_t)d[i + (size_t)b] << (8 * b);
        i += (size_t)f->bytes;
        double v = raw;
        if (f->sgn) { const uint32_t bit = 1u << (f->bytes * 8 - 1); v = (double)(int32_t)((raw ^ bit) - bit); }
        out += std::string(", ") + f->name + " " + numFmt(v * f->factor, f->unit);
    }
    return true;
}

bool pvvx(const uint8_t* d, size_t n, std::string& out) {
    if (n != 15) return false;                       // MAC, temperature, humidity, mV, %, counter, flags
    const double t = (int16_t)(d[6] | d[7] << 8) / 100.0, h = (uint16_t)(d[8] | d[9] << 8) / 100.0;
    const int mv = d[10] | d[11] << 8, pc = d[12];
    char b[120];
    snprintf(b, sizeof b, "ATC/pvvx sensor: %.2f C, %.2f %%, battery %d mV (%d %%)", t, h, mv, pc);
    out = b;
    return true;
}

bool ruuvi5(const uint8_t* d, size_t n, std::string& out) {
    if (n < 18 || d[0] != 5) return false;
    char b[200];
    std::string s = "RuuviTag";
    const int16_t ti = (int16_t)(d[1] << 8 | d[2]);
    if ((uint16_t)ti != 0x8000) { snprintf(b, sizeof b, ", %.2f C", ti * 0.005); s += b; }
    const uint16_t hu = (uint16_t)(d[3] << 8 | d[4]);
    if (hu != 0xFFFF) { snprintf(b, sizeof b, ", %.2f %%", hu * 0.0025); s += b; }
    const uint16_t pr = (uint16_t)(d[5] << 8 | d[6]);
    if (pr != 0xFFFF) { snprintf(b, sizeof b, ", %.2f hPa", (pr + 50000) / 100.0); s += b; }
    const uint16_t pw = (uint16_t)(d[13] << 8 | d[14]);
    if ((pw >> 5) != 2047) { snprintf(b, sizeof b, ", battery %d mV", (pw >> 5) + 1600); s += b; }
    out = s;
    return true;
}
} // namespace

static void serviceData(Adv& o, int u, const uint8_t* d, size_t n) {
    char b[200];
    std::string sens;
    if (u == 0xFCD2 && btHome(d, n, sens)) { addBeacon(o, sens); return; }
    if (u == 0x181A && pvvx(d, n, sens)) { addBeacon(o, sens); return; }
    if (u == 0xFEAA && n >= 1) {   // Eddystone
        if (d[0] == 0x00 && n >= 18) {
            snprintf(b, sizeof b, "Eddystone UID %s / %s, %d dBm at 0 m", hex(d + 2, 10).c_str(), hex(d + 12, 6).c_str(), (int8_t)d[1]);
            addBeacon(o, b);
        } else if (d[0] == 0x10 && n >= 3) {
            snprintf(b, sizeof b, "Eddystone URL %s, %d dBm at 0 m", eddystoneUrl(d + 2, n - 2).c_str(), (int8_t)d[1]);
            addBeacon(o, b);
        } else if (d[0] == 0x20 && n >= 14 && d[1] == 0) {
            const int mv = d[2] << 8 | d[3];
            const double t = (int16_t)(d[4] << 8 | d[5]) / 256.0;
            const uint32_t cnt = (uint32_t)d[6] << 24 | (uint32_t)d[7] << 16 | (uint32_t)d[8] << 8 | d[9];
            const uint32_t sec = (uint32_t)d[10] << 24 | (uint32_t)d[11] << 16 | (uint32_t)d[12] << 8 | d[13];
            snprintf(b, sizeof b, "Eddystone TLM %d mV, %.1f C, %u adverts, up %.0f s", mv, t, cnt, sec / 10.0);
            addBeacon(o, b);
        } else if (d[0] == 0x30) addBeacon(o, "Eddystone EID");
        else addBeacon(o, "Eddystone");
    } else if (u == 0xFD6F && n >= 16) {   // exposure notification: a rolling proximity identifier, changed every 10 to 20 minutes
        snprintf(b, sizeof b, "Exposure Notification, RPI %s", hex(d, 16).c_str());
        addBeacon(o, b);
    }
}

static void manufacturer(Adv& o, const uint8_t* d, size_t n) {
    if (n < 2) return;
    o.company = d[0] | d[1] << 8;
    o.mfr.assign(d + 2, d + n);
    const char* c = companyName(o.company);
    char b[200];
    if (c) o.vendor = c; else { snprintf(b, sizeof b, "company 0x%04X", o.company); o.vendor = b; }
    { std::string sens; if (o.company == 0x0499 && ruuvi5(d + 2, n - 2, sens)) addBeacon(o, sens); }
    if (o.company == 0x004C && n >= 3) {
        if (d[2] == 0x02 && n >= 25 && d[3] == 0x15) {
            snprintf(b, sizeof b, "iBeacon %s major %d minor %d, %d dBm at 1 m", uuidBigEndian(d + 4).c_str(), d[20] << 8 | d[21], d[22] << 8 | d[23], (int8_t)d[24]);
            addBeacon(o, b);
            o.vendor = "Apple: iBeacon";
        } else if (const char* t = appleTypeName(d[2])) o.vendor = std::string("Apple: ") + t;
    }
}

void parseAd(const uint8_t* d, size_t n, Adv& o) {
    size_t i = 0;
    while (i < n) {
        const size_t len = d[i];
        if (len == 0) break;                 // the rest is padding
        if (i + 1 + len > n) break;          // truncated
        const int type = d[i + 1];
        const uint8_t* v = d + i + 2;
        const size_t m = len - 1;
        switch (type) {
        case 0x01: if (m >= 1) o.flags = v[0]; break;
        case 0x02: case 0x03: for (size_t k = 0; k + 2 <= m; k += 2) o.services.push_back(svc16(v[k] | v[k + 1] << 8)); break;
        case 0x04: case 0x05:
            for (size_t k = 0; k + 4 <= m; k += 4) { char b[16]; snprintf(b, sizeof b, "0x%08X", (unsigned)(v[k] | v[k + 1] << 8 | v[k + 2] << 16 | (uint32_t)v[k + 3] << 24)); o.services.push_back(b); }
            break;
        case 0x06: case 0x07: for (size_t k = 0; k + 16 <= m; k += 16) o.services.push_back(uuid128(v + k)); break;
        case 0x08: case 0x09:
            if (o.name.empty() || type == 0x09) {
                o.name.clear();
                for (size_t k = 0; k < m; k++) o.name += (v[k] >= 0x20 && v[k] != 0x7F) ? (char)v[k] : '?';
                o.nameComplete = type == 0x09;
            }
            break;
        case 0x0A: if (m >= 1) o.txPower = (int8_t)v[0]; break;
        case 0x16: if (m >= 2) { const int u = v[0] | v[1] << 8; bool have = false; for (auto& s : o.services) if (s == svc16(u)) have = true; if (!have) o.services.push_back(svc16(u)); serviceData(o, u, v + 2, m - 2); } break;
        case 0x19: if (m >= 2) o.appearance = v[0] | v[1] << 8; break;
        case 0xFF: manufacturer(o, v, m); break;
        default: break;
        }
        i += 1 + len;
    }
}

static void put6(uint8_t dst[6], const uint8_t* s) { memcpy(dst, s, 6); }

bool parsePdu(const uint8_t* p, size_t n, Adv& o) {
    if (n < 2) return false;
    o = Adv();
    o.type = p[0] & 0x0F;
    o.chSel = p[0] >> 5 & 1;
    o.txRandom = p[0] >> 6 & 1;
    o.rxRandom = p[0] >> 7 & 1;
    o.len = p[1];
    if ((size_t)o.len + 2 > n) return false;
    const uint8_t* q = p + 2;
    const size_t L = (size_t)o.len;
    char b[240];
    switch (o.type) {
    case AdvInd: case AdvNonconnInd: case AdvScanInd: case ScanRsp:
        if (L < 6 || L > 37) return false;
        put6(o.adv, q); o.hasAdv = true;
        o.data.assign(q + 6, q + L);
        parseAd(o.data.data(), o.data.size(), o);
        break;
    case AdvDirectInd:
        if (L != 12) return false;
        put6(o.adv, q); put6(o.peer, q + 6); o.hasAdv = o.hasPeer = true;
        break;
    case ScanReq:
        if (L != 12) return false;
        put6(o.peer, q); put6(o.adv, q + 6); o.hasAdv = o.hasPeer = true;
        break;
    case ConnectInd:
        if (L != 34) return false;
        put6(o.peer, q); put6(o.adv, q + 6); o.hasAdv = o.hasPeer = true;
        o.connAa = (uint32_t)q[12] | (uint32_t)q[13] << 8 | (uint32_t)q[14] << 16 | (uint32_t)q[15] << 24;
        o.connInterval = q[22] | q[23] << 8;
        break;
    case AdvExtInd: {
        if (L < 1) return false;
        const int hl = q[0] & 0x3F;
        o.advMode = q[0] >> 6;
        if ((size_t)hl + 1 > L) return false;
        if (hl > 0) {
            const int fl = q[1];
            size_t k = 2;
            const size_t end = 1 + (size_t)hl;
            auto room = [&](size_t m) { return k + m <= end; };
            if (fl & 0x01) { if (!room(6)) return false; put6(o.adv, q + k); o.hasAdv = true; k += 6; }
            if (fl & 0x02) { if (!room(6)) return false; put6(o.peer, q + k); o.hasPeer = true; k += 6; }
            if (fl & 0x04) { if (!room(1)) return false; k += 1; }
            if (fl & 0x08) { if (!room(2)) return false; const int adi = q[k] | q[k + 1] << 8; o.adiDid = adi & 0x0FFF; o.adiSid = adi >> 12; k += 2; }
            if (fl & 0x10) { if (!room(3)) return false; o.auxChannel = q[k] & 0x3F; o.auxPhy = (q[k + 2] >> 5) & 7; k += 3; }
            if (fl & 0x20) { if (!room(18)) return false; k += 18; }
            if (fl & 0x40) { if (!room(1)) return false; o.txPower = (int8_t)q[k]; k += 1; }
        }
        o.data.assign(q + 1 + hl, q + L);
        parseAd(o.data.data(), o.data.size(), o);
        break;
    }
    default: return false;
    }
    // the log line
    std::string s;
    if (o.type == ScanReq) { snprintf(b, sizeof b, "scan request from %s", addrText(o.peer).c_str()); s = b; }
    else if (o.type == ConnectInd) { snprintf(b, sizeof b, "connection request from %s (not followed)", addrText(o.peer).c_str()); s = b; }
    else if (o.type == AdvDirectInd) { snprintf(b, sizeof b, "directed to %s", addrText(o.peer).c_str()); s = b; }
    else if (o.type == AdvExtInd) {
        static const char* const phy[] = {"1M", "2M", "Coded", "?", "?", "?", "?", "?"};
        s = "extended";
        if (o.auxChannel >= 0) { snprintf(b, sizeof b, ", data on channel %d (%s PHY, not followed)", o.auxChannel, phy[o.auxPhy & 7]); s += b; }
        if (o.adiSid >= 0) { snprintf(b, sizeof b, ", set %d", o.adiSid); s += b; }
    }
    auto addPart = [&](const std::string& x) { if (x.empty()) return; if (!s.empty()) s += ", "; s += x; };
    if (!o.name.empty()) addPart("\"" + o.name + "\"");
    if (!o.vendor.empty()) addPart(o.vendor);
    if (!o.beacon.empty()) addPart(o.beacon);
    if (o.txPower != 127) addPart("tx " + std::to_string(o.txPower) + " dBm");
    for (size_t i = 0; i < o.services.size() && i < 3; i++) addPart(o.services[i]);
    o.summary = s;
    return true;
}

// ---------------------------------------------------------------- building

AdBuilder& AdBuilder::add(int type, const std::vector<uint8_t>& v) {
    b.push_back((uint8_t)(v.size() + 1));
    b.push_back((uint8_t)type);
    b.insert(b.end(), v.begin(), v.end());
    return *this;
}

AdBuilder& AdBuilder::name(const std::string& s, bool complete) { return add(complete ? 0x09 : 0x08, std::vector<uint8_t>(s.begin(), s.end())); }

AdBuilder& AdBuilder::uuid16(const std::vector<int>& u) {
    std::vector<uint8_t> v;
    for (int x : u) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
    return add(0x03, v);
}

AdBuilder& AdBuilder::mfr(int company, const std::vector<uint8_t>& v) {
    std::vector<uint8_t> w = {(uint8_t)company, (uint8_t)(company >> 8)};
    w.insert(w.end(), v.begin(), v.end());
    return add(0xFF, w);
}

AdBuilder& AdBuilder::serviceData16(int uuid, const std::vector<uint8_t>& v) {
    std::vector<uint8_t> w = {(uint8_t)uuid, (uint8_t)(uuid >> 8)};
    w.insert(w.end(), v.begin(), v.end());
    return add(0x16, w);
}

AdBuilder& AdBuilder::ibeacon(const uint8_t uuid[16], int major, int minor, int power) {
    std::vector<uint8_t> v = {0x02, 0x15};
    v.insert(v.end(), uuid, uuid + 16);
    v.push_back((uint8_t)(major >> 8)); v.push_back((uint8_t)major);
    v.push_back((uint8_t)(minor >> 8)); v.push_back((uint8_t)minor);
    v.push_back((uint8_t)(int8_t)power);
    return mfr(0x004C, v);
}

AdBuilder& AdBuilder::eddystoneUrl(int power, int scheme, const std::string& encoded) {
    std::vector<uint8_t> v = {0x10, (uint8_t)(int8_t)power, (uint8_t)scheme};
    v.insert(v.end(), encoded.begin(), encoded.end());
    uuid16({0xFEAA});
    return serviceData16(0xFEAA, v);
}

AdBuilder& AdBuilder::eddystoneUid(int power, const uint8_t ns[10], const uint8_t inst[6]) {
    std::vector<uint8_t> v = {0x00, (uint8_t)(int8_t)power};
    v.insert(v.end(), ns, ns + 10);
    v.insert(v.end(), inst, inst + 6);
    v.push_back(0); v.push_back(0);
    uuid16({0xFEAA});
    return serviceData16(0xFEAA, v);
}

AdBuilder& AdBuilder::eddystoneTlm(int mv, double tempC, uint32_t advCount, uint32_t secTenths) {
    const int16_t t = (int16_t)std::lround(tempC * 256);
    std::vector<uint8_t> v = {0x20, 0x00, (uint8_t)(mv >> 8), (uint8_t)mv, (uint8_t)((uint16_t)t >> 8), (uint8_t)t,
                              (uint8_t)(advCount >> 24), (uint8_t)(advCount >> 16), (uint8_t)(advCount >> 8), (uint8_t)advCount,
                              (uint8_t)(secTenths >> 24), (uint8_t)(secTenths >> 16), (uint8_t)(secTenths >> 8), (uint8_t)secTenths};
    uuid16({0xFEAA});
    return serviceData16(0xFEAA, v);
}

AdBuilder& AdBuilder::exposure(const uint8_t rpi[16], const uint8_t meta[4]) {
    std::vector<uint8_t> v(rpi, rpi + 16);
    v.insert(v.end(), meta, meta + 4);
    flags(0x1A);
    uuid16({0xFD6F});
    return serviceData16(0xFD6F, v);
}

std::vector<uint8_t> makePdu(int type, bool txRandom, const std::vector<uint8_t>& payload, bool rxRandom) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)((type & 0x0F) | (txRandom ? 0x40 : 0) | (rxRandom ? 0x80 : 0)));
    p.push_back((uint8_t)payload.size());
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

std::vector<uint8_t> advPayload(const uint8_t adv[6], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> v(adv, adv + 6);
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

void packetBits(const std::vector<uint8_t>& pdu, int channel, std::vector<uint8_t>& bits) {
    std::vector<uint8_t> air = pdu;
    const uint32_t c = crc24(pdu.data(), pdu.size());
    air.push_back((uint8_t)c); air.push_back((uint8_t)(c >> 8)); air.push_back((uint8_t)(c >> 16));
    whiten(air.data(), air.size(), channel);
    bits.clear();
    const uint8_t pre = (kAdvAccess & 1) ? 0x55 : 0xAA;
    for (int i = 0; i < 8; i++) bits.push_back(pre >> i & 1);
    for (int i = 0; i < 32; i++) bits.push_back(kAdvAccess >> i & 1);
    for (uint8_t v : air) for (int i = 0; i < 8; i++) bits.push_back(v >> i & 1);
}

} // namespace ble
} // namespace dect2
