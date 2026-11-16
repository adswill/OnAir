// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live Google Earth KML of the selected radiosonde: see sonde_kml.h.
#include "dect2/sonde/sonde_kml.h"
#include "dect2/export/exporters.h"
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace {

std::string num(double v, int dec) {
    if (!std::isfinite(v)) v = 0;
    char b[48]; std::snprintf(b, sizeof b, "%.*f", dec, v);
    for (char* p = b; *p; p++) if (*p == ',') *p = '.';
    return b;
}
std::string coord(double lat, double lon, double alt) { return num(lon, 6) + "," + num(lat, 6) + "," + num(alt, 1); }

std::string utcText(double t) {
    const time_t tt = (time_t)std::floor(t);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[48]; std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string style(const char* id, const char* lineCol, const char* fillCol, int width) {
    return std::string("<Style id=\"") + id + "\"><LineStyle><color>" + lineCol + "</color><width>" + std::to_string(width) + "</width></LineStyle><PolyStyle><color>" + fillCol + "</color><fill>1</fill><outline>0</outline></PolyStyle></Style>\n";
}

// a wall line: the path drawn down to the ground
std::string wall(const std::string& name, const char* styleId, const std::vector<SondeTrackPoint>& v, size_t i0, size_t i1) {
    if (i1 <= i0) return {};
    std::string o = "<Placemark><name>" + xport::xmlEscape(name) + "</name><styleUrl>#" + styleId + "</styleUrl><LineString><extrude>1</extrude><tessellate>1</tessellate><altitudeMode>absolute</altitudeMode><coordinates>";
    for (size_t i = i0; i <= i1; i++) o += (i > i0 ? " " : "") + coord(v[i].lat, v[i].lon, v[i].altM);
    return o + "</coordinates></LineString></Placemark>\n";
}

std::string pointMark(const std::string& name, const std::string& desc, double lat, double lon, double alt, bool hasAlt, const char* styleId) {
    std::string o = "<Placemark><name>" + xport::xmlEscape(name) + "</name>";
    if (!desc.empty()) o += "<description>" + xport::xmlEscape(desc) + "</description>";
    if (styleId) o += std::string("<styleUrl>#") + styleId + "</styleUrl>";
    o += "<Point>";
    if (hasAlt) o += "<extrude>1</extrude><altitudeMode>absolute</altitudeMode>";
    return o + "<coordinates>" + coord(lat, lon, hasAlt ? alt : 0) + "</coordinates></Point></Placemark>\n";
}

}  // namespace

std::string sondeLiveKml(const SondeInfo& s, const std::vector<sondecourse::Point>& pred, double nowUnix) {
    std::vector<SondeTrackPoint> tr = s.track;
    if (s.hasPos && std::isfinite(s.lat) && std::isfinite(s.lon)) {   // the newest position may be newer than the thinned track
        SondeTrackPoint p; p.lat = (float)s.lat; p.lon = (float)s.lon; p.altM = (float)s.altM; p.unixT = s.hasTime ? (uint32_t)s.unixTime : 0;
        if (tr.empty() || tr.back().lat != p.lat || tr.back().lon != p.lon) tr.push_back(p);
    }
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n<name>" + xport::xmlEscape(s.serial.empty() ? std::string("Radiosonde") : s.serial) + "</name>\n";
    o += "<description>" + xport::xmlEscape("OnAir live track, updated " + utcText(nowUnix)) + "</description>\n";
    o += style("up", "ff00c800", "5500c800", 3) + style("down", "ff2020ff", "552020ff", 3);   // KML colours are aabbggrr
    o += "<Style id=\"pred\"><LineStyle><color>ff00d7ff</color><width>3</width></LineStyle></Style>\n";
    o += "<Style id=\"mark\"><IconStyle><scale>1.1</scale></IconStyle></Style>\n";
    if (tr.empty()) return o + "</Document>\n</kml>\n";
    size_t top = 0;
    for (size_t i = 0; i < tr.size(); i++) if (tr[i].altM > tr[top].altM) top = i;
    const bool burst = top + 1 < tr.size() && tr[top].altM - tr.back().altM > 1000;
    const std::string id = s.serial;
    o += wall(id + " ascent", "up", tr, 0, top);
    o += wall(id + " descent", "down", tr, top, tr.size() - 1);
    if (burst) o += pointMark(id + " burst", num(tr[top].altM, 0) + " m", tr[top].lat, tr[top].lon, tr[top].altM, true, "mark");
    {   // where it is now
        std::string d = "Altitude " + num(s.altM, 0) + " m";
        if (s.hasVel) d += "\nClimb rate " + std::string(s.vSpeed >= 0 ? "+" : "") + num(s.vSpeed, 1) + " m/s";
        if (s.hasTime) d += "\nTime " + utcText(s.unixTime);
        if (!s.type.empty()) d += "\nType " + s.type;
        if (s.hasPos) o += pointMark(id + " " + num(s.altM, 0) + " m", d, s.lat, s.lon, s.altM, true, "mark");
    }
    if (pred.size() >= 2) {   // the predicted course as a dashed line (KML has no dash style: every second segment is left out)
        o += "<Placemark><name>" + xport::xmlEscape(id + " predicted course") + "</name><styleUrl>#pred</styleUrl><MultiGeometry>";
        for (size_t i = 0; i + 1 < pred.size(); i += 2)
            o += "<LineString><tessellate>1</tessellate><altitudeMode>absolute</altitudeMode><coordinates>" + coord(pred[i].lat, pred[i].lon, pred[i].altM) + " " + coord(pred[i + 1].lat, pred[i + 1].lon, pred[i + 1].altM) + "</coordinates></LineString>";
        o += "</MultiGeometry></Placemark>\n";
        const sondecourse::Point& l = pred.back();
        std::string d = "Predicted landing";
        d += "\nIn " + std::to_string((int)l.t / 60) + " min " + std::to_string((int)l.t % 60) + " s";
        if (nowUnix > 0) d += "\nAbout " + utcText(nowUnix + l.t);
        o += pointMark(id + " predicted landing", d, l.lat, l.lon, 0, false, "mark");
    }
    return o + "</Document>\n</kml>\n";
}

std::string sondeNetworkLinkKml(const std::string& hrefUrl, int refreshS, const std::string& name) {
    if (refreshS < 1) refreshS = 5;
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<NetworkLink>\n<name>" + xport::xmlEscape(name) + "</name>\n<Link>\n<href>" + xport::xmlEscape(hrefUrl) +
           "</href>\n<refreshMode>onInterval</refreshMode>\n<refreshInterval>" + std::to_string(refreshS) + "</refreshInterval>\n</Link>\n</NetworkLink>\n</kml>\n";
}

std::string fileUrlOfPath(const std::string& path) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    std::string o = "file://";
    if (p.empty() || p[0] != '/') o += "/";
    for (unsigned char c : p) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[8]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

}  // namespace dect2
