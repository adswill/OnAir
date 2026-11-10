// Drone airspace alerts (rid_alerts.h): drone fixes with position noise against an airport table and ADS-B traffic. Simulated fixes.
#include "dect2/rid/rid_alerts.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static int count(const std::vector<DroneAlert>& v, const char* k) { int n = 0; for (auto& a : v) n += a.kind == k; return n; }

int main() {
    const double aLat = 54.634, aLon = 25.286;   // an airport
    auto airport = [&](double lat, double lon, std::string& code, double& km) { km = DroneAirspaceWatch::distM(lat, lon, aLat, aLon) / 1000; code = "EYVI"; return km < 30; };
    for (double noiseM : {2.0, 20.0}) {
        std::mt19937 r(5); std::normal_distribution<double> g(0, 1);
        DroneAirspaceWatch w; w.nearestAirport = airport;
        std::vector<DroneAlert> out;
        // drone A: 12 km from the airport, climbs from 0 to 160 m over 160 s, one fix a second
        for (int t = 0; t < 300; t++) {
            DroneFix d; d.id = "1581F5FKD229400B"; d.t = t; d.lat = 54.74 + noiseM * g(r) / 111320; d.lon = 25.28; d.heightM = std::min(160, t) + 2 * g(r); d.altM = 150 + d.heightM;
            w.drone(d, out);
        }
        CHECK(count(out, "high") == 1 && count(out, "airport") == 0 && count(out, "aircraft") == 0, "noise %.0f: one high alert (%d %d %d)", noiseM, count(out, "high"), count(out, "airport"), count(out, "aircraft"));
        if (!out.empty()) CHECK(out[0].value > 125 && out[0].t >= 115, "high at %.0f m, t %.0f", out[0].value, out[0].t);
        // after the cooldown a drone still too high is reported again
        out.clear();
        for (int t = 300; t < 1000; t++) { DroneFix d; d.id = "1581F5FKD229400B"; d.t = t; d.lat = 54.74; d.lon = 25.28; d.heightM = 150; w.drone(d, out); }
        CHECK(count(out, "high") == 1, "again after 10 min %d", count(out, "high"));
        // drone B: 3 km from the airport at 60 m
        out.clear();
        for (int t = 0; t < 30; t++) { DroneFix d; d.id = "FIN87astrdge12k8"; d.t = 2000 + t; d.lat = aLat + 3000.0 / 111320 + noiseM * g(r) / 111320; d.lon = aLon; d.heightM = 60; d.altM = 200; w.drone(d, out); }
        CHECK(count(out, "airport") == 1 && out[0].other == "EYVI" && std::fabs(out[0].value - 3) < 0.2, "airport %d %.2f", count(out, "airport"), out.empty() ? 0 : out[0].value);
        // an aircraft passes 600 m from drone B at 350 m (150 m above it), then one 1.9 km away, then one far above
        out.clear();
        for (int t = 0; t < 60; t++) {
            MannedFix m; m.icao = 0x4B1234; m.callsign = "BTI4LK"; m.t = 2100 + t; m.lat = aLat + 3000.0 / 111320; m.lon = aLon - 0.05 + t * 0.0017; m.altM = 350;
            w.aircraft(m);
            MannedFix far = m; far.icao = 0x111111; far.callsign = ""; far.lat += 2500.0 / 111320; w.aircraft(far);
            MannedFix hi = m; hi.icao = 0x222222; hi.altM = 1500; w.aircraft(hi);
            DroneFix d; d.id = "FIN87astrdge12k8"; d.t = 2100 + t; d.lat = aLat + 3000.0 / 111320 + 600.0 / 111320 + noiseM * g(r) / 111320; d.lon = aLon; d.heightM = 60; d.altM = 200;
            w.drone(d, out);
        }
        int ac = 0; for (auto& a : out) if (a.kind == "aircraft") { ac++; CHECK(a.other == "BTI4LK" && a.value < 1000, "aircraft %s %.0f", a.other.c_str(), a.value); }
        CHECK(ac == 1, "one aircraft alert %d", ac);
    }
    {   // a stale aircraft position (30 s old) or an unknown drone altitude gives no proximity alert
        DroneAirspaceWatch w; std::vector<DroneAlert> out;
        MannedFix m; m.icao = 1; m.t = 0; m.lat = 54; m.lon = 25; m.altM = 100; w.aircraft(m);
        DroneFix d; d.id = "X"; d.t = 30; d.lat = 54; d.lon = 25; d.altM = 100; w.drone(d, out);
        DroneFix d2 = d; d2.t = 5; d2.altM = -1000; w.drone(d2, out);
        CHECK(out.empty(), "nothing %zu", out.size());
        d.t = 5; w.drone(d, out);
        CHECK(count(out, "aircraft") == 1 && out[0].other == "000001", "fresh one %zu", out.size());
    }
    return testSummary("rid_alerts");
}
