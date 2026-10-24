// NMEA 0183 RMC / GGA position sentences carried as plain text in DMR short-data messages.
// Format references: the NMEA 0183 standard (copyrighted) and the public field tables of Trimble
// (RMC/GGA) and Microchip GNSSDO; the DMR transport is short data, ETSI TS 102 361-3 clause 6.
#include "dect2/dmr/dmr_nmea.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dect2 {
namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return v;
}

// Parses a whole field: it must not be empty, must be finite, and nothing may follow the number.
bool num(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE || !std::isfinite(v)) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    out = v;
    return true;
}

char hemiOf(const std::string& s) { return s.size() == 1 ? s[0] : 0; }

// ddmm.mmmm (latitude) or dddmm.mmmm (longitude) -> signed degrees; validates the field and its range
bool coord(const std::string& v, bool isLat, char hemi, double& out) {
    double raw = 0;
    if (!num(v, raw) || raw < 0.0) return false;
    const int deg = (int)(raw / 100.0);
    const double min = raw - deg * 100.0;
    if (min < 0.0 || min >= 60.0) return false;
    if (isLat ? deg > 90 : deg > 180) return false;
    if (isLat ? (hemi != 'N' && hemi != 'S') : (hemi != 'E' && hemi != 'W')) return false;
    const double d = deg + min / 60.0;
    out = (hemi == 'S' || hemi == 'W') ? -d : d;
    return true;
}

// latitude and hemisphere / longitude and hemisphere, starting at f[i]; a 0/0 position is rejected
bool position(const std::vector<std::string>& f, size_t i, double& lat, double& lon) {
    if (f.size() < i + 4) return false;
    if (!coord(f[i], true, hemiOf(f[i + 1]), lat)) return false;
    if (!coord(f[i + 2], false, hemiOf(f[i + 3]), lon)) return false;
    return !(lat == 0.0 && lon == 0.0);
}

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool checksumOk(const std::string& body, bool hasCs, const std::string& cs) {
    if (!hasCs) return true;                          // no checksum marker: nothing to verify
    if (cs.size() != 2 || !isHex(cs[0]) || !isHex(cs[1])) return false;   // a marker with a malformed checksum
    unsigned calc = 0;
    for (unsigned char c : body) calc ^= c;
    unsigned given = 0;
    if (std::sscanf(cs.c_str(), "%2x", &given) != 1) return false;
    return calc == given;
}

} // namespace

bool parseDmrNmea(const char* text, size_t size, DmrLocation& out) {
    if (!text || size < 6) return false;
    const std::string s(text, size);
    for (size_t start = s.find('$'); start != std::string::npos; start = s.find('$', start + 1)) {
        if (start + 6 > s.size()) break;
        const char* p = s.c_str() + start;                  // "$GNRMC" / "$GPGGA" / "$GLRMC" ...
        const bool rmc = p[3] == 'R' && p[4] == 'M' && p[5] == 'C';
        const bool gga = p[3] == 'G' && p[4] == 'G' && p[5] == 'A';
        if (!rmc && !gga) continue;
        size_t end = s.find_first_of("*\r\n", start);
        if (end == std::string::npos) end = s.size();
        const bool hasCs = end < s.size() && s[end] == '*';
        const std::string cs = hasCs ? s.substr(end + 1, 2) : std::string();
        if (!checksumOk(s.substr(start + 1, end - start - 1), hasCs, cs)) continue;
        const std::vector<std::string> f = split(s.substr(start, end - start), ',');
        if (rmc) {
            if (f.size() < 9 || f[2] != "A") continue;      // A = valid, V = void
            double lat = 0, lon = 0;
            if (!position(f, 3, lat, lon)) continue;
            DmrLocation loc;
            loc.hasFix = true; loc.lat = lat; loc.lon = lon;
            double v = 0;
            if (num(f[7], v)) { loc.hasSpeed = true; loc.speedMs = v * 0.514444; }   // knots -> m/s
            if (num(f[8], v)) { loc.hasHeading = true; loc.headingDeg = v; }
            out = loc;
            return true;
        }
        if (f.size() < 7) continue;
        double quality = 0;
        if (!num(f[6], quality) || quality < 1.0 || quality != std::floor(quality)) continue;   // GGA quality 0 = no fix
        double lat = 0, lon = 0;
        if (!position(f, 2, lat, lon)) continue;
        out = DmrLocation();
        out.hasFix = true; out.lat = lat; out.lon = lon;
        return true;
    }
    return false;
}

} // namespace dect2
