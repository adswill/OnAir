// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/qo100/qo100_plan.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace qo100 {

const PlanSegment* bandPlan(int& count) {
    static const PlanSegment k[] = {
        {10489.500e6, 10489.505e6, kPlanBeacon, "lower CW beacon", 0},
        {10489.505e6, 10489.540e6, kPlanCw, "CW only", 500},
        {10489.540e6, 10489.580e6, kPlanNarrowDigital, "narrow digital (up to 500 Hz)", 500},
        {10489.580e6, 10489.650e6, kPlanDigital, "digital (up to 2.7 kHz)", 2700},
        {10489.650e6, 10489.745e6, kPlanSsb, "SSB only", 2700},
        {10489.745e6, 10489.755e6, kPlanBeacon, "middle beacon (400 bit/s BPSK)", 0},
        {10489.755e6, 10489.850e6, kPlanSsb, "SSB only", 2700},
        {10489.850e6, 10489.995e6, kPlanMixed, "mixed and special modes (up to 2.7 kHz)", 2700},
        {10489.995e6, 10490.000e6, kPlanBeacon, "experimental beacon", 0},
    };
    count = (int)(sizeof k / sizeof k[0]);
    return k;
}

const PlanSegment* segmentAt(double rfHz) {
    int n = 0; const PlanSegment* p = bandPlan(n);
    for (int i = 0; i < n; i++) if (rfHz >= p[i].loHz && rfHz < p[i].hiHz) return &p[i];
    return nullptr;
}

std::string planProblem(double centreRfHz, double widthHz) {
    const PlanSegment* s = segmentAt(centreRfHz);
    if (!s) return "outside the narrowband transponder";
    if (s->use == kPlanBeacon) return "";   // the beacons themselves
    if (widthHz > s->maxBwHz * 1.15) {
        char b[96]; snprintf(b, sizeof b, "%.0f Hz wide, the %s segment allows %.0f Hz", widthHz, s->name, s->maxBwHz);
        return b;
    }
    return "";
}

bool levelOverBeaconDb(const Qo100Telemetry& t, double levelDb, double& relDb) {
    for (const auto& s : t.signals) if (s.beacon == 1 && s.levelDb > -150) { relDb = levelDb - s.levelDb; return true; }
    return false;
}

ClickTune clickTune(double topHz, double widthHz) {
    const PlanSegment* seg = segmentAt(topHz - 0.5 * widthHz);
    const bool cw = widthHz < 400 && (!seg || seg->use == kPlanCw || seg->use == kPlanNarrowDigital || seg->use == kPlanBeacon);
    if (cw) return {topHz - 700, "CW"};
    const bool digital = seg && (seg->use == kPlanDigital || seg->use == kPlanNarrowDigital);
    return {topHz - widthHz - 300, digital ? "USB (digital)" : "USB (SSB)"};
}

DatvQuality datvQuality(double esN0Db, bool decoding, bool tsLock) {
    if (decoding && tsLock) return esN0Db >= kDatvThresholdDb + 6 ? kDatvGood : kDatvLocked;
    if (esN0Db < 1.0) return kDatvNone;
    if (esN0Db < kDatvThresholdDb + 1.5) return kDatvWeak;
    return esN0Db >= kDatvThresholdDb + 6 ? kDatvGood : kDatvUsable;
}

const char* datvQualityName(DatvQuality q) {
    static const char* const k[] = {"no signal", "weak", "strong enough to try", "locked", "good"};
    return k[(int)q];
}

std::string LnbMemory::serialize() const {
    std::string o;
    for (const auto& kv : m_) { char b[64]; snprintf(b, sizeof b, "%s%lld:%.4f", o.empty() ? "" : ";", kv.first, kv.second); o += b; }
    return o;
}

void LnbMemory::parse(const std::string& s) {
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(';', at); if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(at, e - at);
        at = e + 1;
        const size_t c = item.find(':');
        if (c == std::string::npos || c == 0) continue;
        char* end1 = nullptr; char* end2 = nullptr;
        const long long k = std::strtoll(item.c_str(), &end1, 10);
        const double v = std::strtod(item.c_str() + c + 1, &end2);
        if (end1 != item.c_str() + c || end2 == item.c_str() + c + 1 || !std::isfinite(v) || std::fabs(v) > 500) continue;
        m_[k] = v;
    }
}

std::string plainStatus(const Qo100Telemetry& t, bool running) {
    char b[200];
    if (!running) return "Start the receiver. It finds the beacons and corrects your LNB by itself.";
    if (t.rate <= 0) return "Waiting for samples from the radio.";
    if (t.state == 0) return "The narrowband transponder is outside the radio's band: press Narrowband only or Whole downlink.";
    if (t.state == 1 && t.seeded) {
        snprintf(b, sizeof b, "Looking for the beacons. Meanwhile the %.1f kHz LNB error remembered from last time is applied.", std::fabs(t.seedErrHz) / 1e3);
        return b;
    }
    if (t.state == 1) return "Looking for the two CW beacons. Check the dish points at QO-100 and the LO setting matches your LNB.";
    const double k = std::fabs(t.lnbErrHz) / 1e3;
    const char* held = t.state == 3 ? " Beacons lost for now: using the last correction." : "";
    if (!t.corrApplied) snprintf(b, sizeof b, "Beacons found. The LNB is %.1f kHz %s; correction is off.%s", k, t.lnbErrHz < 0 ? "low" : "high", held);
    else if (k < 0.05) snprintf(b, sizeof b, "Locked on the beacons. Your LNB is spot on.%s", held);
    else snprintf(b, sizeof b, "Locked on the beacons. Your LNB runs %.1f kHz %s; every frequency is corrected for it.%s", k, t.lnbErrHz < 0 ? "low" : "high", held);
    return b;
}

} // namespace qo100
} // namespace dect2
