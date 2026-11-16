// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The live Google Earth KML of the selected sonde (dect2/sonde/sonde_kml.h): well-formed XML, the extruded path with every track point,
// the escaped serial, the current position, the burst, the dashed predicted course and landing, and the NetworkLink file.
#include "dect2/common/xmlmini.h"
#include "dect2/sonde/sonde_kml.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>

using namespace dect2;

static size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + what.size())) n++;
    return n;
}

int main() {
    SondeInfo s; s.type = "RS41"; s.serial = "A<B&\"C'1";   // awkward on purpose: the serial must be escaped
    for (int t = 0; t <= 200; t++) {   // 201 points: up to 30 km at t = 150, then down
        SondeTrackPoint p;
        p.altM = (float)(t <= 150 ? 100 + 200.0 * t : 30100 - (t - 150) * 400.0); p.lat = (float)(54.6 + t * 1e-3); p.lon = (float)(25.3 + t * 2e-3); p.unixT = 1'800'000'000u + (uint32_t)t * 30;
        s.track.push_back(p);
    }
    s.trackIncluded = true; s.hasPos = true; s.lat = s.track.back().lat; s.lon = s.track.back().lon; s.altM = s.track.back().altM;
    s.hasVel = true; s.vSpeed = -13.4; s.hSpeed = 8; s.headingDeg = 90; s.hasTime = true; s.unixTime = s.track.back().unixT;
    const std::vector<sondecourse::Point> course = sondecourse::build(s.lat, s.lon, s.altM, 120, s.lat + 0.05, s.lon + 0.1, 900, {{s.lat + 0.01, s.lon + 0.02}, {s.lat + 0.02, s.lon + 0.04}, {s.lat + 0.03, s.lon + 0.06}});
    const std::string k = sondeLiveKml(s, course, 1'800'006'000.0);
    CHECK(parseXml(k) != nullptr, "the KML is well-formed XML");
    CHECK(k.find("A<B") == std::string::npos && k.find("A&lt;B&amp;&quot;C&apos;1") != std::string::npos, "the serial is escaped");
    CHECK(count(k, "<extrude>1</extrude>") >= 3, "extruded: ascent, descent, current position (%zu)", count(k, "<extrude>1</extrude>"));
    CHECK(count(k, "<tessellate>1</tessellate>") >= 3 && count(k, "<altitudeMode>absolute</altitudeMode>") >= 4, "tessellate and absolute altitude");
    CHECK(k.find("<PolyStyle>") != std::string::npos && k.find("ff00c800") != std::string::npos && k.find("ff2020ff") != std::string::npos, "ground fill, green up and red down");
    // N points: the ascent holds points 0..150 (151), the descent 150..200 (51); the current position is the last track point, not added twice
    const size_t up = k.find("ascent"), down = k.find("descent");
    CHECK(up != std::string::npos && down != std::string::npos && up < down, "ascent before descent");
    auto coordCount = [&](size_t from) {   // the points of the first <coordinates> after `from`
        const size_t a = k.find("<coordinates>", from), b = k.find("</coordinates>", a);
        size_t n = 1;
        for (size_t i = a; i < b; i++) if (k[i] == ' ') n++;
        return n;
    };
    CHECK(coordCount(up) == 151 && coordCount(down) == 51, "ascent has 151 points, descent 51 (%zu, %zu)", coordCount(up), coordCount(down));
    CHECK(k.find("burst") != std::string::npos && k.find("30100 m") != std::string::npos, "the burst point");
    CHECK(k.find("Climb rate -13.4 m/s") != std::string::npos && k.find("Altitude 10100 m") != std::string::npos && k.find("UTC") != std::string::npos, "the current position text");
    CHECK(k.find("predicted course") != std::string::npos && k.find("<MultiGeometry>") != std::string::npos && k.find("predicted landing") != std::string::npos, "predicted course and landing");
    // dashed: a course of 5 points gives segments 0-1 and 2-3 (4 left out)
    CHECK(course.size() == 5 && count(k.substr(k.find("<MultiGeometry>")), "<LineString>") == 2, "dashes (%zu)", count(k.substr(k.find("<MultiGeometry>")), "<LineString>"));
    // no prediction, no track: still valid
    const std::string k2 = sondeLiveKml(s, {}, 1'800'006'000.0);
    CHECK(parseXml(k2) != nullptr && k2.find("predicted") == std::string::npos, "no prediction");
    SondeInfo none;
    CHECK(parseXml(sondeLiveKml(none, {}, 0)) != nullptr, "an empty sonde is still valid KML");
    // the NetworkLink
    const std::string url = fileUrlOfPath("/Users/me/On Air/onair-sonde-live.kml");
    CHECK(url == "file:///Users/me/On%20Air/onair-sonde-live.kml", "url %s", url.c_str());
    CHECK(fileUrlOfPath("C:\\Users\\me\\x.kml") == "file:///C:/Users/me/x.kml", "windows path");
    const std::string nl = sondeNetworkLinkKml(url, 5, "OnAir <sonde>");
    auto root = parseXml(nl);
    CHECK(root != nullptr, "network link is well-formed");
    CHECK(nl.find("<NetworkLink>") != std::string::npos && nl.find("<refreshMode>onInterval</refreshMode>") != std::string::npos && nl.find("<refreshInterval>5</refreshInterval>") != std::string::npos && nl.find("<href>file:///Users/me/On%20Air/onair-sonde-live.kml</href>") != std::string::npos, "network link fields");
    return testSummary("sonde_kml");
}
