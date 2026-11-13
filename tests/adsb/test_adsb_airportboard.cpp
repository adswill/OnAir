// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B arrivals and departures (round 4 #28): an approach down to the ground, an approach lost low before touchdown, a departure from the
// ground, a turnaround (land, then depart again), an overflight high above, a go-around that climbs away, a missed far approach, runway names.
#include "dect2/adsb/adsb_airport_board.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

static AdsbAircraft at(uint32_t icao, double km, double trk, int alt, int vrate, bool ground, const char* cs = "TEST1") {
    // km along the runway axis (east-west through the airport at 54.6, 25.3)
    AdsbAircraft a;
    a.icao = icao; a.callsign = cs; a.hasPos = true; a.lat = 54.6; a.lon = 25.3 + km / (111.32 * std::cos(54.6 * 3.14159265 / 180));
    a.hasAlt = !ground; a.altFt = alt; a.hasVrate = true; a.vrateFpm = vrate; a.ground = ground; a.hasHeading = true; a.headingIsTrack = true; a.headingDeg = (float)trk; a.ageSec = 0.5f;
    return a;
}

int main() {
    AirportBoardConfig c; c.lat = 54.6; c.lon = 25.3; c.elevFt = 646; c.radiusKm = 8; c.lowFt = 2000;
    AirportBoard b;
    int64_t t = 1'800'000'000;
    int land = 0, dep = 0;
    auto run = [&](std::vector<AdsbAircraft> v) { for (const auto& m : b.update(v, c, t)) (m.landing ? land : dep)++; t++; };
    // 1: an approach from the east, 3 deg, down to the ground, then taxi
    for (double k = 20; k > 0; k -= 0.1) run({at(1, k, 270, 646 + (int)(k * 320), -700, false)});
    for (int s = 0; s < 30; s++) run({at(1, 0, 270, 0, 0, true)});
    CHECK(land == 1 && dep == 0 && b.all().back().runway == "27" && b.all().back().icao == 1, "landing on 27 (%d / %d)", land, dep);
    // the same aircraft departs again 40 minutes later (turnaround)
    t += 2400;
    for (int s = 0; s < 20; s++) run({at(1, -1, 90, 0, 0, true)});
    for (double k = -1; k < 15; k += 0.1) run({at(1, k, 90, 646 + (int)((k + 1) * 400), 1500, false)});
    CHECK(land == 1 && dep == 1 && b.all().back().runway == "09" && !b.all().back().landing, "departure on 09 (%d / %d)", land, dep);
    // 2: an approach lost at 300 ft above the field, 2 km out (no ground report)
    for (double k = 15; k > 2; k -= 0.1) run({at(2, k, 270, 646 + (int)(k * 160), -700, false, "LOW2")});
    for (int s = 0; s < 70; s++) run({});
    CHECK(land == 2, "lost low on the approach counts as landed (%d)", land);
    // 3: an overflight at 30 000 ft
    for (double k = 30; k > -30; k -= 0.5) run({at(3, k, 270, 30000, 0, false, "HIGH3")});
    // 4: a go-around: down to 600 ft, then climbs away
    for (double k = 12; k > 1; k -= 0.1) run({at(4, k, 270, 646 + (int)(k * 300), -700, false, "GA4")});
    for (double k = 1; k > -15; k -= 0.1) run({at(4, k, 270, 646 + 300 + (int)((1 - k) * 300), 1200, false, "GA4")});
    // 5: a low approach to another field 30 km away
    for (double k = 45; k > 30; k -= 0.1) run({at(5, k, 270, 646 + (int)((k - 30) * 300), -700, false, "FAR5")});
    for (int s = 0; s < 70; s++) run({});
    CHECK(land == 2 && dep == 1, "no movement for the overflight, the go-around or the far field (%d / %d)", land, dep);
    // a departure first heard already airborne, low and climbing
    for (double k = 0.5; k < 10; k += 0.1) run({at(6, k, 90, 900 + (int)(k * 300), 1800, false, "UP6")});
    CHECK(dep == 2, "first heard climbing out (%d)", dep);
    CHECK(AirportBoard::runwayOf(3) == "36" && AirportBoard::runwayOf(355) == "36" && AirportBoard::runwayOf(94) == "09" && AirportBoard::runwayOf(-1).empty(), "runway names");
    return testSummary("adsb_airportboard");
}
