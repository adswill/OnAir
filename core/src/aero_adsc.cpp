// ADS-C downlink groups and their ARINC 622 wrapping (layouts in aero_adsc.h). Written from the layouts libacars documents
// (adsc.c, arinc.c, crc.c); no code taken from it.
#include "dect2/aero_adsc.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {

struct Bits {
    const uint8_t* p; size_t n; size_t pos = 0;
    Bits(const uint8_t* b, size_t bytes) : p(b), n(bytes * 8) {}
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++, pos++) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u);
        return v;
    }
};

struct BitOut {
    std::vector<uint8_t> b; size_t pos = 0;
    explicit BitOut(size_t bytes) : b(bytes, 0) {}
    void put(uint32_t v, int k) {
        for (int i = k - 1; i >= 0; i--, pos++)
            if ((v >> i) & 1u) b[pos >> 3] |= uint8_t(0x80 >> (pos & 7));
    }
};

int sext(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    return int(v ^ m) - int(m);
}
// field of `bits` bits, two's complement, most significant bit worth `msbDeg`: the LSB is msbDeg / 2^(bits-2)
double angle(uint32_t v, int bits, double msbDeg) { return sext(v, bits) * msbDeg / double(1u << (bits - 2)); }
double heading(uint32_t v) { double d = angle(v, 12, 90); return d < 0 ? d + 360 : d; }

uint32_t toField(long r, int bits) {
    const long lo = -(1L << (bits - 1)), hi = (1L << (bits - 1)) - 1;
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return uint32_t(r) & ((1u << bits) - 1);
}
uint32_t fromAngle(double deg, int bits, double msbDeg) {
    if (deg > 180) deg -= 360;
    return toField(std::lround(deg * double(1u << (bits - 2)) / msbDeg), bits);
}

// the fixed length of a downlink group after its tag; -1 unknown, -2 variable (handled where it is read)
int groupLen(int tag) {
    switch (tag) {
    case 3: return 1;                                   // acknowledgement: contract number
    case 4: case 5: return -2;                          // negative acknowledgement, noncompliance notification
    case 6: return 0;                                   // cancel emergency mode
    case 7: case 9: case 10: case 18: case 19: case 20: return 10;
    case 12: return 6;
    case 13: return 17;
    case 14: case 15: return 5;
    case 16: return 4;
    case 17: return 3;
    case 22: return 8;
    case 23: return 9;
    default: return -1;
    }
}

bool upperOrDigit(const std::string& s, size_t a, size_t n) {
    if (a + n > s.size()) return false;
    for (size_t i = a; i < a + n; i++) if (!std::isupper((unsigned char)s[i]) && !std::isdigit((unsigned char)s[i])) return false;
    return true;
}
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

uint16_t aeroArincCrc(const uint8_t* p, size_t n, uint16_t init) {
    uint16_t crc = init;
    for (size_t i = 0; i < n; i++) {
        crc ^= uint16_t(p[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

const char* aeroAdscTagName(int tag) {
    switch (tag) {
    case 3: return "Acknowledgement";
    case 4: return "Negative acknowledgement";
    case 5: return "Noncompliance notification";
    case 6: return "Cancel emergency mode";
    case 7: return "Basic report";
    case 9: return "Emergency basic report";
    case 10: return "Lateral deviation change event";
    case 12: return "Flight ID";
    case 13: return "Predicted route";
    case 14: return "Earth reference";
    case 15: return "Air reference";
    case 16: return "Meteorological data";
    case 17: return "Airframe ID";
    case 18: return "Vertical rate change event";
    case 19: return "Altitude range event";
    case 20: return "Waypoint change event";
    case 22: return "Intermediate projection";
    case 23: return "Fixed projection";
    default: return "";
    }
}

bool aeroParseAdscGroups(const uint8_t* p, size_t n, AeroAdscMessage& out) {
    size_t i = 0;
    while (i < n) {
        const int tag = p[i++];
        int len = groupLen(tag);
        if (len == -1) return false;
        if (tag == 4) len = (i + 1 < n && (p[i + 1] == 1 || p[i + 1] == 2 || p[i + 1] == 7)) ? 3 : 2;   // these reasons carry one more octet
        if (tag == 5) {
            // contract number, group count, then per group: tag, flags / count, and the parameter numbers as nibbles
            if (i + 2 > n) return false;
            size_t j = i + 2;
            for (int g = 0; g < p[i + 1]; g++) {
                if (j + 2 > n) return false;
                const uint8_t f = p[j + 1];
                const int cnt = (f & 0xC0) ? 0 : (f & 0x0F);
                j += 2 + size_t((cnt + 1) / 2);
            }
            len = int(j - i);
        }
        if (i + size_t(len) > n) return false;
        out.tags.push_back(tag);
        Bits b(p + i, size_t(len));
        switch (tag) {
        case 7: case 9: case 10: case 18: case 19: case 20:
            // a later report in the same message wins only over an event report; the basic report is the one to show
            if (!out.hasPos || out.posTag != 7) {
                out.hasPos = true;
                out.posTag = tag;
                out.lat = angle(b.get(21), 21, 90);
                out.lon = angle(b.get(21), 21, 90);
                out.altFt = sext(b.get(16), 16) * 4;
                out.timeSec = b.get(15) * 0.125;
                const uint32_t fom = b.get(7);
                out.redundancyOk = fom & 1;
                out.accuracy = int((fom >> 1) & 7);
                out.tcasOk = (fom >> 4) & 1;
            }
            break;
        case 12: {
            std::string id;
            for (int k = 0; k < 8; k++) {
                uint32_t c = b.get(6);
                if (!(c & 0x20)) c += 0x40;                 // A-Z are sent as 01 to 1A
                id += char(c);
            }
            while (!id.empty() && id.back() == ' ') id.pop_back();
            out.flightId = id;
            break;
        }
        case 13: {
            out.hasRoute = true;
            out.next.lat = angle(b.get(21), 21, 90);
            out.next.lon = angle(b.get(21), 21, 90);
            out.next.altFt = sext(b.get(16), 16) * 4;
            out.next.etaSec = int(b.get(14));
            out.nextNext.lat = angle(b.get(21), 21, 90);
            out.nextNext.lon = angle(b.get(21), 21, 90);
            out.nextNext.altFt = sext(b.get(16), 16) * 4;
            break;
        }
        case 14: case 15: {
            const bool invalid = b.get(1);
            const double h = heading(b.get(12));
            const uint32_t spd = b.get(13);
            const int vs = sext(b.get(12), 12) * 16;
            if (tag == 14) { out.hasEarth = true; out.trackValid = !invalid; out.trackDeg = h; out.groundKt = spd / 2.0; out.vsFpm = vs; }
            else { out.hasAir = true; out.headingValid = !invalid; out.headingDeg = h; out.mach = spd / 2000.0; out.airVsFpm = vs; }
            break;
        }
        case 16: {
            out.hasMeteo = true;
            out.windKt = b.get(9) / 2.0;
            out.windValid = !b.get(1);
            double d = angle(b.get(9), 9, 90);
            out.windDeg = d < 0 ? d + 360 : d;
            out.tempC = sext(b.get(12), 12) * 0.25;
            break;
        }
        case 17:
            out.icao = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
            break;
        default:
            break;
        }
        i += size_t(len);
    }
    return true;
}

bool aeroDecodeAdsc(const std::string& text, bool uplink, AeroAdscMessage& out) {
    out = AeroAdscMessage();
    size_t at = std::string::npos;
    const char* imis[2] = {".ADS.", ".DIS."};
    int which = -1;
    for (int k = 0; k < 2 && at == std::string::npos; k++) {
        size_t f = 0;
        while ((f = text.find(imis[k], f)) != std::string::npos) {
            // the ground address in front: 7 or 4 capitals or digits, at the start or after '/' or a space (H1 "#M1B/B6 " prefix)
            for (size_t gl : {size_t(7), size_t(4)}) {
                if (f < gl || !upperOrDigit(text, f - gl, gl)) continue;
                const size_t s = f - gl;
                if (s == 0 || text[s - 1] == '/' || text[s - 1] == ' ') { at = f; which = k; out.groundAddr = text.substr(s, gl); break; }
            }
            if (at != std::string::npos) break;
            f++;
        }
    }
    if (at == std::string::npos) return false;
    const size_t imi = at + 1, reg = at + 4, hex = at + 11;   // "ADS" or "DIS", then '.' is the first of the 7 address characters
    if (text.size() < hex + 4) return false;
    out.found = true;
    out.disconnect = which == 1;
    out.uplink = uplink;
    out.airReg = text.substr(reg, 7);
    while (!out.airReg.empty() && out.airReg[0] == '.') out.airReg.erase(0, 1);
    std::vector<uint8_t> buf(text.begin() + long(imi), text.begin() + long(hex));   // IMI + address, as characters
    const size_t head = buf.size();
    size_t k = hex;
    while (k + 1 < text.size() && hexVal(text[k]) >= 0 && hexVal(text[k + 1]) >= 0) {
        buf.push_back(uint8_t(hexVal(text[k]) * 16 + hexVal(text[k + 1])));
        k += 2;
    }
    if (buf.size() < head + 2) return true;
    out.crcOk = aeroArincCrc(buf.data(), buf.size()) == 0x1D0F;
    const uint8_t* bin = buf.data() + head;
    const size_t n = buf.size() - head - 2;
    if (out.disconnect) {
        if (n >= 1) out.reason = bin[0];
        out.complete = n >= 1;
        return true;
    }
    if (uplink) return true;                                  // contract requests: found, not decoded
    out.complete = aeroParseAdscGroups(bin, n, out);
    return true;
}

std::string aeroDescribeAdsc(const AeroAdscMessage& m) {
    if (!m.found) return "";
    std::string s;
    char b[200];
    std::snprintf(b, sizeof b, "ADS-C %s from %s, ground %s, CRC %s%s\n", m.disconnect ? "disconnect" : m.uplink ? "uplink (contract request, not decoded)" : "report",
                  m.airReg.c_str(), m.groundAddr.c_str(), m.crcOk ? "good" : "FAILED", m.complete || m.uplink ? "" : ", groups cut short or unknown");
    s += b;
    if (m.disconnect && m.reason >= 0) { std::snprintf(b, sizeof b, "  reason code %d\n", m.reason); s += b; }
    if (m.hasPos) {
        const char* acc[8] = {"navigation lost", "< 30 nm", "< 15 nm", "< 8 nm", "< 4 nm", "< 1 nm", "< 0.25 nm", "< 0.05 nm"};
        const int mm = int(m.timeSec) / 60;
        std::snprintf(b, sizeof b, "  %s: %.5f %.5f, %d ft, at %02d:%06.3f past the hour, accuracy %s%s%s\n", aeroAdscTagName(m.posTag), m.lat, m.lon, m.altFt, mm,
                      m.timeSec - mm * 60, acc[m.accuracy & 7], m.redundancyOk ? "" : ", navigation redundancy lost", m.tcasOk ? "" : ", TCAS not available");
        s += b;
    }
    if (!m.flightId.empty()) { s += "  flight id: " + m.flightId + "\n"; }
    if (m.icao) { std::snprintf(b, sizeof b, "  airframe: ICAO %06X\n", m.icao); s += b; }
    if (m.hasEarth) {
        std::snprintf(b, sizeof b, "  earth reference: track %.1f deg%s, ground speed %.1f kt, vertical %d ft/min\n", m.trackDeg, m.trackValid ? "" : " (invalid)", m.groundKt, m.vsFpm);
        s += b;
    }
    if (m.hasAir) {
        std::snprintf(b, sizeof b, "  air reference: heading %.1f deg%s, Mach %.3f, vertical %d ft/min\n", m.headingDeg, m.headingValid ? "" : " (invalid)", m.mach, m.airVsFpm);
        s += b;
    }
    if (m.hasRoute) {
        std::snprintf(b, sizeof b, "  predicted route: next %.4f %.4f %d ft in %d s, then %.4f %.4f %d ft\n", m.next.lat, m.next.lon, m.next.altFt, m.next.etaSec,
                      m.nextNext.lat, m.nextNext.lon, m.nextNext.altFt);
        s += b;
    }
    if (m.hasMeteo) {
        std::snprintf(b, sizeof b, "  meteo: wind %.0f deg%s %.1f kt, temperature %.2f C\n", m.windDeg, m.windValid ? "" : " (invalid)", m.windKt, m.tempC);
        s += b;
    }
    std::string other;
    for (int t : m.tags)
        if (t != 7 && t != 9 && t != 10 && t != 12 && t != 13 && t != 14 && t != 15 && t != 16 && t != 17 && t != 18 && t != 19 && t != 20) {
            other += other.empty() ? "" : ", ";
            other += aeroAdscTagName(t);
        }
    if (!other.empty()) s += "  also: " + other + "\n";
    return s;
}

// ---- builders ----

std::vector<uint8_t> aeroAdscBasicGroup(double lat, double lon, int altFt, double secPastHour, int accuracy, int tag) {
    BitOut o(10);
    o.put(fromAngle(lat, 21, 90), 21);
    o.put(fromAngle(lon, 21, 90), 21);
    o.put(toField(std::lround(altFt / 4.0), 16), 16);
    o.put(uint32_t(std::lround(secPastHour * 8)) & 0x7FFF, 15);
    o.put(uint32_t((1u << 4) | ((accuracy & 7) << 1) | 1u), 7);   // TCAS ok, accuracy, redundancy ok
    std::vector<uint8_t> g{uint8_t(tag)};
    g.insert(g.end(), o.b.begin(), o.b.end());
    return g;
}

std::vector<uint8_t> aeroAdscFlightIdGroup(const std::string& id) {
    BitOut o(6);
    for (int k = 0; k < 8; k++) {
        const char c = k < (int)id.size() ? char(std::toupper((unsigned char)id[k])) : ' ';
        o.put(uint32_t(c) & 0x3F, 6);
    }
    std::vector<uint8_t> g{12};
    g.insert(g.end(), o.b.begin(), o.b.end());
    return g;
}

std::vector<uint8_t> aeroAdscEarthRefGroup(double trackDeg, double groundKt, int vsFpm) {
    BitOut o(5);
    o.put(0, 1);
    o.put(fromAngle(trackDeg, 12, 90), 12);
    o.put(uint32_t(std::lround(groundKt * 2)) & 0x1FFF, 13);
    o.put(toField(std::lround(vsFpm / 16.0), 12), 12);
    std::vector<uint8_t> g{14};
    g.insert(g.end(), o.b.begin(), o.b.end());
    return g;
}

std::vector<uint8_t> aeroAdscPredictedRouteGroup(const AeroAdscPoint& a, const AeroAdscPoint& c) {
    BitOut o(17);
    o.put(fromAngle(a.lat, 21, 90), 21);
    o.put(fromAngle(a.lon, 21, 90), 21);
    o.put(toField(std::lround(a.altFt / 4.0), 16), 16);
    o.put(uint32_t(a.etaSec < 0 ? 0 : a.etaSec) & 0x3FFF, 14);
    o.put(fromAngle(c.lat, 21, 90), 21);
    o.put(fromAngle(c.lon, 21, 90), 21);
    o.put(toField(std::lround(c.altFt / 4.0), 16), 16);
    std::vector<uint8_t> g{13};
    g.insert(g.end(), o.b.begin(), o.b.end());
    return g;
}

std::vector<uint8_t> aeroAdscMeteoGroup(double windKt, double windDeg, double tempC) {
    BitOut o(4);
    o.put(uint32_t(std::lround(windKt * 2)) & 0x1FF, 9);
    o.put(0, 1);
    o.put(fromAngle(windDeg, 9, 90), 9);
    o.put(toField(std::lround(tempC * 4), 12), 12);
    std::vector<uint8_t> g{16};
    g.insert(g.end(), o.b.begin(), o.b.end());
    return g;
}

std::string aeroBuildAdscText(const std::string& groundAddr, const std::string& registration, const std::vector<uint8_t>& groups) {
    std::string reg = registration.substr(0, 7);
    reg.insert(0, 7 - reg.size(), '.');
    const std::string head = "ADS" + reg;
    std::vector<uint8_t> buf(head.begin(), head.end());
    buf.insert(buf.end(), groups.begin(), groups.end());
    const uint16_t crc = uint16_t(~aeroArincCrc(buf.data(), buf.size()));
    std::string s = "/" + groundAddr + "." + head;
    char h[3];
    for (uint8_t v : groups) { std::snprintf(h, sizeof h, "%02X", v); s += h; }
    std::snprintf(h, sizeof h, "%02X", crc >> 8); s += h;
    std::snprintf(h, sizeof h, "%02X", crc & 0xFF); s += h;
    return s;
}

} // namespace dect2
