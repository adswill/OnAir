// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multipath indicator (3.9, docs/SMART5-gnss-3.9.md item 23). A reflector (the ground, a roof, a wall) at height h below the
// antenna adds a delayed copy whose phase turns with the satellite's elevation: C/N0 then oscillates around its elevation trend
// with a frequency of 2 h / lambda cycles per unit of sin(elevation). Per satellite pass, this removes the smooth trend of C/N0
// against elevation, finds the strongest oscillation of the residual in sin(elevation), and reports its depth and the
// reflector height it implies. Fringes deeper than 1 dB flag multipath at that site and in that direction.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct MultipathResult {
    bool valid = false, multipath = false;
    double depthDb = 0;          // peak-to-peak of the fringe
    double cyclesPerSin = 0;     // fringe frequency
    double heightM = 0;          // reflector height below the antenna
    double azDeg = 0;            // mean azimuth of the pass
};

class GnssMultipath {
public:
    explicit GnssMultipath(double wavelengthM = 0.1903) : lambda_(wavelengthM) {}   // GPS L1
    void sample(int prn, double elDeg, double azDeg, double cn0DbHz);
    MultipathResult analyse(int prn) const;   // over the samples so far (call at the end of a pass)
    void clear(int prn) { s_.erase(prn); }
private:
    struct P { double el, az, cn0; };
    std::map<int, std::vector<P>> s_;
    double lambda_;
};

} // namespace smart5
} // namespace dect2
