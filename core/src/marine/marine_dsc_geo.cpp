// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/marine_dsc_geo.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>

namespace dect2 {
namespace marine {

namespace {
std::string xml(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) { case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '"': o += "&quot;"; break; default: o += c; }
    }
    return o;
}
std::string iso(int64_t t) {
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
std::string label(const DscFix& f, const ShipDb& db) {
    const std::string n = db.name(f.mmsi);
    return n.empty() ? f.mmsi : f.mmsi + " " + n;
}
}

std::vector<DscFix> dscFixes(const std::vector<DscCall>& calls) {
    std::vector<DscFix> r;
    dscAddFixes(r, calls);
    return r;
}

size_t dscAddFixes(std::vector<DscFix>& all, const std::vector<DscCall>& calls, size_t cap) {
    size_t added = 0;
    for (const DscCall& c : calls) {
        if (!c.hasPos || !c.eccOk) continue;
        DscFix f;
        f.mmsi = !c.distressMmsi.empty() ? c.distressMmsi : c.fromMmsi;
        if (f.mmsi.empty() || f.mmsi.find('~') != std::string::npos) continue;
        f.lat = c.lat; f.lon = c.lon; f.time = c.rxTime; f.distress = c.distress;
        f.note = c.kind.empty() ? c.formatName : c.kind;
        if (!c.natureName.empty()) f.note += ": " + c.natureName;
        bool dup = false;
        for (size_t i = all.size(); i-- > 0 && !dup;) {
            const DscFix& o = all[i];
            if (o.time + 3600 < f.time) break;
            dup = o.mmsi == f.mmsi && o.time == f.time && o.lat == f.lat && o.lon == f.lon;
        }
        if (dup) continue;
        all.push_back(f);
        added++;
    }
    if (added) std::stable_sort(all.begin(), all.end(), [](const DscFix& a, const DscFix& b) { return a.time < b.time; });
    if (all.size() > cap) all.erase(all.begin(), all.begin() + (long)(all.size() - cap));
    return added;
}

std::string dscKml(const std::vector<DscFix>& fixes, const ShipDb& db) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n<name>OnAir DSC</name>\n";
    char b[96];
    std::map<std::string, std::vector<const DscFix*>> by;
    for (const DscFix& f : fixes) by[f.mmsi].push_back(&f);
    for (const DscFix& f : fixes) {
        snprintf(b, sizeof b, "%.6f,%.6f,0", f.lon, f.lat);
        o += "<Placemark><name>" + xml(label(f, db)) + (f.distress ? " DISTRESS" : "") + "</name><description>" + xml(f.note) + "</description><TimeStamp><when>" + iso(f.time) +
             "</when></TimeStamp><Point><coordinates>" + b + "</coordinates></Point></Placemark>\n";
    }
    for (const ShipRec& r : db.positioned()) {
        snprintf(b, sizeof b, "%.6f,%.6f,0", r.lon, r.lat);
        o += "<Placemark><name>" + xml(r.mmsi + " " + db.name(r.mmsi)) + "</name><description>" + xml(r.type.empty() ? std::string("station") : r.type) + "</description><Point><coordinates>" + b + "</coordinates></Point></Placemark>\n";
    }
    for (const auto& kv : by) {
        if (kv.second.size() < 2) continue;
        o += "<Placemark><name>" + xml(kv.first + " track") + "</name><LineString><tessellate>1</tessellate><coordinates>";
        for (const DscFix* f : kv.second) { snprintf(b, sizeof b, "%.6f,%.6f,0 ", f->lon, f->lat); o += b; }
        o += "</coordinates></LineString></Placemark>\n";
    }
    return o + "</Document>\n</kml>\n";
}

std::string dscGpx(const std::vector<DscFix>& fixes, const ShipDb& db) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    char b[96];
    for (const DscFix& f : fixes) {
        snprintf(b, sizeof b, "<wpt lat=\"%.6f\" lon=\"%.6f\">", f.lat, f.lon);
        o += b;
        o += "<time>" + iso(f.time) + "</time><name>" + xml(label(f, db)) + "</name><desc>" + xml(f.note) + "</desc></wpt>\n";
    }
    for (const ShipRec& r : db.positioned()) {
        snprintf(b, sizeof b, "<wpt lat=\"%.6f\" lon=\"%.6f\">", r.lat, r.lon);
        o += b;
        o += "<name>" + xml(r.mmsi + " " + db.name(r.mmsi)) + "</name><desc>station</desc></wpt>\n";
    }
    std::map<std::string, std::vector<const DscFix*>> by;
    for (const DscFix& f : fixes) by[f.mmsi].push_back(&f);
    for (const auto& kv : by) {
        if (kv.second.size() < 2) continue;
        o += "<trk><name>" + xml(kv.first) + "</name><trkseg>";
        for (const DscFix* f : kv.second) { snprintf(b, sizeof b, "<trkpt lat=\"%.6f\" lon=\"%.6f\"><time>", f->lat, f->lon); o += b; o += iso(f->time) + "</time></trkpt>"; }
        o += "</trkseg></trk>\n";
    }
    return o + "</gpx>\n";
}

} // namespace marine
} // namespace dect2
