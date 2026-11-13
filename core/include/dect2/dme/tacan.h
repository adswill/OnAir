// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TACAN bearing from one receiver: the pulse amplitudes carry 15 Hz and 135 Hz amplitude modulation from the beacon's rotating pattern, and reference bursts
// (MIL-STD-291C 5.4-5.6: main 15 Hz group of 12 pairs on X or 13 single pulses on Y, auxiliary 135 Hz group of 6 pairs on X or 13 pulses on Y) mark the rotation.
// Convention: the main burst is sent as the pattern maximum points due east, rotating clockwise, so a station-bound radial angle is 90 + phase. Validated on
// simulated pulses only; the convention comes from secondary descriptions of the signal, no recording of a TACAN beacon was available.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace dect2::dme {

class TacanBearing {
public:
    void reset();
    void pulse(double tSec, float amp);              // pulses must come in time order
    bool valid() const { return valid_; }
    double radialDeg() const { return radial_; }    // 0..360: the radial of the beacon the receiver sits on
    float depth15() const { return d15_; }
    float depth135() const { return d135_; }
    int mainBursts() const { return nMain_; }
private:
    struct P { double t; float a; };
    struct Cycle { double a[5][5] = {}, b[5] = {}, n = 0; };
    std::vector<P> cluster_;
    double lastT_ = -1, lastMain_ = -1;
    int nMain_ = 0, good_ = 0;
    std::deque<Cycle> cycles_;
    Cycle cur_;
    bool valid_ = false;
    double radial_ = 0;
    float d15_ = 0, d135_ = 0;
    void endCluster();
    void addToCycle(const P& p);
    void solve();
};

} // namespace dect2::dme
