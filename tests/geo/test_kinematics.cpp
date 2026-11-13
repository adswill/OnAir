// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Derived kinematics (core/include/dect2/geo/kinematics.h): geodesic distance, ground speed, track, vertical rate, the uncertainty from
// rounded positions, the antimeridian, the rejection rules, the choice of the time stamp, the 3D figure and which source wins.
#include "dect2/geo/kinematics.h"
#include <cmath>
#include <cstdio>
using namespace dect2::geo;

#include "../common/test_util.h"

static KinFix fx(double lat, double lon, double t, double alt = -1, double rep = -1, double res = -1) {
    KinFix f; f.lat = lat; f.lon = lon; f.t = t; f.repSecOfDay = rep; f.posResM = res;
    if (alt >= 0) { f.hasAlt = true; f.altFt = alt; }
    return f;
}

int main() {
    // Vincenty's own test line: Flinders Peak to Buninyong, 54972.271 m, azimuth 306 deg 52' 05.37"
    {
        const double la1 = -(37 + 57 / 60.0 + 3.72030 / 3600), lo1 = 144 + 25 / 60.0 + 29.52440 / 3600;
        const double la2 = -(37 + 39 / 60.0 + 10.15610 / 3600), lo2 = 143 + 55 / 60.0 + 35.38390 / 3600;
        const Geodesic g = geodesicInverse(la1, lo1, la2, lo2);
        CHECK(std::fabs(g.distM - 54972.271) < 0.01, "Vincenty line %.3f m", g.distM);
        CHECK(std::fabs(g.az1Deg - (306 + 52 / 60.0 + 5.37 / 3600)) < 1e-4, "azimuth %.5f", g.az1Deg);
    }
    // city pair: JFK - LHR about 5540 km; one degree of latitude on the meridian 110.6 km at the equator, 111.7 km at the pole
    {
        const Geodesic g = geodesicInverse(40.6413, -73.7781, 51.4700, -0.4543);
        CHECK(std::fabs(g.distM / 1000 - 5540) < 15, "JFK-LHR %.1f km", g.distM / 1000);
        CHECK(std::fabs(geodesicInverse(0, 10, 1, 10).distM - 110574) < 100, "1 deg at the equator");
        CHECK(std::fabs(geodesicInverse(89, 10, 90, 10).distM - 111694) < 100, "1 deg at the pole");
        CHECK(geodesicInverse(10, 20, 10, 20).distM == 0, "same point");
        const Geodesic n = geodesicInverse(0, 0, 0.5, 179.7);   // nearly antipodal: falls back, still finite
        CHECK(std::isfinite(n.distM) && n.distM > 1.9e7, "near antipode %.0f", n.distM);
    }
    // a level cruise: 450 kt due east along the equator for 10 minutes, exact positions, report times given
    {
        const double dM = 450 * dect2::geo::kKinKt * 600;
        const double dLon = dM / 6378137.0 / kKinDeg;   // on the equator, an east leg is a straight equatorial arc
        const DerivedKin d = deriveKin(fx(0, 10, 1000, 35000, 36000, 1), fx(0, 10 + dLon, 1600, 35000, 36600, 1));
        CHECK(d.ok, "ok");
        CHECK(d.usedReportTime, "report time used");
        CHECK(std::fabs(d.gsKt - 450) < 0.5, "gs %.2f", d.gsKt);
        CHECK(std::fabs(d.trackDeg - 90) < 0.2, "track %.2f", d.trackDeg);
        CHECK(d.hasTrack && d.trackOk && !d.averaged, "track flags");
        CHECK(d.hasVs && std::fabs(d.vsFpm) < 1e-9, "level");
        CHECK(std::fabs(d.slantKt - d.gsKt) < 1e-6, "no climb: slant equals ground speed");
        CHECK(d.gsSigmaKt > 0 && d.gsSigmaKt < 10, "sigma %.2f", d.gsSigmaKt);
    }
    // a northbound climb: vertical rate and the 3D figure; ground speed does not include the climb
    {
        const double dLat = 100000.0 / 111000.0;   // about 100 km
        const DerivedKin d = deriveKin(fx(10, 20, 0, 10000, -1, 1), fx(10 + dLat, 20, 600, 16000, -1, 1));
        CHECK(d.ok, "climb ok");
        CHECK(std::fabs(d.vsFpm - 600) < 1e-6, "vs %.1f", d.vsFpm);
        CHECK(std::fabs(d.trackDeg) < 0.1 || std::fabs(d.trackDeg - 360) < 0.1, "north %.3f", d.trackDeg);
        CHECK(d.slantKt > d.gsKt && d.dist3dM > d.distM, "slant larger");
        CHECK(std::fabs(d.dist3dM - std::hypot(d.distM, 6000 * 0.3048)) < 1e-6, "3D distance");
        CHECK(!d.usedReportTime, "reception time");
        CHECK(d.vsSigmaFpm > 0, "vs sigma");
        CHECK(deriveKin(fx(10, 20, 0, 10000, -1, 1), fx(10 + dLat, 20, 600, -1, -1, 1)).hasVs == false, "no altitude, no vs");
    }
    // rounding: positions at 0.1 minute and at 1 minute widen the speed error; the resolution is found from the coordinates
    {
        CHECK(std::fabs(inferPosResM(25.0 + 17.0 / 60, 55.0 + 21.5 / 60) - 185.2) < 1e-9, "0.1 minute");
        CHECK(std::fabs(inferPosResM(25.0 + 17.0 / 60, 55.0 + 21.0 / 60) - 1852) < 1e-9, "1 minute");
        CHECK(inferPosResM(25.123456, 55.654321) == 30.0, "exact");
        const DerivedKin a = deriveKin(fx(30, 40, 0, -1, -1, 30), fx(30, 41.5, 900, -1, -1, 30));
        const DerivedKin b = deriveKin(fx(30, 40, 0, -1, -1, 185.2), fx(30, 41.5, 900, -1, -1, 185.2));
        const DerivedKin c = deriveKin(fx(30, 40, 0, -1, -1, 1852), fx(30, 41.5, 900, -1, -1, 1852));
        CHECK(a.ok && b.ok && c.ok, "all ok");
        CHECK(a.gsSigmaKt < b.gsSigmaKt && b.gsSigmaKt < c.gsSigmaKt, "sigma grows with the rounding %.2f %.2f %.2f", a.gsSigmaKt, b.gsSigmaKt, c.gsSigmaKt);
        const double expect = std::sqrt(2.0) * 0.5 * 185.2 / 900 / kKinKt;   // the position part alone
        CHECK(b.gsSigmaKt >= expect - 1e-9, "at least the position term %.3f vs %.3f", b.gsSigmaKt, expect);
    }
    // the date line: 179.9 E to 179.9 W is 0.2 degrees, not 359.8
    {
        const DerivedKin d = deriveKin(fx(0, 179.9, 0, -1, -1, 1), fx(0, -179.9, 600, -1, -1, 1));
        CHECK(d.ok && d.distM < 25000, "antimeridian %.0f m", d.distM);
        CHECK(std::fabs(d.trackDeg - 90) < 0.5, "east across the line");
        const DerivedKin w = deriveKin(fx(0, -179.9, 0, -1, -1, 1), fx(0, 179.9, 600, -1, -1, 1));
        CHECK(w.ok && std::fabs(w.trackDeg - 270) < 0.5, "west across the line");
    }
    // the pole: crossing it turns the bearing, but the speed is right
    {
        const DerivedKin d = deriveKin(fx(89.5, 0, 0, -1, -1, 1), fx(89.5, 180, 1200, -1, -1, 1));   // 111 km over the pole in 20 min
        CHECK(d.ok, "pole ok");
        CHECK(std::fabs(d.gsKt - (geodesicInverse(89.5, 0, 89.5, 180).distM / 1200 / kKinKt)) < 1e-6, "pole speed");
    }
    // rejection rules
    {
        CHECK(deriveKin(fx(0, 0, 0), fx(0, 1, 30)).reason == KinReason::TooShort, "30 s too short");
        CHECK(deriveKin(fx(0, 0, 0), fx(0, 1, 6000)).reason == KinReason::TooLong, "100 min too long");
        CHECK(deriveKin(fx(0, 0, 0, -1, -1, 1852), fx(0, 0.02, 300, -1, -1, 1852)).reason == KinReason::TooClose, "inside the rounding");
        CHECK(deriveKin(fx(0, 0, 0, -1, -1, 1), fx(0, 20, 600, -1, -1, 1)).reason == KinReason::TooFast, "2200 km in 10 min");
        CHECK(deriveKin(fx(0, 0, 100), fx(0, 1, 100)).reason == KinReason::BadInput, "same time");
        CHECK(deriveKin(fx(0, 0, 100), fx(0, 1, 50)).reason == KinReason::BadInput, "backwards");
        CHECK(deriveKin(fx(95, 0, 0), fx(0, 1, 600)).reason == KinReason::BadInput, "bad latitude");
        CHECK(deriveKin(fx(NAN, 0, 0), fx(0, 1, 600)).reason == KinReason::BadInput, "NaN");
        // a slow chord with large rounding is too uncertain
        KinOptions tight; tight.maxSigmaFrac = 0.2;
        const DerivedKin u = deriveKin(fx(0, 0, 0, -1, -1, 1852), fx(0.05, 0, 3600, -1, -1, 1852), tight);
        CHECK(!u.ok && u.reason == KinReason::TooUncertain, "too uncertain: %d", (int)u.reason);
        // below the track speed the bearing is not offered
        const DerivedKin s = deriveKin(fx(0, 0, 0, -1, -1, 1), fx(0.01, 0, 600, -1, -1, 1));
        CHECK(s.ok && !s.hasTrack, "slow: speed but no track");
    }
    // long gap: averaged, track not good enough for an icon
    {
        const DerivedKin d = deriveKin(fx(10, 10, 0, -1, -1, 1), fx(11.5, 12, 1500, -1, -1, 1));
        CHECK(d.ok && d.averaged && d.hasTrack && !d.trackOk, "average over 25 min");
    }
    // time stamp choice: report time wins when it agrees with the reception order; a midnight wrap is handled; a mismatch is refused
    {
        const DerivedKin a = deriveKin(fx(0, 0, 100, -1, 3000, 1), fx(0, 1.0, 100 + 700, -1, 3600, 1));   // report says 600 s, reception 700 s
        CHECK(a.ok && a.usedReportTime && std::fabs(a.dtSec - 600) < 1e-9, "report time %.1f", a.dtSec);
        const DerivedKin w = deriveKin(fx(0, 0, 100, -1, 86100, 1), fx(0, 1.0, 700, -1, 300, 1));       // 23:55 -> 00:05
        CHECK(w.ok && w.usedReportTime && std::fabs(w.dtSec - 600) < 1e-9, "midnight %.1f", w.dtSec);
        const DerivedKin m = deriveKin(fx(0, 0, 100, -1, 3000, 1), fx(0, 1.0, 100 + 5000, -1, 3600, 1));   // report 600 s, reception 5000 s apart
        CHECK(!m.ok && m.reason == KinReason::ClockMismatch, "clock mismatch %d", (int)m.reason);
        const DerivedKin one = deriveKin(fx(0, 0, 100, -1, 3000, 1), fx(0, 1.0, 700, -1, -1, 1));
        CHECK(one.ok && !one.usedReportTime, "only one report time: reception time");
        const DerivedKin r = deriveKin(fx(0, 0, 100, -1, 3000, 1), fx(0, 1.0, 700, -1, 3600, 1));
        CHECK(r.gsSigmaKt < one.gsSigmaKt, "report time gives the smaller error");
    }
    // walking back over a pair that is too close to the newest
    {
        KinFix p[3] = {fx(10, 10, 0, -1, -1, 1), fx(10.2, 10, 600, -1, -1, 1), fx(10.2, 10, 660, -1, -1, 1)};   // the last repeats the position
        const DerivedKin d = deriveFromTrack(3, [&](int i) { return p[i]; });
        CHECK(d.ok && std::fabs(d.dtSec - 660) < 1e-9, "uses the older point (%.0f s)", d.dtSec);
        CHECK(!deriveFromTrack(1, [&](int i) { return p[i]; }).ok, "one point");
    }
    // which source wins: measured over estimated over derived; fields mix; none -> None
    {
        const DerivedKin d = deriveKin(fx(0, 0, 0, -1, -1, 1), fx(0, 1, 600, -1, -1, 1));
        KinMeasured m; KinEstimated e; e.valid = true; e.gsKt = 400; e.trackDeg = 80; e.vsFpm = 0;
        KinValue v = chooseKin(m, e, d);
        CHECK(v.src == KinSource::Estimated && v.gsKt == 400, "estimator before the two-point value");
        v = chooseKin(m, KinEstimated(), d);
        CHECK(v.src == KinSource::Derived && std::fabs(v.gsKt - d.gsKt) < 1e-9 && v.gsSigmaKt > 0, "derived fallback");
        m.hasGs = true; m.gsKt = 123; v = chooseKin(m, e, d);
        CHECK(v.src == KinSource::Measured && v.gsKt == 123, "measured never replaced");
        CHECK(v.hasTrack && v.trackDeg == 80, "missing track filled from the estimate");
        CHECK(chooseKin(KinMeasured(), KinEstimated(), DerivedKin()).src == KinSource::None, "nothing");
    }
    return testSummary("kinematics");
}
