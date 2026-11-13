// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Aircraft position estimator shared by ADS-B, UAT, ACARS, VDL2 and HFDL: an interacting-multiple-model (IMM) filter (Bar-Shalom) over
// three horizontal motion models (constant velocity, coordinated turn with a decaying turn rate, manoeuvre = high acceleration noise) and a
// two-model vertical channel (level / climbing or descending), extended Kalman filters in a local ENU frame, Joseph-form covariance updates.
// Input: timestamped fixes from any source with source specific noise. Output: the state at any time `now` (interpolation between fixes,
// dead reckoning when they stop) with its covariance, a 95 % ellipse and a confidence. Pure C++, deterministic, no global state.
// Written from the literature (docs/position-estimation.md); no code of another project is used.
#pragma once
#include "track_geo.h"
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

namespace dect2::track {

enum class Src : uint8_t { Adsb = 0, Uat, Acars, Vdl2, Hfdl, Other };
constexpr int kSrcCount = 6;
const char* srcName(Src s);

// One report. Only the parts with has* set are used. All times are seconds on one clock per aircraft (signal time or wall time).
struct Fix {
    double t = 0;
    Src src = Src::Other;
    bool hasPos = false; double lat = 0, lon = 0;
    int nic = -1, nacp = -1;          // ADS-B / UAT integrity and accuracy categories (-1 unknown)
    double resolutionDeg = 0;         // rounding step of a text report: 1/600 = tenths of a minute, 1/60 = whole minutes, 0.001 ... (0 exact)
    double timeSigmaSec = -1;         // how sure the time of the position is (-1: the default of the source)
    double latencySec = -1;           // how much older the position is than `t` (message received at t): -1 = the default of the source, 0 = `t` is the time of the position
    double posSigmaM = 0;             // > 0: use this 1-sigma instead of everything above
    bool hasAlt = false; double altM = 0, altSigmaM = 0;            // barometric altitude, metres (0 sigma: default of the source)
    bool hasVrate = false; double vrateMs = 0, vrateSigmaMs = 0;
    bool hasVel = false; double speedMs = 0, trackDeg = 0;          // ground speed and track; or true airspeed and heading when velIsAir
    double speedSigmaMs = 0, trackSigmaDeg = 0;
    bool velIsAir = false;
    bool hasRoll = false; double rollDeg = 0;                       // bank angle, right wing down positive (Comm-B 5,0)
    bool hasWind = false; double windFromDeg = 0, windMs = 0;       // a wind report or a derived wind (direction it blows from)
    bool onGround = false;
};

struct TrackOptions {
    // process noise (continuous white-noise acceleration, m^2/s^3)
    double qCv = 0.01, qCt = 0.02, qMan = 0.5, qOmega = 1.0e-5;     // qOmega: turn-rate random walk, (rad/s)^2/s
    double omegaTauSec = 30;                                        // the turn rate of the coordinated-turn model relaxes with this time constant
    double sojournSec[3] = {300, 20, 60};                            // mean time in the CV / CT / manoeuvre model
    double qVertLevel = 0.002, qVertClimb = 0.25;                    // vertical acceleration noise
    double vertSojournSec[2] = {240, 90};
    double covInflate = 1.0;                                         // scales every process-noise term (calibration knob)
    double climbQBoost = 10;                                         // horizontal process noise grows by up to this factor at 6 m/s or more of climb or descent (arrivals slow down and turn)
    double nisMax = 400;                                             // adaptive process noise: up to this factor when the innovations are larger than the filter expects
    double latencySec[kSrcCount] = {0.2, 0.2, 15, 5, 15, 0};       // typical age of a position when `t` is the receive time (GNSS latency; FMS report to transmission)
    double initSpeedSigmaMs = 140;
    double gateChi2 = 21.0;                                          // 2-dof Mahalanobis gate on a position (about 1 - 4e-5)
    double maxSpeedMs = 450;                                         // implied speed above this is an outlier (override for military traffic)
    int reinitAfter = 2;                                             // consecutive consistent rejects that restart the track
    double maxGapSec = 3 * 3600;                                     // a longer silence restarts the track
    double rebaseM = 100e3;                                          // move the frame origin when the track is this far from it
    double maxStepSec = 10;                                          // prediction sub-step
    double retroWindowSec = 120;                                     // late fixes inside this window are re-fitted
    int retroDepth = 6;                                              // snapshots kept for it
    double predictedAfterSec = 6;                                    // PosEstimate::predicted when the last position is older (ADS-B / UAT)
    double predictedAfterSatSec = 20;                                // the same for ACARS / VDL2 / HFDL
    // route prior
    double routeXtBaseM = 4000, routeXtFrac = 0.05, routeXtRemFrac = 0.12, routeHeadingFrac = 0.25;   // sigma of the cross-track pseudo-measurement: base + flown + share of the distance still to go
    double routeMaxHeadingErrDeg = 35;
};

struct Ellipse { double majorM = 0, minorM = 0, orientDeg = 0; };   // 95 % semi-axes; orientation of the major axis from north, clockwise

struct PosEstimate {
    bool valid = false;
    double lat = 0, lon = 0, altM = 0, altFt = 0;
    double trackDeg = 0, speedKt = 0, vrateFpm = 0, turnRateDegS = 0;   // turn rate: clockwise positive
    double sigmaH = 0, sigmaV = 0;            // 1-sigma, metres (sigmaH: root of the mean of the two horizontal variances)
    Ellipse ellipse;
    double cEE = 0, cEN = 0, cNN = 0;         // horizontal covariance, m^2, in the track's local frame (east, north)
    double srcWeight[kSrcCount] = {};         // share of each source in the recent position fixes
    Src mainSrc = Src::Other;
    double ageSec = 0;                        // since the last position fix
    bool predicted = false;                   // dead reckoning: the last position fix is older than the options say
    bool interpolated = false;                // between fixes, the last one is not older than that
    double confidence = 0, staleness = 0;     // confidence 1 (a few metres) - 0 (300 km); staleness: age / one hour, 0 - 1
    double modelProb[3] = {};                 // CV, CT, manoeuvre
    bool vertClimbing = false;
    bool routeUsed = false, arrived = false, coarse = false;
    double etaT = 0;                          // when the destination is reached (clock of the fixes), 0 unknown
    double windE = 0, windN = 0;              // wind the track knows, m/s toward east/north
    double posAgeSec = 0;
};

enum class UpdateResult { Accepted, Started, Restarted, RejectedGate, RejectedSpeed, RejectedLate, NoPosition, Retrofitted };

struct RouteHint {
    bool hasDest = false, hasOrigin = false;
    double destLat = 0, destLon = 0, origLat = 0, origLon = 0;
    double etaT = 0;                          // reported arrival time on the fixes' clock (0 none)
};

class TrackEstimator {
public:
    explicit TrackEstimator(const TrackOptions& o = TrackOptions());
    UpdateResult update(const Fix& f);
    bool estimate(double now, PosEstimate& out, bool quick = false) const;   // quick: mean only (no covariance, no route)
    void setRoute(const RouteHint& r) { route_ = r; }
    void clearRoute() { route_ = RouteHint(); }
    const RouteHint& route() const { return route_; }
    void setWindPrior(double eMs, double nMs, double sigmaMs);   // used until the track has a wind of its own
    bool started() const { return c_.init; }
    double lastFixT() const { return c_.lastPosT; }
    uint32_t accepted() const { return nAccepted_; }
    uint32_t rejected() const { return nRejected_; }
    const TrackOptions& options() const { return o_; }

    // the filter state; public so the late-fix snapshots and the tests can copy it
    struct Core {
        bool init = false, vinit = false;
        Frame fr;
        double t = 0, vt = 0;                                    // filter times (horizontal, vertical)
        double hmu[3] = {0.8, 0.1, 0.1};
        Mat<5, 1> hx[3];                                         // east, north, v east, v north, omega (math sense: counter-clockwise positive)
        Mat<5, 5> hP[3];
        double vmu[2] = {0.8, 0.2};
        Mat<2, 1> vx[2];                                         // altitude m, vertical speed m/s
        Mat<2, 2> vP[2];
        double lastPosT = -1e18, lastPosLat = 0, lastPosLon = 0, lastPosE = 0, lastPosN = 0;
        Src lastSrc = Src::Other;
        double srcW[kSrcCount] = {};
        double interval = 0;
        int nPos = 0, rejectRun = 0;
        double rejE[2] = {0, 0}, rejN[2] = {0, 0}, rejT[2] = {0, 0};
        double windE = 0, windN = 0, windVar = 400.0, windT = -1e18;
        double pendSpeed = 0, pendTrack = 0, pendSig = 0, pendT = -1e18;
        bool hasVelEver = false;
        double nis = 1.0, qScale = 1.0;                          // smoothed normalised innovation (expected 1) and the process-noise scale it asks for
    };
private:
    struct Snap { Core core; Fix fix; };
    UpdateResult process(const Fix& f);
    TrackOptions o_;
    RouteHint route_;
    Core c_;
    uint32_t nAccepted_ = 0, nRejected_ = 0;
    std::deque<Snap> hist_;
};

// the 1-sigma position noise of a fix (metres), for the tests and the documentation
double fixPosSigmaM(const Fix& f, double speedMs);
double nacpEpuM(int nacp);        // 95 % bound of the NACp category (ICAO Doc 9871 / DO-260B table), 0 when unknown
double nicRcM(int nic);           // containment radius of the NIC category, 0 when unknown

}  // namespace dect2::track
