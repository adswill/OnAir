// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ground station positions: a corrupted report is ignored, the first report is unconfirmed, impossible coordinates are rejected, names come
// from the airport table.
#include "dect2/vdl2/vdl2_gsname.h"
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const double la = 48.8566, lo = 2.3522;   // Paris
    {
        Vdl2GroundStation g;
        CHECK(vdl2StationReport(g, la, lo) == Vdl2PosResult::Unconfirmed, "first report unconfirmed");
        CHECK(!g.hasPos && g.hasPending, "no position yet");
        CHECK(vdl2StationReport(g, la + 0.0005, lo - 0.0005) == Vdl2PosResult::Confirmed, "second agreeing report");
        CHECK(g.hasPos && vdl2DistNm(g.lat, g.lon, la, lo) < 0.2, "consensus at Paris");
        CHECK(vdl2StationReport(g, la, lo - 40.0) == Vdl2PosResult::Outlier, "one wrong digit: Paris in the Atlantic");
        CHECK(vdl2DistNm(g.lat, g.lon, la, lo) < 0.2, "position did not move: %.2f nm", vdl2DistNm(g.lat, g.lon, la, lo));
        CHECK(g.posOutliers == 1, "outlier counted");
        for (int i = 0; i < 5; i++) vdl2StationReport(g, la, lo);
        CHECK(vdl2DistNm(g.lat, g.lon, la, lo) < 0.2, "stays put");
    }
    {   // a wrong first report does not win once two good ones agree
        Vdl2GroundStation g;
        vdl2StationReport(g, la, lo - 40.0); vdl2StationReport(g, la, lo); vdl2StationReport(g, la, lo);
        CHECK(g.hasPos && vdl2DistNm(g.lat, g.lon, la, lo) < 0.2, "good cluster wins");
    }
    {
        Vdl2GroundStation g;
        CHECK(vdl2StationReport(g, 91, 0) == Vdl2PosResult::Rejected, "lat > 90");
        CHECK(vdl2StationReport(g, 10, 181) == Vdl2PosResult::Rejected, "lon > 180");
        CHECK(vdl2StationReport(g, 0, 0) == Vdl2PosResult::Rejected, "0,0");
        CHECK(!g.hasPos && !g.hasPending && g.posRejected == 3, "nothing kept");
    }
    {   // names: reported airport, else the nearest one
        const AirportDb& db = AirportDb::builtin();
        Vdl2GroundStation g; g.airports = "EDDF";
        const std::string n1 = vdl2StationName(g, db);
        CHECK(n1.rfind("EDDF", 0) == 0, "reported airport: '%s'", n1.c_str());
        Vdl2GroundStation h; h.hasPos = true; h.lat = 50.03; h.lon = 8.56;
        const std::string n2 = vdl2StationName(h, db);
        CHECK(n2.rfind("near ", 0) == 0 && n2.find("EDDF") != std::string::npos, "nearest: '%s'", n2.c_str());
        Vdl2GroundStation m; m.hasPos = true; m.lat = -60; m.lon = -120;   // open ocean
        CHECK(vdl2StationName(m, db).empty(), "nothing near");
    }
    return testSummary("vdl2_gsstable");
}
