// Z-Wave frame layer, see zwave_proto.h
#include "dect2/zwave/zwave_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace zwave {

int bitRate(int r) { return r == 1 ? 9600 : r == 2 ? 40000 : 100000; }
int chipRate(int r) { return r == 1 ? 19200 : bitRate(r); }
const char* rateName(int r) { return r == 1 ? "R1 9.6k" : r == 2 ? "R2 40k" : "R3 100k"; }
uint8_t sofByte(int r) { return r == 3 ? 0x5B : 0xF0; }
int maxLength(int r) { return r == 3 ? 170 : 64; }
int checkOctets(int r) { return r == 3 ? 2 : 1; }

// Channel tables of the public regional frequency list: the R1 and R2 channels are 20 kHz apart where both exist (R2 at 868.40, 908.40, 921.40)
const std::vector<Region>& regions() {
    static const std::vector<Region> t = {
        {"EU", "Europe", 869.13, {{868.42, 1}, {868.40, 2}, {869.85, 3}}},
        {"US", "USA / Canada / Mexico", 908.00, {{908.42, 1}, {908.40, 2}, {916.00, 3}}},
        {"ANZ", "Australia / New Zealand", 921.00, {{921.42, 1}, {921.40, 2}, {919.80, 3}}},
        {"HK", "Hong Kong", 919.80, {{919.80, 1}, {919.80, 2}, {919.80, 3}}},
        {"IN", "India", 865.20, {{865.20, 1}, {865.20, 2}, {865.20, 3}}},
        {"RU", "Russia", 869.00, {{869.00, 1}, {869.00, 2}, {869.00, 3}}},
        {"CN", "China", 868.40, {{868.40, 1}, {868.40, 2}, {868.40, 3}}},
        {"IL", "Israel", 916.00, {{916.00, 1}, {916.00, 2}, {916.00, 3}}},
        {"JP", "Japan", 923.00, {{922.50, 2}, {923.90, 2}, {926.30, 2}}},
        {"KR", "South Korea", 921.70, {{920.90, 2}, {921.70, 2}, {923.10, 2}}},
    };
    return t;
}
const Region* region(const std::string& id) {
    for (const Region& r : regions()) if (id == r.id) return &r;
    return nullptr;
}

uint8_t checksum8(const uint8_t* d, size_t n) { uint8_t c = 0xFF; for (size_t i = 0; i < n; i++) c ^= d[i]; return c; }
uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0x1D0F;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

bool parse(const uint8_t* d, size_t n, int rate, Frame& f) {
    const int chk = checkOctets(rate);
    if (n < (size_t)(9 + chk) || n > 255) return false;
    f = Frame();
    f.homeId = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    f.src = d[4]; f.fc1 = d[5]; f.fc2 = d[6]; f.len = d[7];
    if ((size_t)f.len != n) return false;
    f.headerType = f.fc1 & 0x0F;
    f.routed = (f.fc1 & 0x80) != 0; f.ackReq = (f.fc1 & 0x40) != 0; f.lowPower = (f.fc1 & 0x20) != 0; f.speedMod = (f.fc1 & 0x10) != 0;
    f.beam = (f.fc2 >> 5) & 3; f.seq = f.fc2 & 0x0F;
    size_t p = 8;
    if (f.headerType == kHdrMulticast) {
        const int ctl = d[8], maskLen = ctl & 0x1F, off = ctl >> 5;
        if (maskLen < 1 || maskLen > 29 || 9 + (size_t)maskLen + chk > n) return false;
        for (int i = 0; i < maskLen; i++)
            for (int b = 0; b < 8; b++) if (d[9 + i] & (1 << b)) f.group.push_back(off * 32 + i * 8 + b + 1);
        f.dst = -1;
        p = 9 + (size_t)maskLen;
    } else { f.dst = d[8]; p = 9; }
    f.payload.assign(d + p, d + n - chk);
    if (chk == 1) f.checkOk = checksum8(d, n - 1) == d[n - 1];
    else f.checkOk = crc16(d, n - 2) == (uint16_t)((d[n - 2] << 8) | d[n - 1]);
    return true;
}

std::vector<uint8_t> build(const Frame& f, int rate) {
    std::vector<uint8_t> o;
    o.push_back((uint8_t)(f.homeId >> 24)); o.push_back((uint8_t)(f.homeId >> 16)); o.push_back((uint8_t)(f.homeId >> 8)); o.push_back((uint8_t)f.homeId);
    o.push_back((uint8_t)f.src);
    int fc1 = f.headerType & 0x0F;
    if (f.routed) fc1 |= 0x80;
    if (f.ackReq) fc1 |= 0x40;
    if (f.lowPower) fc1 |= 0x20;
    if (f.speedMod) fc1 |= 0x10;
    o.push_back((uint8_t)fc1);
    o.push_back((uint8_t)(((f.beam & 3) << 5) | (f.seq & 0x0F)));
    o.push_back(0);   // Length, set below
    if (f.headerType == kHdrMulticast) {
        int maxNode = 1;
        for (int n : f.group) if (n > maxNode) maxNode = n;
        const int off = (maxNode - 1) / 32 > 7 ? 7 : 0;
        const int maskLen = (maxNode - 1 - off * 32) / 8 + 1;
        std::vector<uint8_t> m((size_t)maskLen, 0);
        for (int n : f.group) { const int k = n - 1 - off * 32; if (k >= 0 && k / 8 < maskLen) m[(size_t)(k / 8)] |= (uint8_t)(1 << (k % 8)); }
        o.push_back((uint8_t)((off << 5) | maskLen));
        o.insert(o.end(), m.begin(), m.end());
    } else o.push_back((uint8_t)f.dst);
    o.insert(o.end(), f.payload.begin(), f.payload.end());
    const int chk = checkOctets(rate);
    o[7] = (uint8_t)(o.size() + (size_t)chk);
    if (chk == 1) o.push_back(checksum8(o.data(), o.size()));
    else { const uint16_t c = crc16(o.data(), o.size()); o.push_back((uint8_t)(c >> 8)); o.push_back((uint8_t)c); }
    return o;
}

const char* headerName(int t) {
    switch (t) {
    case kHdrSinglecast: return "singlecast";
    case kHdrMulticast: return "multicast";
    case kHdrAck: return "ack";
    case kHdrRouted: return "routed";
    default: return "other";
    }
}

const char* ccName(int cc) {
    switch (cc) {
    case 0x00: return "NOP"; case 0x01: return "PROTOCOL";
    case 0x20: return "BASIC"; case 0x21: return "CONTROLLER_REPLICATION"; case 0x22: return "APPLICATION_STATUS"; case 0x23: return "ZIP";
    case 0x24: return "SECURITY_PANEL_MODE"; case 0x25: return "SWITCH_BINARY"; case 0x26: return "SWITCH_MULTILEVEL"; case 0x27: return "SWITCH_ALL";
    case 0x28: return "SWITCH_TOGGLE_BINARY"; case 0x29: return "SWITCH_TOGGLE_MULTILEVEL"; case 0x2B: return "SCENE_ACTIVATION";
    case 0x2C: return "SCENE_ACTUATOR_CONF"; case 0x2D: return "SCENE_CONTROLLER_CONF"; case 0x2E: return "SECURITY_PANEL_ZONE";
    case 0x2F: return "SECURITY_PANEL_ZONE_SENSOR"; case 0x30: return "SENSOR_BINARY"; case 0x31: return "SENSOR_MULTILEVEL"; case 0x32: return "METER";
    case 0x33: return "SWITCH_COLOR"; case 0x34: return "NETWORK_MANAGEMENT_INCLUSION"; case 0x35: return "METER_PULSE"; case 0x36: return "BASIC_TARIFF_INFO";
    case 0x37: return "HRV_STATUS"; case 0x38: return "THERMOSTAT_HEATING"; case 0x39: return "HRV_CONTROL"; case 0x3A: return "DCP_CONFIG";
    case 0x3B: return "DCP_MONITOR"; case 0x3C: return "METER_TBL_CONFIG"; case 0x3D: return "METER_TBL_MONITOR"; case 0x3E: return "METER_TBL_PUSH";
    case 0x3F: return "PREPAYMENT"; case 0x40: return "THERMOSTAT_MODE"; case 0x41: return "PREPAYMENT_ENCAPSULATION"; case 0x42: return "THERMOSTAT_OPERATING_STATE";
    case 0x43: return "THERMOSTAT_SETPOINT"; case 0x44: return "THERMOSTAT_FAN_MODE"; case 0x45: return "THERMOSTAT_FAN_STATE"; case 0x46: return "CLIMATE_CONTROL_SCHEDULE";
    case 0x47: return "THERMOSTAT_SETBACK"; case 0x4C: return "DOOR_LOCK_LOGGING"; case 0x4D: return "NETWORK_MANAGEMENT_BASIC"; case 0x4E: return "SCHEDULE_ENTRY_LOCK";
    case 0x4F: return "ZIP_6LOWPAN"; case 0x50: return "BASIC_WINDOW_COVERING"; case 0x51: return "MTP_WINDOW_COVERING"; case 0x52: return "NETWORK_MANAGEMENT_PROXY";
    case 0x53: return "SCHEDULE"; case 0x54: return "NETWORK_MANAGEMENT_PRIMARY"; case 0x55: return "TRANSPORT_SERVICE"; case 0x56: return "CRC_16_ENCAP";
    case 0x57: return "APPLICATION_CAPABILITY"; case 0x58: return "ZIP_ND"; case 0x59: return "ASSOCIATION_GRP_INFO"; case 0x5A: return "DEVICE_RESET_LOCALLY";
    case 0x5B: return "CENTRAL_SCENE"; case 0x5C: return "IP_ASSOCIATION"; case 0x5D: return "ANTITHEFT"; case 0x5E: return "ZWAVEPLUS_INFO"; case 0x5F: return "ZIP_GATEWAY";
    case 0x60: return "MULTI_CHANNEL"; case 0x61: return "ZIP_PORTAL"; case 0x62: return "DOOR_LOCK"; case 0x63: return "USER_CODE"; case 0x64: return "HUMIDITY_CONTROL_SETPOINT";
    case 0x65: return "DMX"; case 0x66: return "BARRIER_OPERATOR"; case 0x67: return "NETWORK_MANAGEMENT_INSTALLATION_MAINTENANCE"; case 0x68: return "ZIP_NAMING";
    case 0x69: return "MAILBOX"; case 0x6A: return "WINDOW_COVERING"; case 0x6B: return "IRRIGATION"; case 0x6C: return "SUPERVISION"; case 0x6D: return "HUMIDITY_CONTROL_MODE";
    case 0x6E: return "HUMIDITY_CONTROL_OPERATING_STATE"; case 0x6F: return "ENTRY_CONTROL"; case 0x70: return "CONFIGURATION"; case 0x71: return "NOTIFICATION";
    case 0x72: return "MANUFACTURER_SPECIFIC"; case 0x73: return "POWERLEVEL"; case 0x74: return "INCLUSION_CONTROLLER"; case 0x75: return "PROTECTION"; case 0x76: return "LOCK";
    case 0x77: return "NODE_NAMING"; case 0x78: return "NODE_PROVISIONING"; case 0x79: return "SOUND_SWITCH"; case 0x7A: return "FIRMWARE_UPDATE_MD"; case 0x7B: return "GROUPING_NAME";
    case 0x7C: return "REMOTE_ASSOCIATION_ACTIVATE"; case 0x7D: return "REMOTE_ASSOCIATION"; case 0x80: return "BATTERY"; case 0x81: return "CLOCK"; case 0x82: return "HAIL";
    case 0x84: return "WAKE_UP"; case 0x85: return "ASSOCIATION"; case 0x86: return "VERSION"; case 0x87: return "INDICATOR"; case 0x88: return "PROPRIETARY"; case 0x89: return "LANGUAGE";
    case 0x8A: return "TIME"; case 0x8B: return "TIME_PARAMETERS"; case 0x8C: return "GEOGRAPHIC_LOCATION"; case 0x8E: return "MULTI_CHANNEL_ASSOCIATION"; case 0x8F: return "MULTI_CMD";
    case 0x90: return "ENERGY_PRODUCTION"; case 0x91: return "MANUFACTURER_PROPRIETARY"; case 0x92: return "SCREEN_MD"; case 0x93: return "SCREEN_ATTRIBUTES";
    case 0x94: return "SIMPLE_AV_CONTROL"; case 0x98: return "SECURITY"; case 0x9A: return "IP_CONFIGURATION"; case 0x9B: return "ASSOCIATION_COMMAND_CONFIGURATION";
    case 0x9C: return "SENSOR_ALARM"; case 0x9D: return "SILENCE_ALARM"; case 0x9E: return "SENSOR_CONFIGURATION"; case 0x9F: return "SECURITY_2";
    case 0xEF: return "MARK"; case 0xF0: return "NON_INTEROPERABLE";
    default: return "";
    }
}

std::string cmdName(int cc, int cmd) {
    switch (cc) {
    case 0x01:   // protocol commands
        switch (cmd) {
        case 0x01: return "Node Info"; case 0x02: return "Request Node Info"; case 0x03: return "Assign IDs"; case 0x04: return "Find Nodes In Range";
        case 0x05: return "Get Nodes In Range"; case 0x06: return "Range Info"; case 0x07: return "Command Complete"; case 0x0C: return "Assign Return Route";
        case 0x0D: return "New Node Registered"; case 0x11: return "SUC Node ID"; case 0x12: return "Set SUC"; case 0x15: return "Static Route Request";
        case 0x16: return "Lost"; case 0x18: return "NOP Power"; default: return "";
        }
    case 0x20: case 0x25: case 0x26: case 0x30: case 0x80: case 0x71: case 0x62: case 0x31: case 0x32: case 0x70: case 0x72: case 0x5E: case 0x85: case 0x84: case 0x86: case 0x6C:
        break;
    default: return "";
    }
    switch (cc) {
    case 0x20: case 0x25: return cmd == 1 ? "Set" : cmd == 2 ? "Get" : cmd == 3 ? "Report" : "";
    case 0x26: return cmd == 1 ? "Set" : cmd == 2 ? "Get" : cmd == 3 ? "Report" : cmd == 4 ? "Start Level Change" : cmd == 5 ? "Stop Level Change" : "";
    case 0x30: return cmd == 2 ? "Get" : cmd == 3 ? "Report" : "";
    case 0x31: return cmd == 4 ? "Get" : cmd == 5 ? "Report" : "";
    case 0x32: return cmd == 1 ? "Get" : cmd == 2 ? "Report" : "";
    case 0x62: return cmd == 1 ? "Operation Set" : cmd == 2 ? "Operation Get" : cmd == 3 ? "Operation Report" : "";
    case 0x70: return cmd == 4 ? "Set" : cmd == 5 ? "Get" : cmd == 6 ? "Report" : "";
    case 0x71: return cmd == 4 ? "Get" : cmd == 5 ? "Report" : "";
    case 0x72: return cmd == 4 ? "Get" : cmd == 5 ? "Report" : "";
    case 0x5E: return cmd == 1 ? "Get" : cmd == 2 ? "Report" : "";
    case 0x6C: return cmd == 1 ? "Get" : cmd == 2 ? "Report" : "";
    case 0x80: return cmd == 2 ? "Get" : cmd == 3 ? "Report" : "";
    case 0x84: return cmd == 4 ? "Interval Set" : cmd == 5 ? "Interval Get" : cmd == 6 ? "Interval Report" : cmd == 7 ? "Notification" : cmd == 8 ? "No More Information" : "";
    case 0x85: return cmd == 1 ? "Set" : cmd == 2 ? "Get" : cmd == 3 ? "Report" : cmd == 4 ? "Remove" : "";
    case 0x86: return cmd == 0x11 ? "Get" : cmd == 0x12 ? "Report" : "";
    default: return "";
    }
}

int encryption(const std::vector<uint8_t>& p) {
    if (p.size() < 2) return 0;
    if (p[0] == 0x98 && (p[1] == 0x81 || p[1] == 0xC1)) return 1;
    if (p[0] == 0x9F && p[1] == 0x03) return 2;
    return 0;
}

static const char* sensorType(int t) {
    switch (t) {
    case 1: return "temperature"; case 2: return "general"; case 3: return "luminance"; case 4: return "power"; case 5: return "humidity";
    case 6: return "velocity"; case 7: return "direction"; case 8: return "atmospheric pressure"; case 9: return "barometric pressure"; case 10: return "solar radiation";
    default: return "sensor";
    }
}

bool parseRoute(const std::vector<uint8_t>& p, Route& r) {
    r = Route();
    if (p.size() < 2) return false;
    const int type = p[0] & 0x0F, count = p[1] >> 4, hop = p[1] & 0x0F;
    if (type > 2 || count > 4 || hop > count || p.size() < 2 + (size_t)count) return false;
    r.valid = true; r.type = type; r.count = count; r.hop = hop; r.size = 2 + (size_t)count;
    for (int i = 0; i < count; i++) r.repeaters.push_back(p[2 + i]);
    return true;
}

std::vector<uint8_t> buildRoute(int type, int hop, const std::vector<int>& reps) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(type & 0x0F));
    v.push_back((uint8_t)((reps.size() & 15) << 4 | (hop & 15)));
    for (int r : reps) v.push_back((uint8_t)r);
    return v;
}

std::vector<int> routePath(const Frame& f, const Route& r) {
    std::vector<int> path;
    if (!f.routed || !r.valid || f.dst < 0) return path;
    path.push_back(f.src);
    for (int x : r.repeaters) path.push_back(x);
    path.push_back(f.dst);
    return path;
}

std::string describe(const Frame& f) {
    char b[200];
    if (f.headerType == kHdrAck && !f.routed) return "ack";
    const auto& p = f.payload;
    if (p.empty()) return f.routed ? "routed" : "";
    if (f.routed) {
        Route r;
        if (!parseRoute(p, r)) return "routed (route header damaged)";
        std::string via;
        for (int x : r.repeaters) via += (via.empty() ? "" : " > ") + std::to_string(x);
        static const char* kind[] = {"routed", "routed ack", "routed error"};
        snprintf(b, sizeof b, "%s %d > %s%s%s (hop %d of %d)", kind[r.type], f.src, via.c_str(), via.empty() ? "" : " > ", f.dst == 255 ? "all" : std::to_string(f.dst).c_str(), r.hop, r.count + 1);
        std::string head = b;
        if (r.type != 0 || p.size() <= r.size) return head;
        Frame inner = f;
        inner.routed = false; inner.headerType = kHdrSinglecast;
        inner.payload.assign(p.begin() + (long)r.size, p.end());
        const std::string in = describe(inner);
        return in.empty() ? head : head + ": " + in;
    }
    const int enc = encryption(p);
    if (enc) { snprintf(b, sizeof b, "Security %s encrypted (%zu octets, not decoded)", enc == 1 ? "S0" : "S2", p.size()); return b; }
    const int cc = p[0];
    if (cc == 0x00) return "NOP";
    std::string s = ccName(cc);
    if (s.empty()) { snprintf(b, sizeof b, "command class 0x%02X", cc); s = b; }
    if (p.size() < 2) return s;
    const std::string c = cmdName(cc, p[1]);
    if (c.empty()) { snprintf(b, sizeof b, " cmd 0x%02X", p[1]); s += b; } else s += " " + c;
    if (cc == 0x9F || cc == 0x98) {   // key exchange and nonces: the names only, no key material
        static const char* s2[] = {"", "Nonce Get", "Nonce Report", "Message Encapsulation", "KEX Get", "KEX Report", "KEX Set", "KEX Fail", "Public Key Report", "Network Key Get",
                                   "Network Key Report", "Network Key Verify", "Transfer End", "Commands Supported Get", "Commands Supported Report", "Capabilities Get", "Capabilities Report"};
        if (cc == 0x9F && p[1] < 17) return std::string("SECURITY_2 ") + s2[p[1]];
        switch (p[1]) {
        case 0x40: return "SECURITY Nonce Get"; case 0x80: return "SECURITY Nonce Report"; case 0x02: return "SECURITY Commands Supported Get";
        case 0x03: return "SECURITY Commands Supported Report"; case 0x04: return "SECURITY Scheme Get"; case 0x05: return "SECURITY Scheme Report";
        case 0x06: return "SECURITY Network Key Set"; case 0x07: return "SECURITY Network Key Verify"; case 0x08: return "SECURITY Scheme Inherit";
        default: return s;
        }
    }
    if (p.size() >= 3 && (cc == 0x20 || cc == 0x25 || cc == 0x26) && (p[1] == 1 || p[1] == 3)) {
        const int v = p[2];
        if (v == 0) s += " off"; else if (v == 0xFF) s += " on"; else if (v <= 99) { snprintf(b, sizeof b, " %d%%", v); s += b; }
        return s;
    }
    if (p.size() >= 3 && cc == 0x80 && p[1] == 3) { if (p[2] == 0xFF) s += " low battery"; else { snprintf(b, sizeof b, " %d%%", p[2]); s += b; } return s; }
    if (p.size() >= 3 && cc == 0x30 && p[1] == 3) { s += p[2] ? " triggered" : " idle"; return s; }
    if (p.size() >= 5 && cc == 0x31 && p[1] == 5) {
        const int size = p[3] & 7, prec = p[3] >> 5, scale = (p[3] >> 3) & 3;
        if ((size == 1 || size == 2 || size == 4) && p.size() >= 4 + (size_t)size) {
            int32_t v = (int8_t)p[4];
            for (int i = 1; i < size; i++) v = (v << 8) | p[4 + (size_t)i];
            double x = v;
            for (int i = 0; i < prec; i++) x /= 10;
            snprintf(b, sizeof b, " %s %.*f%s", sensorType(p[2]), prec, x, p[2] == 1 ? (scale ? " F" : " C") : "");
            s += b;
        }
    }
    return s;
}

std::string nodeText(int id) { if (id == 255) return "all"; char b[8]; snprintf(b, sizeof b, "%d", id); return b; }
std::string homeText(uint32_t id) { char b[12]; snprintf(b, sizeof b, "%08X", id); return b; }
std::string hex(const uint8_t* d, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", d[i]); s += b; }
    return s;
}

} // namespace zwave
} // namespace dect2
