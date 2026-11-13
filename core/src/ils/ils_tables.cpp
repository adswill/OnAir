// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ILS localizer / glide path frequency pairing, ICAO Annex 10 Volume I, 3.1.6.1 (checked against the list at sigidwiki.com).
#include "dect2/ils/ils.h"
#include <cmath>

namespace dect2 {

static const double kPairs[40][2] = {
    {108.10, 334.70}, {108.15, 334.55}, {108.30, 334.10}, {108.35, 333.95}, {108.50, 329.90}, {108.55, 329.75}, {108.70, 330.50}, {108.75, 330.35},
    {108.90, 329.30}, {108.95, 329.15}, {109.10, 331.40}, {109.15, 331.25}, {109.30, 332.00}, {109.35, 331.85}, {109.50, 332.60}, {109.55, 332.45},
    {109.70, 333.20}, {109.75, 333.05}, {109.90, 333.80}, {109.95, 333.65}, {110.10, 334.40}, {110.15, 334.25}, {110.30, 335.00}, {110.35, 334.85},
    {110.50, 329.60}, {110.55, 329.45}, {110.70, 330.20}, {110.75, 330.05}, {110.90, 330.80}, {110.95, 330.65}, {111.10, 331.70}, {111.15, 331.55},
    {111.30, 332.30}, {111.35, 332.15}, {111.50, 332.90}, {111.55, 332.75}, {111.70, 333.50}, {111.75, 333.35}, {111.90, 331.10}, {111.95, 330.95},
};

int ilsChannelCount() { return 40; }
void ilsChannel(int i, double& loc, double& gs) { loc = kPairs[i][0]; gs = kPairs[i][1]; }
bool ilsIsLocalizer(double mhz) { return ilsGlideslopeFor(mhz) > 0; }
double ilsGlideslopeFor(double loc) {
    for (const auto& p : kPairs) if (std::fabs(p[0] - loc) < 0.005) return p[1];
    return 0;
}
double ilsLocalizerFor(double gs) {
    for (const auto& p : kPairs) if (std::fabs(p[1] - gs) < 0.005) return p[0];
    return 0;
}

} // namespace dect2
