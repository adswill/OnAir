// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cospas-Sarsat 406 MHz beacon incidents (4.0): groups the decoded bursts by the beacon's ID and says what each one is. Pure code on sarsat::Beacon.
//  - burst count, first and last time, and the interval between bursts (a real beacon repeats about every 50 s with a random spread of +-5 s;
//    an interval much shorter or exactly regular is not a beacon, it is a decoder echo or a test transmitter),
//  - self-test frames are kept apart ("test") so an annual test does not look like a distress,
//  - position drift: how far the reported position moved between the first and last burst (a beacon that moves is on a vessel or a person),
//  - an "alert" rank: valid, not a test, has a position, and at least 2 bursts with plausible intervals.
#pragma once
#include "dect2/sarsat/sarsat_proto.h"
#include "dect2/track/track_geo.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct SarsatIncident {
    std::string hexId, beaconType, country;
    int bursts = 0, tests = 0;
    double firstS = 0, lastS = 0;
    double medianGapS = 0;
    bool plausibleRepeat = false;    // median gap 40..70 s with at least 3 gaps
    bool hasPos = false; double lat = 0, lon = 0, driftKm = 0;
    bool alert = false;
};

class SarsatIncidents {
public:
    void add(const sarsat::Beacon& b, double tS) {
        if (!b.valid || b.hexId.empty()) return;
        Rec& r = m_[b.hexId];
        r.beaconType = b.beaconType; r.country = b.country;
        if (b.selfTest) { if (++r.tests == 1) r.firstTest = tS; r.lastTest = tS; return; }
        if (!r.times.empty() && tS - r.times.back() < 2.0) return;   // the same burst decoded twice
        r.times.push_back(tS);
        if (b.hasPos) { if (!r.hasPos) { r.lat0 = b.lat; r.lon0 = b.lon; } r.hasPos = true; r.lat1 = b.lat; r.lon1 = b.lon; }
    }
    std::vector<SarsatIncident> list() const {
        std::vector<SarsatIncident> out;
        for (const auto& kv : m_) {
            const Rec& r = kv.second; SarsatIncident i;
            i.hexId = kv.first; i.beaconType = r.beaconType; i.country = r.country; i.tests = r.tests; i.bursts = (int)r.times.size();
            i.firstS = r.times.empty() ? r.firstTest : r.times.front(); i.lastS = r.times.empty() ? r.lastTest : r.times.back();
            std::vector<double> gaps;
            for (size_t k = 1; k < r.times.size(); k++) gaps.push_back(r.times[k] - r.times[k - 1]);
            if (!gaps.empty()) { std::sort(gaps.begin(), gaps.end()); i.medianGapS = gaps[gaps.size() / 2]; }
            i.plausibleRepeat = gaps.size() >= 3 && i.medianGapS >= 40 && i.medianGapS <= 70;
            if (r.hasPos) { i.hasPos = true; i.lat = r.lat1; i.lon = r.lon1; i.driftKm = track::gcDistM(r.lat0, r.lon0, r.lat1, r.lon1) / 1000.0; }
            i.alert = i.bursts >= 2 && i.hasPos && (i.plausibleRepeat || gaps.size() < 3);
            out.push_back(i);
        }
        std::sort(out.begin(), out.end(), [](const SarsatIncident& a, const SarsatIncident& b) { return a.alert != b.alert ? a.alert : a.lastS > b.lastS; });
        return out;
    }
private:
    struct Rec { std::string beaconType, country; std::vector<double> times; int tests = 0; double firstTest = 0, lastTest = 0; bool hasPos = false; double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0; };
    std::map<std::string, Rec> m_;
};

} // namespace dect2
