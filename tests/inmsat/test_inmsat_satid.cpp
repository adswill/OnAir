// Satellite identification of the Inmarsat all-carriers mode: content, frequency and geometry evidence on synthetic carrier sets of two or three
// satellites, with offsets, with carriers missing, with the wrong location and with clues that disagree. The tables are unverified against a real
// capture: needs a user test with a real capture.
#include "dect2/inmsat/inmsat_satid.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2::inmsat;
#include "../common/test_util.h"

namespace {
SatCarrier car(double mhz, int rate, double offHz = 0) { SatCarrier c; c.freqHz = mhz * 1e6 + offHz; c.bitRate = rate; return c; }
SatSysFact sysLon(double lon) { SatSysFact f; f.kind = 2; f.satId = 12; f.lonDeg = lon; f.pMHz = 1545.0; return f; }
void dump(const char* n, const SatResult& r) {
    printf("%s: %s  (valid %d, conf %.2f%s)\n", n, r.summary.c_str(), r.valid, r.confidence, r.ambiguous ? ", ambiguous" : "");
    if (!r.ranked.empty()) for (const auto& e : r.ranked[0].evidence) printf("    %-9s %s %.2f  %s\n", e.kind.c_str(), e.available ? "on " : "off", e.score, e.text.c_str());
}
const double kLtLat = 54.7, kLtLon = 25.3;      // Lithuania
const double kMiLat = 25.8, kMiLon = -80.2;     // Miami
}

int main() {
    // geometry sanity
    {
        double az, el;
        geoLook(kLtLat, kLtLon, 25.0, az, el);
        CHECK(el > 25 && el < 35 && (az > 175 && az < 185), "Lithuania to 25E: az %.1f el %.1f", az, el);
        geoLook(kLtLat, kLtLon, -98.0, az, el);
        CHECK(el < 0, "Lithuania to 98W: el %.1f must be below the horizon", el);
        geoLook(kMiLat, kMiLon, -98.0, az, el);
        CHECK(el > 40, "Miami to 98W: el %.1f", el);
        geoLook(0, 25.0, 25.0, az, el);
        CHECK(std::fabs(el - 90) < 0.5, "sub-satellite point el %.1f", el);
    }
    // 1. content alone: the system table says 25E
    {
        SatInput in; in.sys.push_back(sysLon(25.5));
        const SatResult r = identifySatellite(in);
        dump("content only", r);
        CHECK(r.valid && r.name == "Alphasat 25E" && r.confidence <= 0.6 && r.confidence >= 0.3, "name '%s' conf %.2f", r.name.c_str(), r.confidence);
    }
    // 2. three sources agree: Alphasat's channels (with a 2 kHz offset and one carrier missing), 25E in the table, Lithuania
    {
        SatInput in; in.sys.push_back(sysLon(25.5)); in.haveLocation = true; in.latDeg = kLtLat; in.lonDeg = kLtLon;
        in.carriers = {car(1545.115, 600, 2000), car(1545.315, 10500, -2000)};      // 1545.130 and 1545.3275 not heard
        const SatResult r = identifySatellite(in);
        dump("three agree", r);
        CHECK(r.valid && r.name == "Alphasat 25E" && r.confidence >= 0.85 && !r.ambiguous, "name '%s' conf %.2f", r.name.c_str(), r.confidence);
        CHECK(r.ranked[0].evidence.size() == 3 && r.ranked[0].evidence[0].available && r.ranked[0].evidence[1].available && r.ranked[0].evidence[2].available, "all three evidences");
        CHECK(satNameForCarrier(r, 1545.115e6, 600) == "Alphasat 25E", "carrier name");
    }
    // 3. frequency and geometry only (no system table yet): 4F3's channels from Miami identify 98W; the same carriers seen from Lithuania do not
    {
        SatInput in; in.haveLocation = true; in.latDeg = kMiLat; in.lonDeg = kMiLon;
        in.carriers = {car(1545.020, 600), car(1545.205, 1200), car(1546.005, 10500), car(1546.0775, 10500)};
        const SatResult r = identifySatellite(in);
        dump("4F3 from Miami", r);
        CHECK(r.valid && r.name == "Inmarsat-4 F3 98W" && r.confidence >= 0.6, "name '%s' conf %.2f", r.name.c_str(), r.confidence);
        in.latDeg = kLtLat; in.lonDeg = kLtLon;
        const SatResult w = identifySatellite(in);
        dump("4F3 channels, wrong location", w);
        CHECK(!w.valid || w.name != "Inmarsat-4 F3 98W" || w.confidence < 0.3, "below the horizon is not accepted: '%s' %.2f", w.name.c_str(), w.confidence);
        bool noted = false;
        for (const auto& c : w.ranked) for (const auto& e : c.evidence) noted |= e.kind == "geometry" && std::strstr(e.text.c_str(), "below the horizon");
        CHECK(noted, "the evidence says below the horizon");
    }
    // 4. two satellites in one capture (Alphasat and 6F1 channels together): ambiguous, never a confident single answer from frequency alone
    {
        SatInput in; in.haveLocation = true; in.latDeg = kLtLat; in.lonDeg = kLtLon;
        in.carriers = {car(1545.120, 600), car(1545.200, 600), car(1545.315, 10500), car(1546.100, 10500)};
        const SatResult r = identifySatellite(in);
        dump("two satellites", r);
        CHECK(r.confidence < 0.8, "two satellites: conf %.2f", r.confidence);
    }
    // 5. clues that disagree: the table says 83.5E, the channels are Alphasat's
    {
        SatInput in; in.sys.push_back(sysLon(83.0)); in.haveLocation = true; in.latDeg = kLtLat; in.lonDeg = kLtLon;
        in.carriers = {car(1545.115, 600), car(1545.130, 600), car(1545.315, 10500)};
        const SatResult r = identifySatellite(in);
        dump("disagree", r);
        CHECK(r.confidence < 0.85, "disagreement must lower the confidence: %.2f", r.confidence);
    }
    // 6. Inmarsat-C ocean region IOR
    {
        SatInput in; in.inmcRegions.push_back(3);
        const SatResult r = identifySatellite(in);
        dump("IOR", r);
        CHECK(r.valid && std::fabs(r.lonDeg - 64.5) < 0.1, "IOR -> %s", r.name.c_str());
    }
    // 7. nothing known: no answer, and no name on carriers
    {
        const SatResult r = identifySatellite(SatInput());
        CHECK(!r.valid && satNameForCarrier(r, 1545.1e6, 600).empty(), "nothing known");
    }
    // 8. a longitude that is not in the table is still shown
    {
        SatInput in; in.sys.push_back(sysLon(-30.0));
        const SatResult r = identifySatellite(in);
        dump("unknown slot", r);
        CHECK(r.summary.find("30.0W") != std::string::npos || r.valid, "unknown slot shown: %s", r.summary.c_str());
    }
    // 9. manual override
    {
        SatInput in; in.sys.push_back(sysLon(25.5)); in.manual = "Inmarsat-6 F1 83.5E";
        SatSysFact b; b.kind = 3; b.spotBeam = true; in.sys.push_back(b);
        const SatResult r = identifySatellite(in);
        CHECK(r.valid && r.manual && r.name == "Inmarsat-6 F1 83.5E" && r.confidence == 1.0 && r.beam == "spot beam", "manual: %s beam '%s'", r.name.c_str(), r.beam.c_str());
        CHECK(satNameForCarrier(r, 1545.0e6, 600) == "Inmarsat-6 F1 83.5E", "manual carrier name");
    }
    // 10. frequency not known (capture centre unknown): the frequency evidence is off, the rest still works
    {
        SatInput in; in.sys.push_back(sysLon(25.0)); in.carriers.push_back(car(0, 600));
        const SatResult r = identifySatellite(in);
        CHECK(r.valid && !r.ranked[0].evidence[1].available, "frequency evidence off without frequencies");
    }
    return testSummary("inmsat_satid");
}
