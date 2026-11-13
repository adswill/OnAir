// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite pass prediction: two-line elements, the SGP4 propagator (near-earth part, Vallado et al. 2006 "Revisiting
// Spacetrack Report #3", WGS-72), look angles from an observer, pass search (AOS, culmination, LOS) and Doppler.
// Only low earth orbits (period under 225 minutes) are handled: every satellite the pass planner lists is one.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct Tle {
    std::string name;
    int norad = 0;
    double epochJd = 0;          // UTC Julian date of the epoch
    double bstar = 0;            // 1 / earth radii
    double inclDeg = 0, raanDeg = 0, ecc = 0, argpDeg = 0, meanAnomDeg = 0;
    double meanMotion = 0;       // revolutions per day
};

// One satellite from three lines (name, line 1, line 2) or two lines (no name). False when the lines are not a TLE
// (wrong line numbers, bad checksum, numbers that do not parse).
bool parseTle(const std::string& l1, const std::string& l2, Tle& out, const std::string& name = "");
// Every TLE in a text (a Celestrak file, or what a user pasted): name lines optional, blank lines and CR ignored.
std::vector<Tle> parseTleText(const std::string& text);

// The propagator for one satellite: position and velocity in TEME, km and km/s, minutes after the epoch
class Sgp4 {
public:
    bool init(const Tle& t);     // false for a deep-space orbit or elements SGP4 cannot use
    bool propagate(double minutes, double r[3], double v[3]) const;
    const Tle& tle() const { return tle_; }
    bool ok() const { return ok_; }
private:
    Tle tle_;
    bool ok_ = false, isimp_ = false;
    double ecco_ = 0, inclo_ = 0, nodeo_ = 0, argpo_ = 0, mo_ = 0, no_ = 0, bstar_ = 0;
    double aycof_ = 0, con41_ = 0, cc1_ = 0, cc4_ = 0, cc5_ = 0, d2_ = 0, d3_ = 0, d4_ = 0, delmo_ = 0, eta_ = 0, argpdot_ = 0, omgcof_ = 0,
           sinmao_ = 0, t2cof_ = 0, t3cof_ = 0, t4cof_ = 0, t5cof_ = 0, x1mth2_ = 0, x7thm1_ = 0, mdot_ = 0, nodedot_ = 0, xlcof_ = 0,
           xmcof_ = 0, nodecf_ = 0;
};

struct Observer { double latDeg = 0, lonDeg = 0, altKm = 0; };

struct Look {
    double azDeg = 0, elDeg = 0, rangeKm = 0;
    double rangeRateKmS = 0;     // positive: moving away (the received frequency is lower)
};

double unixToJd(double unixSec);
double jdToUnix(double jd);
// Greenwich mean sidereal time (radians) at a UT1 ~ UTC Julian date
double gmst(double jd);
// Look angles of the satellite at a UNIX time; false when the propagator fails (decayed)
bool lookAt(const Sgp4& s, const Observer& o, double unixSec, Look& out);
// Geostationary satellite on the equator at satLonEastDeg (east positive, e.g. 19.2 for Astra 19.2°E).
bool geoLookAngles(const Observer& o, double satLonEastDeg, Look& out);
// The received frequency offset (Hz) for a downlink on freqHz: -f * range rate / c
double dopplerHz(double freqHz, double rangeRateKmS);
// The point under the satellite (geodetic, WGS-84) and its height
bool subPoint(const Sgp4& s, double unixSec, double& latDeg, double& lonDeg, double& altKm);

// Whether the TLE files should be fetched now (all times UNIX seconds, 0 = never): the last good fetch is a day old or more (or in the
// future: the clock was set back), and the last attempt, good or not, is at least retrySec ago, so a failing network is tried a few
// times a day and not on every frame. At most one fetch every minInterval after a success.
bool tleUpdateDue(double nowSec, double lastOkSec, double lastTrySec, double minIntervalSec = 86400, double retrySec = 6 * 3600);

struct Pass {
    double aos = 0, los = 0, tca = 0;   // UNIX seconds: rise, set and highest point
    double maxElDeg = 0, aosAzDeg = 0, losAzDeg = 0;
};
// The passes that rise above minElDeg between t0 and t1 (UNIX seconds); a pass under way at t0 starts at t0.
// The edges are found to about a second.
std::vector<Pass> findPasses(const Sgp4& s, const Observer& o, double t0, double t1, double minElDeg = 0);

}
