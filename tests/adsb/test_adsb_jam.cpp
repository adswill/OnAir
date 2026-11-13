// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPS interference grid: synthetic fleets with degraded NACp near a jammer, aggregation, windows, jumps, privacy of the wire body.
#include "dect2/adsb/adsb_jam.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static GpsJamObs ob(uint32_t id, double lat, double lon, double t, int nacp, int nic = -1) {
    GpsJamObs o; o.icao = id; o.lat = lat; o.lon = lon; o.timeSec = t; o.nacp = nacp; o.nic = nic; return o;
}

int main() {
    CHECK(!GpsJamGrid::degradedAccuracy(9, 10) && !GpsJamGrid::degradedAccuracy(7, 6), "good accuracy");
    CHECK(GpsJamGrid::degradedAccuracy(0, -1) && GpsJamGrid::degradedAccuracy(6, -1) && GpsJamGrid::degradedAccuracy(10, 5), "bad accuracy");
    CHECK(!GpsJamGrid::degradedAccuracy(-1, -1), "unknown accuracy is not bad");

    // a fleet of 400 aircraft over 10 x 10 degrees; a jammer at 30N 40E degrades NACp within 150 km, partly out to 300 km
    GpsJamGrid g;
    uint32_t id = 1;
    const double jl = 30, jo = 40;
    for (double la = 25; la < 35; la += 0.25) for (double lo = 35; lo < 45; lo += 0.25) {
        const double dkm = std::hypot((la - jl) * 111.0, (lo - jo) * 96.0);
        const int nacp = dkm < 150 ? 0 : (dkm < 300 && ((int)(la * 4 + lo * 4) & 1)) ? 5 : 10;
        g.observe(ob(id++, la, lo, 100, nacp));
    }
    auto cells = g.cells(200);
    CHECK(!cells.empty(), "cells");
    int hot = 0, quiet = 0; bool centreOk = false, farOk = true;
    for (const auto& c : cells) {
        const double dkm = std::hypot((c.south() + 0.25 - jl) * 111.0, (c.west() + 0.25 - jo) * 96.0);
        if (c.level() == 3) hot++;
        if (c.level() == 0) quiet++;
        if (dkm < 40 && c.level() == 3) centreOk = true;
        if (dkm > 400 && c.level() != 0) farOk = false;
    }
    CHECK(centreOk, "the cell on the jammer is level 3");
    CHECK(farOk, "cells far from the jammer stay normal");
    CHECK(hot >= 4 && quiet >= 20, "hot %d quiet %d", hot, quiet);

    // one aircraft counts once per cell; thin cells are left out
    GpsJamGrid h;
    for (int i = 0; i < 10; i++) h.observe(ob(7, 10.1, 20.1, 10 + i, 10));
    h.observe(ob(8, 10.2, 20.2, 12, 0));
    CHECK(h.cells(30).empty(), "2 aircraft: below the minimum");
    h.observe(ob(9, 10.3, 20.3, 13, 9));
    auto hc = h.cells(30);
    CHECK(hc.size() == 1 && hc[0].aircraft == 3 && hc[0].degraded == 1 && hc[0].latIdx == 20 && hc[0].lonIdx == 40, "one cell: %d aircraft %d bad", hc.empty() ? 0 : hc[0].aircraft, hc.empty() ? 0 : hc[0].degraded);
    CHECK(hc[0].level() == 2, "1 of 3 is level 2, got %d", hc[0].level());
    CHECK(h.cells(30 + 3601).empty() && h.trackedAircraft() == 0, "the window expires");

    // position jump: 500 km in 10 s, with a good NACp, counts as degraded
    GpsJamGrid j;
    for (uint32_t k = 0; k < 3; k++) { j.observe(ob(100 + k, 50.1, 8.1, 0, 10)); j.observe(ob(100 + k, 54.6, 8.1, 10, 10)); j.observe(ob(100 + k, 54.6, 8.1, 20, 10)); }
    auto jc = j.cells(30);
    CHECK(jc.size() == 2, "two cells");
    int jumps = 0; for (auto& c : jc) jumps += c.jumps;
    CHECK(jumps == 3, "3 jumps, got %d", jumps);
    CHECK(!GpsJamGrid::impliesJump(50, 8, 0, 50.05, 8, 60) && GpsJamGrid::impliesJump(50, 8, 0, 55, 8, 10) && !GpsJamGrid::impliesJump(50, 8, 0, 60, 8, 1000), "jump rule");

    // ground targets and unknown accuracy do not count
    GpsJamGrid k2;
    for (uint32_t n = 0; n < 5; n++) { GpsJamObs o = ob(n, 1, 1, 0, 0); o.ground = true; k2.observe(o); k2.observe(ob(50 + n, 1, 1, 0, -1)); }
    CHECK(k2.cells(1).empty(), "ground and unknown skipped");

    // negative and western cells, the wire body
    GpsJamGrid w;
    for (uint32_t n = 0; n < 3; n++) w.observe(ob(900 + n, -33.9, -70.9, 0, n == 0 ? 2 : 9));
    auto wc = w.cells(10);
    CHECK(wc.size() == 1 && wc[0].latIdx == -68 && wc[0].lonIdx == -142, "negative cell %d %d", wc.empty() ? 0 : wc[0].latIdx, wc.empty() ? 0 : wc[0].lonIdx);
    const std::string body = GpsJamGrid::toJson("station-abcdef12", wc);
    CHECK(body == "{\"station\":\"station-abcdef12\",\"cell\":0.5,\"cells\":[[-68,-142,3,1]]}", "body %s", body.c_str());
    CHECK(body.find("900") == std::string::npos && body.find("icao") == std::string::npos, "no aircraft id in the body");
    CHECK(GpsJamGrid::toJson("s", {}).empty(), "nothing to send");

    printf(fails ? "FAILED\n" : "adsb jam ok\n");
    return fails ? 1 : 0;
}
