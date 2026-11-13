// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CubeSat beacon interval learner (3.9, docs/SMART5-sat-3.9.md item 20). Most CubeSats beacon on a fixed period (10 s to a few
// minutes). From the beacons decoded, over passes and with missed beacons in between, this learns each satellite's period and
// jitter, announces when the next beacon is due, and flags a beacon that did not come while the satellite was up (a satellite
// in safe mode, a changed schedule, or weak reception at that moment).
#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct BeaconState {
    double periodSec = 0, jitterSec = 0;
    double last = 0, nextDue = 0;
    int heard = 0, missedWhileUp = 0;
    bool overdue = false;
};

class BeaconLearn {
public:
    void beacon(int norad, double t);
    void tick(int norad, double now, bool satelliteUp);
    BeaconState state(int norad) const;
    std::string text(int norad, const std::string& name, double now) const;
private:
    struct S { std::deque<double> t; BeaconState st; double lastJudged = 0; };
    std::map<int, S> s_;
    static void learn(S& s);
};

} // namespace smart5
} // namespace dect2
