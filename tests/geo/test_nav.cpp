// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Car navigation maths (core/include/dect2/geo/nav.h): route progress, off-route, arrival, dead reckoning, GPX round trips.
#include "dect2/geo/nav.h"
#include <cstdio>
using namespace dect2::geo;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // a route 0.02 deg east then 0.02 deg north at 55N (about 1.28 km, then 2.2 km)
    Route r; r.name = "A & B";
    r.pts = {{55.0, 24.0}, {55.0, 24.02}, {55.02, 24.02}};
    const double len = routeLengthM(r);
    CHECK(len > 3300 && len < 3600, "length %.0f", len);

    NavProgress p = navProgress(r, {55.0, 24.01}, 10, 90, true, 3, -1);
    CHECK(p.valid && p.seg == 0 && p.next == 1, "seg %d next %d", p.seg, p.next);
    CHECK(p.crossM < 2 && !p.offRoute, "cross %.1f", p.crossM);
    CHECK(std::fabs(p.bearingToNext - 90) < 1, "bearing %.1f", p.bearingToNext);
    CHECK(std::fabs(p.turnDeg) < 1, "turn %.1f", p.turnDeg);
    CHECK(std::fabs(p.leftM - (len - p.alongM)) < 1e-6 && p.etaS > 0, "left/eta");

    // 200 m north of the first leg: off route, and the second leg is not mistaken for it
    p = navProgress(r, {55.0018, 24.01}, 10, 90, true, 3, -1);
    CHECK(p.offRoute && p.crossM > 150, "off route cross %.0f", p.crossM);
    // a poor fix widens the limit
    p = navProgress(r, {55.0004, 24.01}, 10, 90, true, 30, -1);
    CHECK(!p.offRoute, "sigma 30 m: 45 m off is not off route");

    // past the corner: second leg, turn left of north for a heading east is -90 (left) -> bearing 0, course 90 => -90
    p = navProgress(r, {55.005, 24.02}, 10, 90, true, 3, 0);
    CHECK(p.seg == 1 && p.next == 2, "second leg seg %d", p.seg);
    CHECK(std::fabs(p.turnDeg + 90) < 2, "turn %.1f", p.turnDeg);

    // arrival
    p = navProgress(r, {55.02, 24.02}, 0, 0, false, 3, 1);
    CHECK(p.arrived && p.etaS < 0, "arrived");

    // a waypoint reached moves on to the next one
    p = navProgress(r, {55.0, 24.02}, 10, 90, true, 3, 0);
    CHECK(p.next == 2, "next after reaching the corner: %d", p.next);

    // dead reckoning: 20 m/s due east, fixes each second, then the fix stops
    DeadReckoner d;
    LatLon f{55, 24};
    d.onFix(0, f, 3, true, 20, 90);
    CHECK(d.state == DeadReckoner::Fix, "fix state");
    d.tick(3);
    CHECK(d.state == DeadReckoner::Coasting, "coasting");
    const double moved = navDistM(f, d.pos);
    CHECK(std::fabs(moved - 60) < 1.5, "coasted %.1f m in 3 s", moved);
    CHECK(std::fabs(navBearing(f, d.pos) - 90) < 0.5, "coast direction");
    d.tick(11);
    CHECK(d.state == DeadReckoner::NoFix, "dropped after the coast limit");
    d.onFix(20, {55, 24.001}, 3, true, 0, 0);
    CHECK(d.state == DeadReckoner::Fix && std::fabs(d.pos.lon - 24.001) < 1e-9, "restart takes the fix as is");
    // speed and course worked out from the move when the fix has no velocity
    DeadReckoner e;
    e.onFix(0, {55, 24}, 3, false, 0, 0);
    e.onFix(1, navAhead({55, 24}, 0, 15), 3, false, 0, 0);
    CHECK(std::fabs(e.speedMps - 15) < 1 && e.courseKnown && (e.courseDeg < 1 || e.courseDeg > 359), "derived speed %.1f course %.1f", e.speedMps, e.courseDeg);

    // GPX: route round trip with an ampersand in the name, a track with a timestamp, waypoints
    Route q;
    CHECK(parseGpx(gpxRoute(r), q) && q.pts.size() == 3 && std::fabs(q.pts[1].lon - 24.02) < 1e-7, "route round trip");
    TripLog log;
    CHECK(log.add(55, 24, 10, 1700000000, 5), "first point");
    CHECK(!log.add(55, 24.00001, 10, 1700000001, 5), "1 m move is not logged");
    CHECK(log.add(55, 24.001, 10, 1700000002, 5), "70 m move is logged");
    CHECK(log.add(55, 24.00101, 10, 1700000010, 5), "8 s gap is logged");
    const std::string gx = gpxTrack(log, "Trip <1>");
    CHECK(gx.find("2023-11-14T22:13:20Z") != std::string::npos, "time stamp");
    CHECK(gx.find("Trip &lt;1&gt;") != std::string::npos, "name escaped");
    CHECK(parseGpx(gx, q) && q.pts.size() == 3, "track parse %zu", q.pts.size());
    CHECK(parseGpx("<gpx><wpt lat=\"1.5\" lon=\"2.5\"><name>x</name></wpt><wpt lon='4' lat='3'/></gpx>", q) && q.pts.size() == 2 && q.pts[1].lat == 3, "waypoints, attribute order, quotes");
    CHECK(!parseGpx("<gpx><wpt lat=\"99\" lon=\"2\"/></gpx>", q), "out-of-range latitude rejected");
    CHECK(!parseGpx("not xml", q), "garbage rejected");
    // thinning
    std::string big = "<gpx><trk><trkseg>";
    char b[80];
    for (int i = 0; i < 10000; i++) { snprintf(b, sizeof b, "<trkpt lat=\"%.5f\" lon=\"24\"/>", 50 + i * 1e-4); big += b; }
    big += "</trkseg></trk></gpx>";
    CHECK(parseGpx(big, q, 4000) && q.pts.size() <= 4001 && std::fabs(q.pts.back().lat - (50 + 9999e-4)) < 1e-6, "thinned to %zu", q.pts.size());

    CHECK(navDistText(42) == "40 m" || navDistText(42) == "45 m", "dist text %s", navDistText(42).c_str());
    CHECK(navDistText(2500) == "2.5 km" && navEtaText(3600 + 600) == "1 h 10 min" && navEtaText(-1) == "-", "texts");
    if (!fails) std::printf("PASS nav\n");
    return fails ? 1 : 0;
}
