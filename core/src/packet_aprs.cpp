// APRS parser: see packet_aprs.h. Written from APRS101 (chapters 5-12) and the APRS 1.2 addenda; the Mic-E longitude and speed rules were
// cross-checked against the description in Dire Wolf's decode_aprs.c (WB2OSZ, GPL-2.0-or-later) - no code was copied.
#include "dect2/packet_aprs.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace aprs {

namespace {

std::string fmt(const char* f, double a) { char b[64]; snprintf(b, sizeof b, f, a); return b; }

std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}
bool allDigits(const std::string& s, size_t a, size_t n) {
    if (a + n > s.size()) return false;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[a + i])) return false;
    return true;
}

// "4903.50N" -> degrees; spaces (position ambiguity) count as zero
bool parseLat(const std::string& s, double& v) {
    if (s.size() < 8) return false;
    std::string t = s.substr(0, 8);
    for (char& c : t) if (c == ' ') c = '0';
    if (!allDigits(t, 0, 4) || t[4] != '.' || !allDigits(t, 5, 2)) return false;
    const char h = t[7];
    if (h != 'N' && h != 'S') return false;
    v = atoi(t.substr(0, 2).c_str()) + atof(t.substr(2, 5).c_str()) / 60.0;
    if (v > 90) return false;
    if (h == 'S') v = -v;
    return true;
}
bool parseLon(const std::string& s, double& v) {
    if (s.size() < 9) return false;
    std::string t = s.substr(0, 9);
    for (char& c : t) if (c == ' ') c = '0';
    if (!allDigits(t, 0, 5) || t[5] != '.' || !allDigits(t, 6, 2)) return false;
    const char h = t[8];
    if (h != 'E' && h != 'W') return false;
    v = atoi(t.substr(0, 3).c_str()) + atof(t.substr(3, 5).c_str()) / 60.0;
    if (v > 180) return false;
    if (h == 'W') v = -v;
    return true;
}
long base91(const std::string& s, size_t a, size_t n) {
    long v = 0;
    for (size_t i = 0; i < n; i++) v = v * 91 + ((unsigned char)s[a + i] - 33);
    return v;
}

// Weather fields: letter + fixed number of characters ('.' or ' ' = not measured)
bool wxNumber(const std::string& s, size_t pos, size_t n, double& v) {
    if (pos + n > s.size()) return false;
    std::string t = s.substr(pos, n);
    bool anyDigit = false;
    for (char c : t) {
        if (isdigit((unsigned char)c)) anyDigit = true;
        else if (!(c == '.' || c == ' ' || c == '-')) return false;
    }
    if (!anyDigit) { v = NAN; return true; }
    v = atof(t.c_str());
    return true;
}

// The weather part of a comment or of a positionless report. Returns false when nothing looked like weather.
bool parseWeather(const std::string& s, Weather& w) {
    size_t p = 0;
    // wind: "DDD/SSS" or "cDDDsSSS"
    if (s.size() >= 7 && s[3] == '/' ) {
        double d, sp;
        if (wxNumber(s, 0, 3, d) && wxNumber(s, 4, 3, sp)) { w.windDirDeg = d; w.windMph = sp; p = 7; }
    } else if (s.size() >= 8 && s[0] == 'c' && s[4] == 's') {
        double d, sp;
        if (wxNumber(s, 1, 3, d) && wxNumber(s, 5, 3, sp)) { w.windDirDeg = d; w.windMph = sp; p = 8; }
    }
    while (p < s.size()) {
        const char k = s[p];
        double v = NAN;
        size_t n = 0;
        switch (k) {
        case 'g': case 't': case 'r': case 'p': case 'P': case 'L': case 'l': case 's': case '#': n = 3; break;
        case 'h': n = 2; break;
        case 'b': n = 5; break;
        default: break;
        }
        if (!n || !wxNumber(s, p + 1, n, v)) break;
        switch (k) {
        case 'g': w.gustMph = v; break;
        case 't': w.tempF = v; break;
        case 'r': w.rain1hIn = v / 100.0; break;
        case 'p': w.rain24hIn = v / 100.0; break;
        case 'P': w.rainMidnightIn = v / 100.0; break;
        case 'h': w.humidityPct = v == 0 ? 100 : v; break;       // 00 means 100 %
        case 'b': w.pressureMbar = v / 10.0; break;
        default: break;
        }
        p += 1 + n;
    }
    return w.any();
}

std::string wxSummary(const Weather& w) {
    std::string s;
    auto add = [&](const std::string& t) { if (!s.empty()) s += ", "; s += t; };
    if (!std::isnan(w.tempF)) add(fmt("%.0f F", w.tempF));
    if (!std::isnan(w.windMph)) add(fmt("wind %.0f mph", w.windMph) + (std::isnan(w.windDirDeg) ? "" : fmt(" from %.0f deg", w.windDirDeg)));
    if (!std::isnan(w.gustMph)) add(fmt("gust %.0f mph", w.gustMph));
    if (!std::isnan(w.humidityPct)) add(fmt("%.0f%% RH", w.humidityPct));
    if (!std::isnan(w.pressureMbar)) add(fmt("%.1f mbar", w.pressureMbar));
    if (!std::isnan(w.rain1hIn)) add(fmt("rain %.2f in/h", w.rain1hIn));
    return s;
}

// course/speed "DDD/SSS" at the start of a comment, and "/A=aaaaaa" (feet) anywhere
void commentExtras(Info& o) {
    std::string& c = o.comment;
    if (c.size() >= 7 && c[3] == '/' && allDigits(c, 0, 3) && allDigits(c, 4, 3) && o.symCode != '_') {
        o.hasCourse = true; o.courseDeg = atoi(c.substr(0, 3).c_str());
        o.hasSpeed = true; o.speedKnots = atof(c.substr(4, 3).c_str());
        c.erase(0, 7);
    }
    const size_t a = c.find("/A=");
    if (a != std::string::npos && a + 9 <= c.size()) {
        const std::string v = c.substr(a + 3, 6);
        bool ok = true;
        for (size_t i = 0; i < 6; i++) if (!(isdigit((unsigned char)v[i]) || (i == 0 && v[i] == '-'))) ok = false;
        if (ok) { o.hasAlt = true; o.altM = atoi(v.c_str()) * 0.3048; }
    }
}

// The position part: uncompressed (19 characters) or compressed (13). s starts at the symbol table / latitude. Returns the characters used, or 0.
size_t parsePos(const std::string& s, Info& o) {
    if (s.empty()) return 0;
    const unsigned char f = (unsigned char)s[0];
    if (isdigit(f) || f == ' ') {
        if (s.size() < 19) return 0;
        double la, lo;
        if (!parseLat(s.substr(0, 8), la) || !parseLon(s.substr(9, 9), lo)) return 0;
        o.hasPos = true; o.lat = la; o.lon = lo;
        o.symTable = s[8]; o.symCode = s[18];
        return 19;
    }
    if (s.size() < 13) return 0;
    if (!(f == '/' || f == '\\' || isupper(f) || (f >= 'a' && f <= 'j'))) return 0;
    for (size_t i = 1; i <= 8; i++) if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 123) return 0;
    o.hasPos = true;
    o.lat = 90.0 - (double)base91(s, 1, 4) / 380926.0;
    o.lon = -180.0 + (double)base91(s, 5, 4) / 190463.0;
    o.symTable = s[0]; o.symCode = s[9];
    const int c = (unsigned char)s[10] - 33, sp = (unsigned char)s[11] - 33, t = (unsigned char)s[12] - 33;
    if (c >= 0 && sp >= 0) {
        if ((t & 0x18) == 0x10) { o.hasAlt = true; o.altM = std::pow(1.002, c * 91 + sp) * 0.3048; }
        else if (c < 90) {
            if (c > 0) { o.hasCourse = true; o.courseDeg = c * 4; }
            o.hasSpeed = true; o.speedKnots = std::pow(1.08, sp) - 1.0;
        }
    }
    return 13;
}

std::string posText(const Info& o) {
    std::string s = fmtLat(o.lat) + " " + fmtLon(o.lon);
    if (o.symTable && o.symCode) { s += " ["; s += o.symTable; s += o.symCode; s += "]"; }
    if (o.hasSpeed && o.speedKnots >= 0.5) s += fmt(" %.0f kn", o.speedKnots);
    if (o.hasCourse && o.hasSpeed && o.speedKnots >= 0.5) s += fmt(" %.0f deg", (double)o.courseDeg);
    if (o.hasAlt) s += fmt(" %.0f m", o.altM);
    return s;
}

void finishPosition(Info& o, const std::string& rest) {
    o.comment = rest;
    commentExtras(o);
    if (o.symCode == '_') {
        Weather w;
        if (parseWeather(o.comment, w)) { o.wx = w; o.hasWx = true; }
    }
}

bool micE(const std::string& dest, const std::string& info, Info& o) {
    if (dest.size() < 6 || info.size() < 9) return false;
    int dig[6];
    int stdBits = 0, custom = 0;
    for (int i = 0; i < 6; i++) {
        const char c = dest[(size_t)i];
        int d, bit = 0;
        bool custBit = false;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'J') { d = c - 'A'; custBit = true; }
        else if (c == 'K') { d = 0; custBit = true; }
        else if (c == 'L') d = 0;
        else if (c >= 'P' && c <= 'Y') { d = c - 'P'; bit = 1; }
        else if (c == 'Z') { d = 0; bit = 1; }
        else return false;
        dig[i] = d;
        if (i < 3) { if (custBit) custom |= 4 >> i; if (bit) stdBits |= 4 >> i; }
    }
    auto high = [&](int i) { const char c = dest[(size_t)i]; return c >= 'P' && c <= 'Z'; };
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 1000 + dig[3] * 100 + dig[4] * 10 + dig[5]) / 6000.0;
    if (!high(3)) lat = -lat;
    const bool offs = high(4), west = high(5);
    const int lo0 = (unsigned char)info[1], lo1 = (unsigned char)info[2], lo2 = (unsigned char)info[3];
    int d = lo0 - 28 + (offs ? 100 : 0);
    if (d >= 180 && d <= 189) d -= 80;
    else if (d >= 190 && d <= 199) d -= 190;
    int m = lo1 - 28;
    if (m >= 60) m -= 60;
    const int h = lo2 - 28;
    if (d < 0 || d > 179 || m < 0 || m > 59 || h < 0 || h > 99) return false;
    double lon = d + (m + h / 100.0) / 60.0;
    if (west) lon = -lon;
    const int sp = (unsigned char)info[4] - 28, dc = (unsigned char)info[5] - 28, se = (unsigned char)info[6] - 28;
    int speed = sp * 10 + dc / 10;
    if (speed >= 800) speed -= 800;
    int course = (dc % 10) * 100 + se;
    if (course >= 400) course -= 400;
    o.hasPos = true; o.lat = lat; o.lon = lon;
    o.hasSpeed = true; o.speedKnots = speed;
    if (course > 0) { o.hasCourse = true; o.courseDeg = course == 360 ? 0 : course; }
    o.symCode = info[7]; o.symTable = info[8];
    o.comment = info.substr(9);
    // altitude: three base-91 characters and '}' near the start of the comment, metres above -10000
    for (size_t i = 0; i + 3 < o.comment.size() && i < 4; i++) {
        if (o.comment[i + 3] == '}' && (unsigned char)o.comment[i] >= 33 && (unsigned char)o.comment[i + 1] >= 33 && (unsigned char)o.comment[i + 2] >= 33) {
            o.hasAlt = true; o.altM = (double)base91(o.comment, i, 3) - 10000.0;
            o.comment.erase(i, 4);
            break;
        }
    }
    if (!custom && stdBits) {
        static const char* names[8] = {"Emergency", "Priority", "Special", "Committed", "Returning", "In Service", "En Route", "Off Duty"};
        o.micEStatus = names[stdBits];
    }
    return true;
}

} // namespace

std::string fmtLat(double v) { char b[32]; snprintf(b, sizeof b, "%.4f%c", std::fabs(v), v < 0 ? 'S' : 'N'); return b; }
std::string fmtLon(double v) { char b[32]; snprintf(b, sizeof b, "%.4f%c", std::fabs(v), v < 0 ? 'W' : 'E'); return b; }

bool parse(const std::string& dest, const std::string& info, Info& o) {
    o = Info();
    if (info.empty()) return false;
    const unsigned char dti = (unsigned char)info[0];
    switch (dti) {
    case '!': case '=': case '/': case '@': {
        if (dti == '!' && info.size() > 1 && info[1] == '!') { o.type = "Other"; o.text = info; o.summary = "Ultimeter weather station data"; return true; }
        const size_t skip = (dti == '/' || dti == '@') ? 8 : 1;
        if (info.size() <= skip) return false;
        const size_t used = parsePos(info.substr(skip), o);
        if (!used) {
            o.type = "Other"; o.text = info; o.summary = "position report that could not be read"; return true;
        }
        finishPosition(o, info.substr(skip + used));
        o.type = o.hasWx ? "Weather" : "Position";
        o.summary = (o.hasWx ? "Weather " : "Position ") + posText(o);
        if (o.hasWx) o.summary += ": " + wxSummary(o.wx);
        if (!o.comment.empty() && !o.hasWx) o.summary += " " + o.comment;
        return true;
    }
    case '`': case '\'': case 0x1c: case 0x1d:
        if (!micE(dest, info, o)) return false;
        o.type = "Mic-E";
        o.summary = "Mic-E " + posText(o) + (o.micEStatus.empty() ? "" : ", " + o.micEStatus) + (o.comment.empty() ? "" : " " + o.comment);
        return true;
    case ':': {
        if (info.size() < 11 || info[10] != ':') return false;
        o.type = "Message";
        o.name = trimRight(info.substr(1, 9));
        std::string t = info.substr(11);
        const size_t br = t.rfind('{');
        if (t.compare(0, 3, "ack") == 0 && t.size() > 3) { o.isAck = true; o.msgNo = t.substr(3); t.clear(); }
        else if (t.compare(0, 3, "rej") == 0 && t.size() > 3) { o.isRej = true; o.msgNo = t.substr(3); t.clear(); }
        else if (br != std::string::npos && t.size() - br <= 7) { o.msgNo = t.substr(br + 1); t.erase(br); }
        o.text = t;
        if (o.isAck) o.summary = "Ack " + o.msgNo + " to " + o.name;
        else if (o.isRej) o.summary = "Reject " + o.msgNo + " to " + o.name;
        else o.summary = "Message to " + o.name + ": " + t + (o.msgNo.empty() ? "" : " {" + o.msgNo + "}");
        return true;
    }
    case ';': {
        if (info.size() < 18 || (info[10] != '*' && info[10] != '_')) return false;
        o.type = "Object";
        o.name = trimRight(info.substr(1, 9));
        o.live = info[10] == '*';
        const size_t used = parsePos(info.substr(18), o);
        if (!used) return false;
        finishPosition(o, info.substr(18 + used));
        o.summary = std::string("Object ") + o.name + (o.live ? " " : " (killed) ") + posText(o) + (o.comment.empty() ? "" : " " + o.comment);
        return true;
    }
    case ')': {
        size_t e = 1;
        while (e < info.size() && e < 11 && info[e] != '!' && info[e] != '_') e++;
        if (e >= info.size() || e < 4 || (info[e] != '!' && info[e] != '_')) return false;
        o.type = "Item";
        o.name = info.substr(1, e - 1);
        o.live = info[e] == '!';
        const size_t used = parsePos(info.substr(e + 1), o);
        if (!used) return false;
        finishPosition(o, info.substr(e + 1 + used));
        o.summary = std::string("Item ") + o.name + (o.live ? " " : " (killed) ") + posText(o) + (o.comment.empty() ? "" : " " + o.comment);
        return true;
    }
    case '>':
        o.type = "Status";
        o.text = trimRight(info.substr(1));
        if (o.text.size() > 7 && allDigits(o.text, 0, 6) && o.text[6] == 'z') o.text.erase(0, 7);     // a time stamp in front
        o.summary = "Status: " + o.text;
        return true;
    case '_': {
        if (info.size() < 9) return false;
        Weather w;
        if (!parseWeather(info.substr(9), w)) return false;
        o.type = "Weather"; o.hasWx = true; o.wx = w;
        o.summary = "Weather: " + wxSummary(w);
        return true;
    }
    case 'T':
        if (info.size() > 2 && info[1] == '#') { o.type = "Telemetry"; o.text = info.substr(2); o.summary = "Telemetry " + o.text; return true; }
        break;
    case '$': o.type = "Raw GPS"; o.text = trimRight(info); o.summary = o.text; return true;
    case '<': o.type = "Capabilities"; o.text = info.substr(1); o.summary = "Capabilities " + o.text; return true;
    case '?': o.type = "Query"; o.text = info.substr(1); o.summary = "Query " + o.text; return true;
    case '}': o.type = "Third-party"; o.text = info.substr(1); o.summary = "Third-party " + o.text; return true;
    default: break;
    }
    o.type = "Other";
    o.text = trimRight(info);
    o.summary = o.text;
    return true;
}

} // namespace aprs
} // namespace dect2
