// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SNR each OnAir mode needs to decode, as measured by OnAir's own sensitivity sweep (docs/SENSITIVITY.md, tools/sens/sens_modes*.h):
// the lowest SNR with at least 90 % of trials decoding, in the mode's noise bandwidth, for white noise only, for the "combined"
// impairment set (2 ppm carrier + 20 ppm clock + echo + phase noise) and for TU6 Rayleigh fading. Measured on generated signals, so
// they are receiver thresholds, not field results. The propagation tool uses them to answer "will mode X decode here?".
#pragma once
#include <string>
#include <vector>

namespace dect2::prop {

struct ModeSnr {
    const char* id;          // the sensitivity sweep's mode id
    const char* name;
    double bwHz;             // the bandwidth the SNR refers to
    double awgnDb;           // threshold SNR, white noise
    double combinedDb;       // with the combined impairments (kNoValue: not measured)
    double fadingDb;         // TU6 fading (kNoValue: not measured)
    const char* source;      // which document the numbers come from
    static constexpr double kNoValue = -999;
};
const std::vector<ModeSnr>& modeThresholds();
const ModeSnr* findModeThreshold(const std::string& id);
// the threshold for a channel condition: 0 white noise, 1 combined impairments (falls back to white noise), 2 fading (falls back to 1)
double modeRequiredSnrDb(const ModeSnr& m, int condition);

} // namespace dect2::prop
