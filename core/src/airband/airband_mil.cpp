// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The military UHF air band: the published common channels (sources in airband_mil.h).
#include "dect2/airband/airband_mil.h"
#include <cmath>

namespace dect2 {

const std::vector<AirbandKnownChannel>& airbandMilUhfKnown() {
    static const std::vector<AirbandKnownChannel> v = {
        {243.000e6, "UHF guard", "military emergency and guard channel (the UHF counterpart of 121.5 MHz)"},
        {282.800e6, "SAR on-scene", "NATO joint / combined on-scene search and rescue and direction finding"},
    };
    return v;
}

bool airbandIsMilUhf(double f) { return f >= kMilUhfLoHz - 1 && f <= kMilUhfHiHz + 1; }

std::vector<AirbandChannel> airbandMilUhfChannels() {
    std::vector<AirbandChannel> out;
    for (const auto& k : airbandMilUhfKnown()) {
        AirbandChannel c;
        c.freqHz = k.freqHz; c.is833 = false; c.label = k.label;
        c.priority = std::fabs(k.freqHz - 243.000e6) < 1;   // guard: heard over the others
        out.push_back(c);
    }
    return out;
}

int airbandMilUhfChannelCount() { return (int)std::lround((kMilUhfHiHz - kMilUhfLoHz) / kMilUhfStepHz) + 1; }

} // namespace dect2
