// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The published band plan of the QO-100 narrowband transponder (AMSAT-DL / BATC, downlink 10489.500 - 10490.000 MHz), a check of a signal
// against it, signal levels relative to the middle beacon (comparable between sessions and set-ups), and the plain-words status line.
#pragma once
#include "dect2/qo100/qo100_tel.h"
#include <string>

namespace dect2 {
namespace qo100 {

enum PlanUse { kPlanBeacon = 0, kPlanCw, kPlanNarrowDigital, kPlanDigital, kPlanSsb, kPlanMixed, kPlanUses };

struct PlanSegment {
    double loHz, hiHz;      // downlink RF, true frequency
    PlanUse use;
    const char* name;       // plain words
    double maxBwHz;         // widest signal allowed there (0 for a beacon: nothing may transmit)
};

const PlanSegment* bandPlan(int& count);                 // lowest first, gap-free from 10489.500 to 10490.000 MHz
const PlanSegment* segmentAt(double rfHz);               // nullptr outside the transponder
// "" when the signal fits its segment; else the reason in plain words (too wide for the segment, in a beacon segment, spans two segments)
std::string planProblem(double centreRfHz, double widthHz);
// Level of a signal relative to the middle (PSK) beacon, dB; false without a beacon in the report
bool levelOverBeaconDb(const Qo100Telemetry& t, double levelDb, double& relDb);
// One plain sentence on how the reception is doing and what to do next
std::string plainStatus(const Qo100Telemetry& t, bool running);

} // namespace qo100
} // namespace dect2
