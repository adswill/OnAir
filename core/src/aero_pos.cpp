// Positions from Aero ACARS messages (formats and sources in aero_pos.h).
#include "dect2/aero_pos.h"
#include "dect2/aero_adsc.h"

#include <cctype>
#include <cstdlib>

namespace dect2 {

namespace {

bool digits(const std::string& s, size_t a, size_t n) {
    if (a + n > s.size()) return false;
    for (size_t i = a; i < a + n; i++) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}
int num(const std::string& s, size_t a, size_t n) { return std::atoi(s.substr(a, n).c_str()); }

// [NS]ddddd[ ][EW]dddddd at p; minutes: DDMMm (else thousandths of a degree). Returns the end, or npos.
size_t coords(const std::string& s, size_t p, bool minutes, double& lat, double& lon) {
    if (p >= s.size() || (s[p] != 'N' && s[p] != 'S') || !digits(s, p + 1, 5)) return std::string::npos;
    size_t q = p + 6;
    if (q < s.size() && s[q] == ' ') q++;
    if (q >= s.size() || (s[q] != 'E' && s[q] != 'W') || !digits(s, q + 1, 6)) return std::string::npos;
    const int a = num(s, p + 1, 5), b = num(s, q + 1, 6);
    if (minutes) {
        if (a % 1000 >= 600 || b % 1000 >= 600) return std::string::npos;
        lat = a / 1000 + (a % 1000) / 600.0;
        lon = b / 1000 + (b % 1000) / 600.0;
    } else {
        lat = a / 1000.0;
        lon = b / 1000.0;
    }
    if (lat > 90 || lon > 180) return std::string::npos;
    if (s[p] == 'S') lat = -lat;
    if (s[q] == 'W') lon = -lon;
    return q + 7;
}

// ",[waypoint,]HHMMSS,FL" after the position
void timeAndLevel(const std::string& s, size_t e, AeroPosition& out) {
    std::vector<std::string> f;
    size_t i = e;
    while (i < s.size() && s[i] == ',' && f.size() < 4) {
        size_t j = i + 1;
        while (j < s.size() && s[j] != ',' && s[j] != '/' && s[j] != '\r' && s[j] != '\n') j++;
        f.push_back(s.substr(i + 1, j - i - 1));
        i = j;
    }
    auto isTime = [](const std::string& t) {
        return t.size() == 6 && digits(t, 0, 6) && num(t, 0, 2) < 24 && num(t, 2, 2) < 60 && num(t, 4, 2) < 60;
    };
    size_t k = 0;
    if (!f.empty() && !isTime(f[0])) k = 1;                     // the waypoint (or an empty field) comes first
    if (k < f.size() && isTime(f[k])) {
        out.secOfDay = num(f[k], 0, 2) * 3600 + num(f[k], 2, 2) * 60 + num(f[k], 4, 2);
        if (k + 1 < f.size()) {
            const std::string& fl = f[k + 1];
            if (!fl.empty() && fl.size() <= 3 && digits(fl, 0, fl.size())) { out.hasAlt = true; out.altFt = num(fl, 0, fl.size()) * 100; }
        }
    }
}

bool label16(const std::string& s, AeroPosition& out) {
    if (s.size() < 6 || (s[0] != 'N' && s[0] != 'S') || s[1] != ' ') return false;
    char* end = nullptr;
    const double la = std::strtod(s.c_str() + 2, &end);
    if (end == s.c_str() + 2 || (*end != ',' && *end != '/')) return false;
    const char* p = end + 1;
    if (*p != 'E' && *p != 'W') return false;
    const char ew = *p++;
    while (*p == ' ') p++;
    const double lo = std::strtod(p, &end);
    if (end == p || la > 90 || lo > 180) return false;
    out.lat = s[0] == 'S' ? -la : la;
    out.lon = ew == 'W' ? -lo : lo;
    if (*end == ',') {
        const char* a = end + 1;
        if (std::isdigit((unsigned char)*a)) { out.hasAlt = true; out.altFt = std::atoi(a); }
        else if (a[0] == 'G' && a[1] == 'R' && a[2] == 'D') { out.hasAlt = true; out.altFt = 0; }
    }
    out.kind = "label 16 position";
    return true;
}

} // namespace

bool aeroParseTextPosition(const std::string& label, const std::string& text, AeroPosition& out) {
    out = AeroPosition();
    out.source = 2;
    if (label == "16" && label16(text, out)) { out.valid = true; return true; }
    const bool tenths = label != "20";
    for (const char* key : {"POS", "/PS"}) {
        size_t f = 0;
        while ((f = text.find(key, f)) != std::string::npos) {
            const size_t e = coords(text, f + 3, tenths, out.lat, out.lon);
            if (e != std::string::npos) {
                timeAndLevel(text, e, out);
                out.kind = label == "20" ? "label 20 POS report" : "POS report";
                out.valid = true;
                return true;
            }
            f++;
        }
    }
    out = AeroPosition();
    return false;
}

bool aeroPositionFromMessage(const std::string& label, const std::string& text, bool uplink, AeroPosition& out, std::string* detail) {
    out = AeroPosition();
    if (detail) detail->clear();
    AeroAdscMessage a;
    if (aeroDecodeAdsc(text, uplink, a)) {
        if (detail) *detail = aeroDescribeAdsc(a);
        if (!a.crcOk || !a.hasPos) return false;
        out.valid = true;
        out.source = 1;
        out.kind = std::string("ADS-C ") + aeroAdscTagName(a.posTag);
        out.lat = a.lat; out.lon = a.lon;
        out.hasAlt = true; out.altFt = a.altFt;
        out.secPastHour = a.timeSec;
        if (a.hasEarth && a.trackValid) { out.hasTrack = true; out.trackDeg = a.trackDeg; }
        else if (a.hasAir && a.headingValid) { out.hasTrack = true; out.trackDeg = a.headingDeg; }
        if (a.hasEarth) { out.hasSpeed = true; out.speedKt = a.groundKt; }
        out.flightId = a.flightId;
        out.icao = a.icao;
        if (a.hasRoute) {
            out.route.push_back({a.next.lat, a.next.lon});
            out.route.push_back({a.nextNext.lat, a.nextNext.lon});
        }
        return true;
    }
    return aeroParseTextPosition(label, text, out);
}

} // namespace dect2
