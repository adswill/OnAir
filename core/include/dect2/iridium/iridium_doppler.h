// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium satellite identification and sky position from Doppler against TLEs (3.7, docs/SMART-maritime-3.7.md).
// The ring-alert / broadcast channel (1626.2708 MHz) is on a known frequency, so the offset of a burst from it is the Doppler shift of the satellite
// that sent it plus the receiver's own frequency error. The header names the satellite only by Iridium's internal number (0..127); this matches the
// Doppler curve of each such number against the predicted curve of every Iridium satellite in the user's TLE set (those above the horizon), takes the
// one that fits, and from the fit gives:
//  - the NORAD number and name of the satellite behind each number, with a confidence (how much better than the runner-up),
//  - the receiver's frequency error in Hz and ppm (the common offset of the identified ones: a free calibration),
//  - where every identified satellite is in the sky now (azimuth, elevation, range) for a sky plot,
//  - a ground circle for a terminal that a message came from: the beam centre the satellite announced (IRA), carried along with the satellite's motion.
// Pure logic: no clock, no radio; the TLEs come from the pass planner's files (nothing is downloaded here). Thread safe.
#pragma once
#include "dect2/geo/passes.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

constexpr double kIridiumRingHz = 1626270833.0;     // the ring alert / broadcast channel

struct IridiumIdent {
    int satId = -1;                 // the number in the header
    int norad = 0;
    std::string name;
    int obs = 0;                    // measurements behind it
    double rmsHz = 0;               // residual of the fit
    double confidence = 0;          // 0 .. 1: how far the runner-up is behind
    bool sure = false;              // confident enough to use for the calibration
    double azDeg = 0, elDeg = 0, rangeKm = 0, dopplerHz = 0;   // at the time asked
    double lastUnix = 0;
};

struct IridiumBeamCircle {
    bool valid = false;
    double lat = 0, lon = 0, radiusKm = 250;
    double ageSec = 0;
    bool moved = false;             // carried along with the satellite's motion (the satellite was identified)
};

class IridiumDoppler {
public:
    struct Config {
        double minElDeg = 5;
        double windowSec = 1200;    // measurements older than this are dropped
        int minObs = 3;             // measurements before a number is identified
        int maxObs = 80;
        double minSpanSec = 20;     // and they must span this long (a curve, not a point)
        double sureMargin = 2.0;    // runner-up rms >= margin x best + 60 Hz
        double maxOffsetHz = 40000; // the receiver error the fit may absorb (a poor TCXO at 1.6 GHz)
        double beamMaxAgeSec = 180;
    };
    IridiumDoppler() = default;
    explicit IridiumDoppler(const Config& c) : cfg_(c) {}

    void setObserver(const Observer& o);
    void setTles(const std::vector<Tle>& all);          // keeps the Iridium ones (name starts with IRIDIUM, not DEB)
    size_t satellites() const;

    // A burst of satellite number satId on the ring channel: unixSec the time, freqHz the absolute carrier (or offHz from kIridiumRingHz)
    void addBurst(double unixSec, int satId, double freqHz);
    // A ring alert's beam centre (lat/lon) for satellite number satId and beam
    void addBeam(double unixSec, int satId, int beam, double lat, double lon);

    std::vector<IridiumIdent> identify(double nowUnix);                 // best first
    double offsetHz() const;                                            // the receiver's frequency error from the sure identifications (0 when none)
    bool offsetKnown() const;
    // every Iridium satellite above the horizon at nowUnix (the sky plot); `ident` marks the ones identified with the header number
    struct SkyPoint { int norad = 0; std::string name; double azDeg = 0, elDeg = 0, rangeKm = 0, dopplerHz = 0; int satId = -1; };
    std::vector<SkyPoint> sky(double nowUnix);
    IridiumBeamCircle beamCircle(int satId, int beam, double nowUnix);
    void clear();

private:
    struct Obs { double t, f; };
    struct Cand { Tle tle; Sgp4 sgp; };
    struct Fit { int cand = -1; double rms = 1e18, off = 0, second = 1e18; int n = 0; };
    struct Beam { double t = 0, lat = 0, lon = 0; };
    Fit fitSat(const std::vector<Obs>& obs, bool useOffset, double off) const;
    void refreshLocked(double now);

    mutable std::mutex mu_;
    Config cfg_;
    Observer obs_;
    bool haveObs_ = false;
    std::vector<Cand> cand_;
    std::map<int, std::vector<Obs>> bursts_;
    std::map<std::pair<int, int>, Beam> beams_;
    std::map<int, Fit> fit_;
    double calOff_ = 0; bool calKnown_ = false;
    double lastRefresh_ = -1e18;
};

} // namespace dect2
