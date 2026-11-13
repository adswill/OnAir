// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Message 27 and the long-range channel plan (ais_longrange.h).
#include "dect2/ais/ais_longrange.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace ais {

Bits encodeLongRange(const LongRangeReport& r) {
    Bits b;
    putU(b, 0, 6, 27);
    putU(b, 6, 2, (uint32_t)(r.repeat & 3));
    putU(b, 8, 30, r.mmsi & 0x3FFFFFFFu);
    putU(b, 38, 1, r.posAccuracy ? 1 : 0);
    putU(b, 39, 1, r.raim ? 1 : 0);
    putU(b, 40, 4, (uint32_t)(r.navStatus >= 0 && r.navStatus <= 15 ? r.navStatus : 15));
    const bool posOk = std::fabs(r.lat) <= 90 && std::fabs(r.lon) <= 180;
    putS(b, 44, 18, posOk ? (int32_t)std::lround(r.lon * 600.0) : 181 * 600);
    putS(b, 62, 17, posOk ? (int32_t)std::lround(r.lat * 600.0) : 91 * 600);
    putU(b, 79, 6, (uint32_t)(r.sogKn >= 0 && r.sogKn <= 62 ? r.sogKn : 63));
    putU(b, 85, 9, (uint32_t)(r.cogDeg >= 0 && r.cogDeg <= 359 ? r.cogDeg : 511));
    putU(b, 94, 1, r.latencyOver5s ? 1 : 0);
    putU(b, 95, 1, 0);   // spare
    b.resize(96, 0);
    return b;
}

bool decodeLongRange(const Bits& payload, LongRangeReport& out) {
    AisMsg m;
    if (!decodeMessage(payload, m) || m.type != 27) return false;
    out = LongRangeReport();
    out.repeat = m.repeat;
    out.mmsi = m.mmsi;
    out.posAccuracy = m.posAccuracy == 1;
    out.raim = m.raim == 1;
    out.navStatus = m.navStatus;
    if (m.hasPos) { out.lat = m.lat; out.lon = m.lon; }
    out.sogKn = m.sog >= 0 ? (int)std::lround(m.sog) : 63;
    out.cogDeg = m.cog >= 0 ? (int)std::lround(m.cog) : 511;
    out.latencyOver5s = m.posLatency != 0;
    return true;
}

static bool pairFits(double midHz, double centreHz, double rate) { return std::fabs(midHz - centreHz) + kPairMarginHz <= 0.49 * rate; }

int aisChannelsCovered(double centreHz, double rate) {
    int m = 0;
    if (pairFits(kAisMidHz, centreHz, rate)) m |= 3;
    if (pairFits(kLongRangeMidHz, centreHz, rate)) m |= 12;
    return m;
}

double aisWideRateHz() {
    const double half = std::max(kAisMidHz - kAisWideCentreHz, kAisWideCentreHz - kLongRangeMidHz) + kPairMarginHz;
    return half / 0.49;
}

std::vector<AisTuneStep> aisTunePlan(double rate, double aisDwell, double lrDwell) {
    std::vector<AisTuneStep> p;
    if (aisChannelsCovered(kAisWideCentreHz, rate) == 15) { p.push_back({kAisWideCentreHz, 0, 15}); return p; }
    p.push_back({kAisMidHz, aisDwell, aisChannelsCovered(kAisMidHz, rate)});
    p.push_back({kLongRangeMidHz, std::max(lrDwell, kLongRangeIntervalSec), aisChannelsCovered(kLongRangeMidHz, rate)});
    return p;
}

} // namespace ais
} // namespace dect2
