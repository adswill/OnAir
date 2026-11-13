// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "results_export.h"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include "dect2/export/exporters.h"

namespace resexp {

std::string num(double v, int dec) {
    if (dec < 0) dec = 0;
    if (dec > 9) dec = 9;
    char b[64];
    snprintf(b, sizeof b, "%.*f", dec, v);
    for (char* p = b; *p; p++) if (*p == ',') *p = '.';   // belt and braces if a C locale was changed
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

static std::string csvCell(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string toCsv(const Table& t) {
    std::string o;
    auto line = [&](const std::vector<std::string>& r) {
        for (size_t i = 0; i < r.size(); i++) { if (i) o += ','; o += csvCell(r[i]); }
        o += "\r\n";
    };
    line(t.cols);
    for (const auto& r : t.rows) line(r);
    return o;
}

static bool isNumber(const std::string& s) {
    if (s.empty() || s.size() > 24) return false;
    size_t i = 0;
    if (s[i] == '-') i++;
    if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return false;
    if (s[i] == '0' && i + 1 < s.size() && s[i + 1] != '.') return false;   // 007 stays text
    bool dot = false;
    for (; i < s.size(); i++) {
        if (s[i] == '.') { if (dot || i + 1 >= s.size()) return false; dot = true; }
        else if (!(s[i] >= '0' && s[i] <= '9')) return false;
    }
    return true;
}

static std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;   // UTF-8 passes through
        }
    }
    return o + "\"";
}

std::string toJson(const Table& t) {
    std::string o = "[\n";
    for (size_t r = 0; r < t.rows.size(); r++) {
        o += "  {";
        for (size_t c = 0; c < t.cols.size(); c++) {
            if (c) o += ", ";
            const std::string v = c < t.rows[r].size() ? t.rows[r][c] : std::string();
            o += jsonStr(t.cols[c]) + ": " + (isNumber(v) ? v : jsonStr(v));
        }
        o += r + 1 < t.rows.size() ? "},\n" : "}\n";
    }
    return o + "]\n";
}

std::string stamp() {
    char b[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(b, sizeof b, "%Y%m%d-%H%M", std::localtime(&now));
    return b;
}

static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

static int colOf(const Table& t, std::initializer_list<const char*> names) {
    for (const char* n : names)
        for (size_t i = 0; i < t.cols.size(); i++) if (lower(t.cols[i]) == n) return (int)i;
    return -1;
}
static std::string cell(const Table& t, size_t r, int c) { return c >= 0 && c < (int)t.rows[r].size() ? t.rows[r][c] : std::string(); }

std::string bandOf(double f) {
    static const struct { double lo, hi; const char* n; } b[] = {
        {1.8, 2, "160m"}, {3.5, 4, "80m"}, {5.3, 5.5, "60m"}, {7, 7.3, "40m"}, {10.1, 10.15, "30m"}, {14, 14.35, "20m"}, {18.068, 18.168, "17m"},
        {21, 21.45, "15m"}, {24.89, 24.99, "12m"}, {26.965, 27.405, "11m"}, {28, 29.7, "10m"}, {50, 54, "6m"}, {70, 71, "4m"}, {144, 148, "2m"}, {430, 440, "70cm"}, {1240, 1300, "23cm"}, {2300, 2450, "13cm"}, {10000, 10500, "3cm"}};
    for (const auto& x : b) if (f >= x.lo && f <= x.hi) return x.n;
    return "";
}

static double freqMhz(const Table& t, size_t r) {
    int c = colOf(t, {"frequency_hz", "freq_hz"});
    if (c >= 0) { const std::string s = cell(t, r, c); return s.empty() ? 0 : atof(s.c_str()) / 1e6; }
    c = colOf(t, {"frequency_mhz", "freq_mhz"});
    if (c >= 0) return atof(cell(t, r, c).c_str());
    c = colOf(t, {"frequency", "freq"});
    if (c >= 0) { double v = atof(cell(t, r, c).c_str()); return v > 1e5 ? v / 1e6 : v; }
    return 0;
}

static bool utc(double e, struct tm& g) {
    if (e <= 0) e = (double)std::time(nullptr);
    const std::time_t tt = (std::time_t)e;
#ifdef _WIN32
    return gmtime_s(&g, &tt) == 0;
#else
    return gmtime_r(&tt, &g) != nullptr;
#endif
}

static void adifField(std::string& o, const char* name, const std::string& v) {
    if (v.empty()) return;
    o += "<"; o += name; o += ":" + std::to_string(v.size()) + ">" + v + " ";
}

static std::string adifMode(const std::string& m) {
    const std::string l = lower(m);
    if (l.rfind("psk", 0) == 0) return "PSK";
    if (l == "js8" || l == "js8call") return "JS8";
    if (l == "ft8" || l == "ft4" || l == "wspr" || l == "rtty" || l == "cw" || l == "mfsk" || l == "ssb" || l == "fm" || l == "am") { std::string u = m; for (char& c : u) c = (char)toupper((unsigned char)c); return u; }
    std::string u = m; for (char& c : u) c = (char)toupper((unsigned char)c); return u;
}

std::string toAdif(const Table& t) {
    std::string o = "OnAir logbook export\n";
    adifField(o, "ADIF_VER", "3.1.4");
    adifField(o, "PROGRAMID", "OnAir");
    o += "<EOH>\n";
    const int cCall = colOf(t, {"call", "callsign", "call_sign"}), cGrid = colOf(t, {"grid", "locator", "gridsquare"}), cBand = colOf(t, {"band"}),
              cMode = colOf(t, {"mode"}), cRst = colOf(t, {"rst", "rst_rcvd", "rst_sent", "snr", "snr_db"}),
              cProp = colOf(t, {"prop_mode"}), cSat = colOf(t, {"sat_name"});
    if (cCall < 0) return o;
    for (size_t r = 0; r < t.rows.size(); r++) {
        const std::string call = cell(t, r, cCall);
        if (call.empty()) continue;
        struct tm g {};
        if (!utc(r < t.epoch.size() ? t.epoch[r] : 0, g)) continue;
        char d[16], tm[16];
        strftime(d, sizeof d, "%Y%m%d", &g);
        strftime(tm, sizeof tm, "%H%M%S", &g);
        const double mhz = freqMhz(t, r);
        std::string band = cell(t, r, cBand);
        if (band.empty()) band = bandOf(mhz);
        std::string rec;
        adifField(rec, "CALL", call);
        adifField(rec, "QSO_DATE", d);
        adifField(rec, "TIME_ON", tm);
        adifField(rec, "BAND", band);
        if (mhz > 0) adifField(rec, "FREQ", num(mhz, 6));
        adifField(rec, "MODE", adifMode(cell(t, r, cMode)));
        adifField(rec, "GRIDSQUARE", cell(t, r, cGrid));
        adifField(rec, "RST_RCVD", cell(t, r, cRst));
        adifField(rec, "PROP_MODE", cell(t, r, cProp));   // SAT for a satellite contact
        adifField(rec, "SAT_NAME", cell(t, r, cSat));
        o += rec + "<EOR>\n";
    }
    return o;
}

static std::string xmlEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        else o += (char)c;
    }
    return o;
}

struct Pt { std::string name, desc, lat, lon, ele, time; };

static std::vector<Pt> points(const Table& t) {
    std::vector<Pt> v;
    const int cLat = colOf(t, {"lat", "latitude"}), cLon = colOf(t, {"lon", "lng", "longitude"});
    if (cLat < 0 || cLon < 0) return v;
    const int cName = colOf(t, {"name", "callsign", "call", "icao", "mmsi", "id", "serial"}), cMode = colOf(t, {"mode"}),
              cAltFt = colOf(t, {"alt_ft"}), cAltM = colOf(t, {"alt_m"});
    for (size_t r = 0; r < t.rows.size(); r++) {
        const std::string la = cell(t, r, cLat), lo = cell(t, r, cLon);
        if (la.empty() || lo.empty()) continue;
        char* e1; char* e2;
        const double a = strtod(la.c_str(), &e1), b = strtod(lo.c_str(), &e2);
        if (*e1 || *e2 || a < -90 || a > 90 || b < -180 || b > 180) continue;
        Pt p;
        p.lat = num(a, 6); p.lon = num(b, 6);
        p.name = cell(t, r, cName);
        if (p.name.empty()) p.name = "point " + std::to_string(v.size() + 1);
        p.desc = cell(t, r, cMode);
        const std::string ft = cell(t, r, cAltFt), m = cell(t, r, cAltM);
        if (!m.empty()) p.ele = num(atof(m.c_str()), 1); else if (!ft.empty()) p.ele = num(atof(ft.c_str()) * 0.3048, 1);
        if (r < t.epoch.size() && t.epoch[r] > 0) { struct tm g {}; if (utc(t.epoch[r], g)) { char b2[32]; strftime(b2, sizeof b2, "%Y-%m-%dT%H:%M:%SZ", &g); p.time = b2; } }
        v.push_back(p);
    }
    return v;
}

std::string toKml(const Table& t) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n<name>OnAir</name>\n";
    for (const Pt& p : points(t)) {
        o += "<Placemark><name>" + xmlEsc(p.name) + "</name>";
        if (!p.desc.empty()) o += "<description>" + xmlEsc(p.desc) + "</description>";
        if (!p.time.empty()) o += "<TimeStamp><when>" + p.time + "</when></TimeStamp>";
        o += "<Point><coordinates>" + p.lon + "," + p.lat + (p.ele.empty() ? "" : "," + p.ele) + "</coordinates></Point></Placemark>\n";
    }
    return o + "</Document>\n</kml>\n";
}

std::string toGpx(const Table& t) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    for (const Pt& p : points(t)) {
        o += "<wpt lat=\"" + p.lat + "\" lon=\"" + p.lon + "\">";
        if (!p.ele.empty()) o += "<ele>" + p.ele + "</ele>";
        if (!p.time.empty()) o += "<time>" + p.time + "</time>";
        o += "<name>" + xmlEsc(p.name) + "</name>";
        if (!p.desc.empty()) o += "<desc>" + xmlEsc(p.desc) + "</desc>";
        o += "</wpt>\n";
    }
    return o + "</gpx>\n";
}

std::string toGeoJson(const Table& t) {
    std::vector<dect2::xport::GeoPoint> v;
    for (const Pt& p : points(t)) {
        dect2::xport::GeoPoint g;
        g.name = p.name; g.desc = p.desc; g.lat = atof(p.lat.c_str()); g.lon = atof(p.lon.c_str());
        if (!p.ele.empty()) { g.hasAlt = true; g.altM = atof(p.ele.c_str()); }
        v.push_back(g);
    }
    // times: the Pt strings are ISO already; the writer wants unix seconds, so carry them over from the table's epoch column
    size_t k = 0;
    const int cLat = colOf(t, {"lat", "latitude"}), cLon = colOf(t, {"lon", "lng", "longitude"});
    for (size_t r = 0; r < t.rows.size() && k < v.size(); r++) {
        const std::string la = cell(t, r, cLat), lo = cell(t, r, cLon);
        if (la.empty() || lo.empty()) continue;
        char* e1; char* e2;
        const double a = strtod(la.c_str(), &e1), b = strtod(lo.c_str(), &e2);
        if (*e1 || *e2 || a < -90 || a > 90 || b < -180 || b > 180) continue;
        if (r < t.epoch.size() && t.epoch[r] > 0) v[k].time = t.epoch[r];
        k++;
    }
    return dect2::xport::toGeoJson(v);
}

std::string render(const Table& t, Fmt f) {
    switch (f) {
        case Fmt::Json: return toJson(t);
        case Fmt::Adif: return toAdif(t);
        case Fmt::Kml: return toKml(t);
        case Fmt::Gpx: return toGpx(t);
        case Fmt::GeoJson: return toGeoJson(t);
        default: return toCsv(t);
    }
}

Table since(const Table& t, double t0) {
    Table o;
    o.cols = t.cols;
    for (size_t r = 0; r < t.rows.size(); r++)
        if (r < t.epoch.size() && t.epoch[r] >= t0 && t.epoch[r] > 0) o.add(t.rows[r], t.epoch[r]);
    return o;
}

Fmt fmtFromPath(const std::string& p) {
    const size_t d = p.rfind('.');
    const std::string e = d == std::string::npos ? "" : lower(p.substr(d));
    if (e == ".adi" || e == ".adif") return Fmt::Adif;
    if (e == ".kml") return Fmt::Kml;
    if (e == ".gpx") return Fmt::Gpx;
    if (e == ".geojson") return Fmt::GeoJson;
    if (e == ".json") return Fmt::Json;
    return Fmt::Csv;
}

const char* fmtExt(Fmt f) {
    switch (f) { case Fmt::Json: return ".json"; case Fmt::Adif: return ".adi"; case Fmt::Kml: return ".kml"; case Fmt::Gpx: return ".gpx"; case Fmt::GeoJson: return ".geojson"; default: return ".csv"; }
}

}  // namespace resexp
