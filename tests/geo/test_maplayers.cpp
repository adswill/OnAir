// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Map layers: result lists to markers, satellite footprint and ground track, circle geometry
#include "dect2/geo/maplayers.h"
#include "dect2/geo/maidenhead.h"
#include <cmath>
#include <cstdio>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    {   // ADS-B: only aircraft with a position; callsign or address as label
        AdsbTelemetry t;
        AdsbAircraft a; a.icao = 0x4840D6; a.callsign = "KLM1023"; a.hasPos = true; a.lat = 52.3; a.lon = 4.7; a.hasAlt = true; a.altFt = 10000;
        a.hasSpeed = true; a.speedKt = 250; a.hasHeading = true; a.headingDeg = 90; a.ageSec = 7;
        a.track.push_back({52.2f, 4.5f}); a.track.push_back({52.3f, 4.7f});
        AdsbAircraft b; b.icao = 0xABCDEF; b.hasPos = true; b.lat = 1; b.lon = 2;
        AdsbAircraft c; c.icao = 1;   // no position
        t.aircraft = {a, b, c};
        auto m = markersFromAdsb(t);
        CHECK(m.size() == 2, "two placed: %zu", m.size());
        CHECK(m[0].label == "KLM1023" && m[0].id == "4840D6" && m[0].layer == LayerAdsb, "label %s id %s", m[0].label.c_str(), m[0].id.c_str());
        CHECK(m[0].hasAlt && std::fabs(m[0].altM - 3048) < 0.5 && m[0].hasSpeed && m[0].headingDeg == 90 && m[0].track.size() == 2, "fields");
        CHECK(m[1].label == "ABCDEF" && !m[1].hasAlt && m[1].headingDeg == -1, "address label");
        CHECK(std::fabs(m[0].ageSec - 7) < 1e-9, "age");
    }
    {   // AIS
        AisTelemetry t;
        AisVessel v; v.mmsi = 244123456; v.name = "NORDLYS"; v.hasPos = true; v.lat = 55; v.lon = 12; v.sog = 12.5f; v.cog = 80; v.ageSec = 90; v.destination = "ROTTERDAM";
        AisVessel w; w.mmsi = 1; w.hasPos = false;
        AisVessel x; x.mmsi = 257000001; x.hasPos = true; x.lat = 60; x.lon = 5; x.sog = -1; x.cog = -1; x.heading = 45;
        t.vessels = {v, w, x};
        auto m = markersFromAis(t);
        CHECK(m.size() == 2, "ais placed %zu", m.size());
        CHECK(m[0].label == "NORDLYS" && m[0].id == "244123456" && m[0].hasSpeed && m[0].speedKt == 12.5 && m[0].headingDeg == 80, "ship fields (cog used when no heading)");
        CHECK(m[0].detail == "to ROTTERDAM", "detail");
        CHECK(m[1].label == "257000001" && !m[1].hasSpeed && m[1].headingDeg == 45, "mmsi label");
    }
    {   // VDES: the latest AIS position per MMSI, frames without a position skipped
        VdesTelemetry t; t.timeSec = 100;
        VdesAsm a; a.hasPos = true; a.lat = 52.5; a.lon = 5.25; a.aisMmsi = 244123456; a.aisType = 1; a.timeSec = 90; a.sogKt = 12.5f; a.cogDeg = 80;
        VdesAsm old = a; old.lat = 52.0; old.timeSec = 50;
        VdesAsm np; np.aisMmsi = 999; np.timeSec = 95;
        t.asms = {np, a, old};
        auto m = markersFromVdes(t);
        CHECK(m.size() == 1 && m[0].layer == LayerAis && m[0].id == "244123456" && m[0].lat == 52.5 && m[0].hasSpeed && m[0].headingDeg == 80 && std::fabs(m[0].ageSec - 10) < 1e-9, "vdes marker");
    }
    {   // APRS: age from signal time
        PacketTelemetry t; t.timeSec = 1000;
        PacketStation s; s.call = "LY2XX-9"; s.hasPos = true; s.lat = 54.7; s.lon = 25.3; s.lastHeardSec = 940; s.hasSpeed = true; s.speedKnots = 30; s.hasCourse = true; s.courseDeg = 270; s.comment = "mobile";
        PacketStation n; n.call = "NOPOS";
        t.stations = {s, n};
        auto m = markersFromAprs(t);
        CHECK(m.size() == 1 && m[0].label == "LY2XX-9" && std::fabs(m[0].ageSec - 60) < 1e-9 && m[0].headingDeg == 270 && m[0].detail == "mobile", "aprs");
    }
    {   // radiosonde
        SondeTelemetry t;
        SondeInfo s; s.serial = "V1234567"; s.type = "RS41"; s.hasPos = true; s.lat = 54; s.lon = 25; s.altM = 12000; s.hasVel = true; s.hSpeed = 10; s.vSpeed = -5; s.headingDeg = 45; s.lastHeardS = 3;
        SondeTrackPoint p; p.lat = 53.9f; p.lon = 24.9f; s.track.push_back(p);
        SondeInfo n; n.serial = "NOPOS";
        t.sondes = {s, n};
        auto m = markersFromSonde(t);
        CHECK(m.size() == 1 && m[0].id == "V1234567" && m[0].hasAlt && m[0].altM == 12000 && std::fabs(m[0].speedKt - 19.44) < 0.01 && m[0].track.size() == 1, "sonde");
        CHECK(m[0].detail == "RS41, falling", "detail %s", m[0].detail.c_str());
    }
    {   // age text
        CHECK(ageText(12) == "12 s" && ageText(180) == "3 min" && ageText(7500) == "2 h 5 min" && ageText(-5) == "0 s", "age text");
    }
    {   // footprint: known values
        CHECK(footprintRadiusKm(0) == 0, "no altitude");
        const double r = footprintRadiusKm(400);   // ISS: about 2200 km horizon radius
        CHECK(r > 2150 && r < 2300, "ISS horizon radius %.0f", r);
        CHECK(footprintRadiusKm(400, 10) < r && footprintRadiusKm(400, 10) > 1200, "10 deg mask smaller: %.0f", footprintRadiusKm(400, 10));
        CHECK(footprintRadiusKm(800) > r, "higher sees more");
        const double g = footprintRadiusKm(35786);   // geostationary: about 9000 km
        CHECK(g > 9000 && g < 9300, "geo %.0f", g);
    }
    {   // circle: every point at the radius from the centre; crossing the date line wraps
        auto c = circlePoints(54.7, 25.3, 500, 60);
        CHECK(c.size() == 60, "n");
        double worst = 0;
        for (auto& p : c) { double km, b; greatCircle(54.7, 25.3, p.first, p.second, km, b); worst = std::fmax(worst, std::fabs(km - 500)); }
        CHECK(worst < 2, "radius error %.2f km", worst);
        auto d = circlePoints(0, 179, 500, 36);
        bool ok = true; for (auto& p : d) if (p.second < -180 || p.second > 180) ok = false;
        CHECK(ok, "longitude wrapped");
        auto pole = circlePoints(89, 0, 500, 36);
        CHECK(pole.size() == 36, "pole");
    }
    {   // satellite
        Tle t;
        CHECK(parseTle("1 06251U 62025E   06176.82412014  .00008885  00000-0  12808-3 0  3985",
                       "2 06251  58.0579  54.0425 0030035 139.1568 221.1854 15.56387291  6774", t, "SAT"), "tle");
        Sgp4 s; CHECK(s.init(t), "init");
        const double now = jdToUnix(t.epochJd) + 600;
        SatMapPoint p;
        CHECK(satelliteMapPoint(s, now, 10, 30, 1, p), "map point");
        CHECK(p.name == "SAT" && p.norad == 6251, "name");
        CHECK(std::fabs(p.lat) <= 58.2 && p.lon >= -180 && p.lon <= 180 && p.altKm > 100 && p.altKm < 1000, "sub-point %.1f %.1f %.0f", p.lat, p.lon, p.altKm);
        CHECK(p.footprintKm > 1500 && p.footprintKm < 3500, "footprint %.0f", p.footprintKm);
        CHECK(p.track.size() == 41, "track points %zu", p.track.size());
        CHECK(p.nowIndex == 10 && std::fabs(p.track[p.nowIndex].first - p.lat) < 0.01 && std::fabs(p.track[p.nowIndex].second - p.lon) < 0.01, "now index %zu", p.nowIndex);
        Sgp4 bad; CHECK(!satelliteMapPoint(bad, now, 1, 1, 1, p), "uninitialised propagator");
        CHECK(!satelliteMapPoint(s, now, 1, 1, 0, p), "zero step");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("maplayers OK\n");
    return 0;
}
