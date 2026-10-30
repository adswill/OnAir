// The aircraft side of the Inmarsat all-carriers mode (inmsat_planes.h, aero_pos.h): positions from ADS-C groups and text reports, bad and wrapped
// coordinates, merging the aircraft of several windows, age and fading, search and sorting, hundreds of aircraft, the date line, the message history.
// Synthetic messages only.
#include "dect2/aero/aero_adsc.h"
#include "dect2/aero/aero_pos.h"
#include "dect2/inmsat/inmsat_planes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/test_util.h"

static AeroAircraft plane(uint32_t aes, const char* reg, const char* flight, bool pos, double lat, double lon, int64_t posWall, int64_t lastWall = 0) {
    AeroAircraft a;
    a.aesId = aes; a.registration = reg; a.flight = flight;
    a.hasPos = pos; a.lat = lat; a.lon = lon; a.hasAlt = pos; a.altFt = 35000; a.posWall = posWall; a.posSource = 1;
    a.lastWall = lastWall ? lastWall : posWall;
    a.messages = 3; a.positions = pos ? 1 : 0;
    if (pos) a.track.push_back({lat, lon, 35000, 0});
    return a;
}

int main() {
    {   // ADS-C basic report groups through the same path the receiver uses
        const std::string txt = aeroBuildAdscText("LPAFAYA", "VQ-BPJ", aeroAdscBasicGroup(50.34, 16.38, 37000, 396));
        AeroPosition p;
        CHECK(aeroPositionFromMessage("B6", txt, false, p) && p.valid && p.source == 1 && std::fabs(p.lat - 50.34) < 0.01 && std::fabs(p.lon - 16.38) < 0.01 && p.altFt == 37000, "ADS-C group");
        double la = p.lat, lo = p.lon;
        CHECK(aeroSanitizePosition(la, lo), "good position kept");
        // southern and western hemispheres
        const std::string t2 = aeroBuildAdscText("LPAFAYA", "ZS-XYZ", aeroAdscBasicGroup(-33.9, -70.6, 28000, 100));
        AeroPosition q;
        CHECK(aeroPositionFromMessage("B6", t2, false, q) && q.lat < -33 && q.lon < -70, "southern / western ADS-C %.2f %.2f", q.lat, q.lon);
    }
    {   // text reports: ARINC 702 POS, label 20, label 16, label 12; and bad ones
        AeroPosition p;
        CHECK(aeroParseTextPosition("H1", "#M1BPOSN43312W123174,EASON,215754,370,", p) && std::fabs(p.lat - 43.52) < 0.01 && p.lon < -123, "H1 POS");
        CHECK(aeroParseTextPosition("20", "POSN38160W077075,TYSON,133411,370", p) && std::fabs(p.lat - 38.16) < 0.01, "label 20");
        CHECK(aeroParseTextPosition("16", "N 44.203,W 86.546,31965,6, 290", p) && p.altFt == 31965, "label 16");
        CHECK(aeroParseTextPosition("12", "N 28.177/W 96.055", p) && std::fabs(p.lon + 96.055) < 0.001, "label 12");
        CHECK(!aeroParseTextPosition("H1", "POSN99000W123174,", p), "latitude 99 refused");
        CHECK(!aeroParseTextPosition("H1", "no position here", p), "no position");
    }
    {   // out of range, wrap-around and no fix
        double la, lo;
        la = 91; lo = 10; CHECK(!aeroSanitizePosition(la, lo), "lat 91");
        la = -90.5; lo = 10; CHECK(!aeroSanitizePosition(la, lo), "lat -90.5");
        la = NAN; lo = 10; CHECK(!aeroSanitizePosition(la, lo), "nan");
        la = 10; lo = INFINITY; CHECK(!aeroSanitizePosition(la, lo), "inf");
        la = 0; lo = 0; CHECK(!aeroSanitizePosition(la, lo), "0,0 no fix");
        la = 20; lo = 190; CHECK(aeroSanitizePosition(la, lo) && std::fabs(lo + 170) < 1e-9, "190 E is 170 W: %.1f", lo);
        la = 20; lo = -185; CHECK(aeroSanitizePosition(la, lo) && std::fabs(lo - 175) < 1e-9, "185 W is 175 E: %.1f", lo);
        la = 20; lo = 400; CHECK(!aeroSanitizePosition(la, lo), "lon 400");
        la = 90; lo = 180; CHECK(aeroSanitizePosition(la, lo), "pole and the date line are fine");
        CHECK(segmentCrossesDateLine(179.5, -179.5) && !segmentCrossesDateLine(10, 20) && !segmentCrossesDateLine(-170, 170 - 340), "date line segments");
    }
    {   // age and fade
        CHECK(posFade(10) == 1.f && posFade(300) == 1.f && posFade(2400) == 0.25f && posFade(99999) == 0.25f && posFade(-1) == 0.25f, "fade ends");
        CHECK(posFade(1000) < 1.f && posFade(1000) > 0.25f && posFade(600) > posFade(1200), "fade falls");
        AeroAircraft a = plane(1, "A", "F", true, 10, 10, 1000);
        CHECK(posAgeSec(a, 1600) == 600 && posAgeSec(a, 900) == 0, "age (a clock behind is 0)");
        a.hasPos = false;
        CHECK(posAgeSec(a, 1600) == -1, "no position, no age");
        CHECK(ageText(30) == "30 s" && ageText(600) == "10 min" && ageText(7200) == "2.0 h" && ageText(-1) == "-", "age text");
    }
    {   // merging: windows see the same aircraft; ones with no ICAO address stay apart
        AeroAircraft x = plane(0xABCDEF, "G-ABCD", "", false, 0, 0, 0, 100); x.messages = 2;
        AeroAircraft y = plane(0xABCDEF, "", "BAW123", true, 51.5, -0.1, 90, 80);
        mergeAircraft(x, y);
        CHECK(x.hasPos && x.lat == 51.5 && x.flight == "BAW123" && x.registration == "G-ABCD" && x.messages == 5 && x.lastWall == 100, "merge fills position and names");
        AeroAircraft n1 = plane(0, "N1", "", true, 10, 10, 50), n2 = plane(0, "N2", "", true, 20, 20, 60), n3 = plane(0, "N1", "", true, 11, 11, 70);
        CHECK(planeKey(n1) != planeKey(n2) && planeKey(n1) == planeKey(n3), "key by registration without ICAO");
        AeroAircraft z = plane(5, "R", "FLT", true, 1, 1, 200), w = plane(5, "R", "FLT", true, 2, 2, 100, 300);
        mergeAircraft(z, w);
        CHECK(z.lat == 1 && z.lastWall == 300, "the newer position wins even when the other was heard last");
    }
    {   // hundreds of aircraft: sorting, search
        std::vector<AeroAircraft> v;
        const int64_t now = 100000;
        for (int i = 0; i < 600; i++) {
            AeroAircraft a = plane((uint32_t)(0x400000 + i), "", "", i % 3 != 0, (i % 170) - 85, (i % 358) - 179, now - i * 7);
            char f[16], r[16]; std::snprintf(f, sizeof f, "FLT%03d", i); std::snprintf(r, sizeof r, "G-%c%c%c%c", 'A' + i % 26, 'A' + (i / 26) % 26, 'A', 'A');
            a.flight = f; a.registration = r;
            a.altFt = i * 50; a.hasSpeed = i % 2 == 0; a.speedKt = 300 + i % 200;
            v.push_back(a);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto byAge = sortedPlanes(v, PlaneSort::Age, false, "", now);
        auto byAlt = sortedPlanes(v, PlaneSort::Altitude, true, "", now);
        auto bySpd = sortedPlanes(v, PlaneSort::Speed, true, "", now);
        auto byName = sortedPlanes(v, PlaneSort::Name, false, "", now);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 200, "four sorts of 600 aircraft took %.1f ms", ms);
        CHECK(byAge.size() == 600 && byAlt.size() == 600 && byName.size() == 600, "all rows kept");
        CHECK(placedCount(v) == 400, "placed %d", placedCount(v));
        bool ok = true;
        double prev = -1;
        for (size_t k = 0; k < byAge.size() && v[byAge[k]].hasPos; k++) { const double ag = posAgeSec(v[byAge[k]], now); if (ag < prev) ok = false; prev = ag; }
        CHECK(ok, "by age, newest first");
        CHECK(v[byAge.back()].hasPos == false, "the ones with no position go last");
        ok = true; int prevAlt = 1 << 30;
        for (size_t k = 0; k < byAlt.size() && v[byAlt[k]].hasPos && v[byAlt[k]].hasAlt; k++) { if (v[byAlt[k]].altFt > prevAlt) ok = false; prevAlt = v[byAlt[k]].altFt; }
        CHECK(ok, "by altitude, highest first");
        CHECK(v[bySpd.front()].hasSpeed, "speed sort starts with a speed");
        CHECK(planeName(v[byName.front()]) == "FLT000" && planeName(v[byName.back()]) == "FLT599", "by name");
        CHECK(sortedPlanes(v, PlaneSort::Name, false, "flt59", now).size() == 10, "search by flight");
        CHECK(sortedPlanes(v, PlaneSort::Name, false, "400000", now).size() == 1, "search by ICAO");
        CHECK(sortedPlanes(v, PlaneSort::Name, false, "nothing like this", now).empty(), "search finds nothing");
        // merging 600 into a map by key
        std::vector<AeroAircraft> merged;
        for (int pass = 0; pass < 2; pass++) for (const auto& a : v) {
            bool f = false;
            for (auto& m : merged) if (planeKey(m) == planeKey(a)) { mergeAircraft(m, a); f = true; break; }
            if (!f) merged.push_back(a);
        }
        CHECK(merged.size() == 600 && merged[5].messages == 6, "merge twice: no duplicates, counts add");
    }
    {   // history of one aircraft
        std::vector<AeroMessage> all;
        for (int i = 0; i < 100; i++) { AeroMessage m; m.aesId = i % 2 ? 7 : 8; m.label = "H1"; m.wallTime = i; all.push_back(m); }
        AeroAircraft a = plane(7, "R", "F", false, 0, 0, 0);
        const auto h = planeHistory(all, a, 10);
        CHECK(h.size() == 10 && h.back()->wallTime == 99 && h.front()->wallTime == 81, "newest 10 of the aircraft's 50, in time order");
        AeroAircraft b = plane(0, "REG1", "", false, 0, 0, 0);
        AeroMessage m; m.registration = "REG1"; all.push_back(m);
        CHECK(planeHistory(all, b, 10).size() == 1, "by registration when there is no ICAO address");
    }
    return testSummary("inmsat planes");
}
