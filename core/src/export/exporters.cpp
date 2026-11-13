// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/export/exporters.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace dect2::xport {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool isNumber(const std::string& s) {
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

bool utc(double e, struct tm& g) {
    if (!(e > 0) || e > 253402300799.0) return false;
    const std::time_t tt = (std::time_t)e;
#ifdef _WIN32
    return gmtime_s(&g, &tt) == 0;
#else
    return gmtime_r(&tt, &g) != nullptr;
#endif
}

std::vector<GeoPoint> validPoints(const std::vector<GeoPoint>& v) {
    std::vector<GeoPoint> o;
    for (const GeoPoint& p : v) if (validLatLon(p.lat, p.lon)) o.push_back(p);
    return o;
}

std::string coordKml(const GeoPoint& p) {
    return numText(p.lon, 6) + "," + numText(p.lat, 6) + (p.hasAlt && std::isfinite(p.altM) ? "," + numText(p.altM, 1) : "");
}

void adifField(std::string& o, const char* name, const std::string& v) {
    if (v.empty()) return;
    o += "<"; o += name; o += ":" + std::to_string(v.size()) + ">" + v + " ";
}

std::string cleanName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (std::isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '-';
    return o;
}
}  // namespace

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&apos;";
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;   // not allowed in XML 1.0 at all
        else o += (char)c;
    }
    return o;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

std::string csvCell(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string numText(double v, int dec) {
    if (!std::isfinite(v)) return "0";
    dec = std::max(0, std::min(dec, 9));
    char b[64];
    snprintf(b, sizeof b, "%.*f", dec, v);
    for (char* p = b; *p; p++) if (*p == ',') *p = '.';
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

std::string isoUtc(double t) {
    struct tm g {};
    if (!utc(t, g)) return {};
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

bool validLatLon(double lat, double lon) { return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180; }

std::string toKml(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks, const std::string& docName) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n<name>" + xmlEscape(docName) + "</name>\n";
    for (const GeoPoint& p : validPoints(points)) {
        o += "<Placemark>";
        if (!p.name.empty()) o += "<name>" + xmlEscape(p.name) + "</name>";
        if (!p.desc.empty()) o += "<description>" + xmlEscape(p.desc) + "</description>";
        const std::string t = isoUtc(p.time);
        if (!t.empty()) o += "<TimeStamp><when>" + t + "</when></TimeStamp>";
        o += "<Point>";
        if (p.hasAlt) o += "<altitudeMode>absolute</altitudeMode>";
        o += "<coordinates>" + coordKml(p) + "</coordinates></Point></Placemark>\n";
    }
    for (const GeoTrack& tr : tracks) {
        const std::vector<GeoPoint> v = validPoints(tr.pts);
        if (v.size() < 2) continue;
        bool alt = false;
        for (const GeoPoint& p : v) alt |= p.hasAlt;
        o += "<Placemark>";
        if (!tr.name.empty()) o += "<name>" + xmlEscape(tr.name) + "</name>";
        if (!tr.desc.empty()) o += "<description>" + xmlEscape(tr.desc) + "</description>";
        o += "<LineString>";
        if (alt) o += "<altitudeMode>absolute</altitudeMode>";
        o += "<coordinates>";
        for (size_t i = 0; i < v.size(); i++) {
            GeoPoint p = v[i];
            if (alt && !p.hasAlt) { p.hasAlt = true; p.altM = 0; }
            o += (i ? " " : "") + coordKml(p);
        }
        o += "</coordinates></LineString></Placemark>\n";
    }
    return o + "</Document>\n</kml>\n";
}

std::string toGpx(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks, const std::string& docName) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n<metadata><name>" + xmlEscape(docName) + "</name></metadata>\n";
    auto pt = [&](const char* tag, const GeoPoint& p) {
        std::string s = std::string("<") + tag + " lat=\"" + numText(p.lat, 6) + "\" lon=\"" + numText(p.lon, 6) + "\">";
        if (p.hasAlt && std::isfinite(p.altM)) s += "<ele>" + numText(p.altM, 1) + "</ele>";
        const std::string t = isoUtc(p.time);
        if (!t.empty()) s += "<time>" + t + "</time>";   // GPX order: ele, time, ..., name, desc
        if (!p.name.empty()) s += "<name>" + xmlEscape(p.name) + "</name>";
        if (!p.desc.empty()) s += "<desc>" + xmlEscape(p.desc) + "</desc>";
        return s + "</" + tag + ">";
    };
    for (const GeoPoint& p : validPoints(points)) o += pt("wpt", p) + "\n";
    for (const GeoTrack& tr : tracks) {
        const std::vector<GeoPoint> v = validPoints(tr.pts);
        if (v.empty()) continue;
        o += "<trk>";
        if (!tr.name.empty()) o += "<name>" + xmlEscape(tr.name) + "</name>";
        if (!tr.desc.empty()) o += "<desc>" + xmlEscape(tr.desc) + "</desc>";
        o += "<trkseg>";
        for (const GeoPoint& p : v) { GeoPoint q = p; q.name.clear(); q.desc.clear(); o += pt("trkpt", q); }
        o += "</trkseg></trk>\n";
    }
    return o + "</gpx>\n";
}

std::string toGeoJson(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks) {
    std::string o = "{\"type\":\"FeatureCollection\",\"features\":[";
    bool first = true;
    auto props = [&](const std::string& name, const std::string& desc, double time) {
        std::string s = "\"properties\":{";
        bool f = true;
        auto add = [&](const char* k, const std::string& v) { if (v.empty()) return; if (!f) s += ","; f = false; s += std::string("\"") + k + "\":\"" + jsonEscape(v) + "\""; };
        add("name", name); add("description", desc); add("time", isoUtc(time));
        return s + "}";
    };
    auto coord = [&](const GeoPoint& p) { return "[" + numText(p.lon, 6) + "," + numText(p.lat, 6) + (p.hasAlt && std::isfinite(p.altM) ? "," + numText(p.altM, 1) : "") + "]"; };
    for (const GeoPoint& p : validPoints(points)) {
        o += first ? "\n" : ",\n"; first = false;
        o += "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":" + coord(p) + "}," + props(p.name, p.desc, p.time) + "}";
    }
    for (const GeoTrack& tr : tracks) {
        const std::vector<GeoPoint> v = validPoints(tr.pts);
        if (v.size() < 2) continue;
        o += first ? "\n" : ",\n"; first = false;
        o += "{\"type\":\"Feature\",\"geometry\":{\"type\":\"LineString\",\"coordinates\":[";
        for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + coord(v[i]);
        o += "]}," + props(tr.name, tr.desc, 0) + "}";
    }
    return o + "\n]}\n";
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

std::string toJson(const Table& t) {
    std::string o = "[\n";
    for (size_t r = 0; r < t.rows.size(); r++) {
        o += "  {";
        for (size_t c = 0; c < t.cols.size(); c++) {
            if (c) o += ", ";
            const std::string v = c < t.rows[r].size() ? t.rows[r][c] : std::string();
            o += "\"" + jsonEscape(t.cols[c]) + "\": " + (isNumber(v) ? v : "\"" + jsonEscape(v) + "\"");
        }
        o += r + 1 < t.rows.size() ? "},\n" : "}\n";
    }
    return o + "]\n";
}

std::string toText(const Table& t) {
    // widths in characters (UTF-8 continuation bytes do not count)
    auto width = [](const std::string& s) { size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; };
    auto flat = [](std::string s) { for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' '; return s; };
    const bool timed = !t.epoch.empty();
    std::vector<size_t> w(t.cols.size(), 0);
    for (size_t c = 0; c < t.cols.size(); c++) w[c] = width(flat(t.cols[c]));
    for (const auto& r : t.rows) for (size_t c = 0; c < r.size() && c < w.size(); c++) w[c] = std::max(w[c], width(flat(r[c])));
    std::string o;
    auto line = [&](const std::string& when, const std::vector<std::string>& r) {
        if (timed) o += when + std::string(when.size() < 20 ? 20 - when.size() : 0, ' ') + "  ";
        for (size_t c = 0; c < w.size(); c++) {
            const std::string s = flat(c < r.size() ? r[c] : std::string());
            o += s;
            if (c + 1 < w.size()) o += std::string(w[c] - width(s) + 2, ' ');
        }
        while (!o.empty() && o.back() == ' ') o.pop_back();
        o += "\n";
    };
    line("UTC", t.cols);
    for (size_t i = 0; i < t.rows.size(); i++) line(i < t.epoch.size() ? isoUtc(t.epoch[i]) : std::string(), t.rows[i]);
    return o;
}

std::string toAdif(const std::vector<Qso>& q, const std::string& programId) {
    std::string o = "OnAir logbook export\n";
    adifField(o, "ADIF_VER", "3.1.4");
    adifField(o, "PROGRAMID", programId);
    o += "<EOH>\n";
    for (const Qso& x : q) {
        struct tm g {};
        if (x.call.empty() || !utc(x.timeOn, g)) continue;
        char d[16], tm[16];
        strftime(d, sizeof d, "%Y%m%d", &g);
        strftime(tm, sizeof tm, "%H%M%S", &g);
        std::string m = x.mode;
        for (char& c : m) c = (char)std::toupper((unsigned char)c);
        std::string rec;
        adifField(rec, "CALL", x.call);
        adifField(rec, "QSO_DATE", d);
        adifField(rec, "TIME_ON", tm);
        adifField(rec, "BAND", x.band);
        if (x.freqMHz > 0) adifField(rec, "FREQ", numText(x.freqMHz, 6));
        adifField(rec, "MODE", m);
        adifField(rec, "GRIDSQUARE", x.grid);
        adifField(rec, "RST_SENT", x.rstSent);
        adifField(rec, "RST_RCVD", x.rstRcvd);
        adifField(rec, "NAME", x.name);
        adifField(rec, "COMMENT", x.comment);
        adifField(rec, "PROP_MODE", x.propMode);
        adifField(rec, "SAT_NAME", x.satName);
        o += rec + "<EOR>\n";
    }
    return o;
}

bool FileSink::put(const ExportItem& item) {
    err_.clear();
    std::string name = item.name;
    const size_t sl = name.find_last_of("/\\");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    if (name.empty() || name == "." || name == "..") { err_ = "no file name"; return false; }
    const std::string path = dir_.empty() ? name : dir_ + "/" + name;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(u8file(tmp), std::ios::binary);
        if (!f) { err_ = "could not write " + path; return false; }
        f.write(item.data.data(), (std::streamsize)item.data.size());
        f.flush();
        if (!f) { err_ = "could not write " + path; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(u8file(tmp), u8file(path), ec);
    if (ec) { std::filesystem::remove(u8file(tmp), ec); err_ = "could not write " + path; return false; }
    lastPath = path;
    return true;
}

const char* formatExt(Format f) {
    switch (f) {
        case Format::Json: return ".json"; case Format::Txt: return ".txt"; case Format::Kml: return ".kml"; case Format::Gpx: return ".gpx";
        case Format::GeoJson: return ".geojson"; case Format::Adif: return ".adi"; default: return ".csv";
    }
}
const char* formatMime(Format f) {
    switch (f) {
        case Format::Json: return "application/json"; case Format::Txt: return "text/plain"; case Format::Kml: return "application/vnd.google-earth.kml+xml";
        case Format::Gpx: return "application/gpx+xml"; case Format::GeoJson: return "application/geo+json"; case Format::Adif: return "text/plain"; default: return "text/csv";
    }
}
Format formatFromPath(const std::string& p, Format def) {
    const size_t d = p.rfind('.');
    if (d == std::string::npos) return def;
    const std::string e = lower(p.substr(d));
    if (e == ".adi" || e == ".adif") return Format::Adif;
    if (e == ".kml") return Format::Kml;
    if (e == ".gpx") return Format::Gpx;
    if (e == ".geojson") return Format::GeoJson;
    if (e == ".json") return Format::Json;
    if (e == ".txt") return Format::Txt;
    if (e == ".csv") return Format::Csv;
    return def;
}
std::string safeFileName(const std::string& base, const std::string& stamp, Format f) {
    std::string b = cleanName(base);
    if (b.empty()) b = "onair";
    return b + (stamp.empty() ? "" : "-" + cleanName(stamp)) + formatExt(f);
}

bool exportGeo(ExportSink& sink, const std::string& base, const std::string& stamp, Format f, const std::vector<GeoPoint>& pts, const std::vector<GeoTrack>& tracks, std::string* why) {
    size_t n = validPoints(pts).size();
    for (const GeoTrack& t : tracks) if (validPoints(t.pts).size() >= 2) n++;
    if (!n) { if (why) *why = "nothing with a position to export"; return false; }
    ExportItem it;
    it.name = safeFileName(base, stamp, f);
    it.mime = formatMime(f);
    switch (f) {
        case Format::Kml: it.data = toKml(pts, tracks, base); break;
        case Format::Gpx: it.data = toGpx(pts, tracks, base); break;
        case Format::GeoJson: it.data = toGeoJson(pts, tracks); break;
        default: if (why) *why = "not a geographic format"; return false;
    }
    if (sink.put(it)) return true;
    if (why) *why = sink.error();
    return false;
}

bool exportTable(ExportSink& sink, const std::string& base, const std::string& stamp, Format f, const Table& t, std::string* why) {
    ExportItem it;
    it.name = safeFileName(base, stamp, f);
    it.mime = formatMime(f);
    switch (f) {
        case Format::Csv: it.data = toCsv(t); break;
        case Format::Json: it.data = toJson(t); break;
        case Format::Txt: it.data = toText(t); break;
        default: if (why) *why = "not a table format"; return false;
    }
    if (sink.put(it)) return true;
    if (why) *why = sink.error();
    return false;
}

bool exportQsos(ExportSink& sink, const std::string& base, const std::string& stamp, const std::vector<Qso>& q, std::string* why) {
    bool any = false;
    for (const Qso& x : q) any |= !x.call.empty();
    if (!any) { if (why) *why = "no contacts to export"; return false; }
    ExportItem it;
    it.name = safeFileName(base, stamp, Format::Adif);
    it.mime = formatMime(Format::Adif);
    it.data = toAdif(q);
    if (sink.put(it)) return true;
    if (why) *why = sink.error();
    return false;
}

}  // namespace dect2::xport
