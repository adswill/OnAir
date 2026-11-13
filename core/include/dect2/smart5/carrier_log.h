// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// L-band carrier quality log (3.9, docs/SMART5-sat-3.9.md item 19). For Inmarsat, Iridium, Thuraya and other L-band carriers:
// per carrier, the SNR and decoded frames per minute over time, and each outage (no frames for a while after the carrier was
// decoding) with its likely reason from the marks the caller gives: a sun outage in progress, the satellite low in the sky
// (below 10 deg), or unexplained (local interference, a blocked view, the network).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

enum class OutageReason { Unexplained, SunOutage, LowElevation, SignalFade };

struct CarrierMinute { double t = 0; double snrDb = 0; int frames = 0; };
struct CarrierOutage { double start = 0, end = 0; OutageReason reason = OutageReason::Unexplained; double snrBeforeDb = 0; };

struct CarrierSample {
    double snrDb = 0;
    int frames = 0;           // decoded since the last sample
    double elDeg = 90;        // the satellite's elevation, if known
    bool sunOutage = false;   // from sun_outage.h for geostationary carriers
};

class CarrierLog {
public:
    explicit CarrierLog(double outageSec = 20) : outSec_(outageSec) {}
    void sample(double freqHz, double t, const CarrierSample& s);
    std::vector<CarrierMinute> minutes(double freqHz) const;
    std::vector<CarrierOutage> outages(double freqHz) const;
    double availabilityPct(double freqHz) const;   // share of minutes with frames
    std::vector<double> carriers() const;
    static const char* reasonName(OutageReason r);
private:
    struct C {
        std::vector<CarrierMinute> min;
        std::vector<CarrierOutage> out;
        double lastFrame = -1, snrSum = 0, snrPrev = 0; int n = 0, frames = 0; double minStart = -1;
        bool inOut = false; CarrierOutage cur; bool sun = false, low = false; double snrMin = 1e9;
    };
    std::map<long long, C> c_;
    double outSec_;
    static long long key(double f) { return (long long)(f / 100 + 0.5); }   // 100 Hz cells
};

} // namespace smart5
} // namespace dect2
