#pragma once
// The sample rates the app offers for a mode on a radio, and the check of a typed one (the "Sample rate" control under the frequency).
// Pure functions of the radio's limits and the mode's minimum: tests/test_rate_choice.cpp.
#include "source.h"
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct RateLimits {
    double minHz = 0, maxHz = 0;                        // 0 = not known
    std::vector<std::pair<double, double>> ranges;      // DeviceInfo::rateRanges: empty = any rate between minHz and maxHz
};

// The limits of a radio: what its driver said, a HackRF's 2 to 20 Msps. Files and the test signal have none (both 0).
RateLimits rateLimitsOf(const DeviceInfo& d);

// The rate the radio really runs at when asked for `hz`: the smallest it offers at or above it (as the drivers pick), else its fastest; `hz`
// itself when nothing is known.
double deliveredRate(const RateLimits& L, double hz);

struct RateEntry { double askHz = 0, getHz = 0; };   // what the app asks for, what the radio delivers (the same unless the radio rounds)

// The dropdown: the radio's own rates when its driver lists single rates, else a ladder from 0.25 to 20 Msps; only rates within the radio's
// limits that deliver at least the mode's minimum (modeMinHz, 0 = none). Sorted, without two entries delivering the same rate.
std::vector<RateEntry> rateEntries(const RateLimits& L, double modeMinHz);

struct RateCheck { bool ok = false; double hz = 0; std::string why; };   // hz: the rate the radio will deliver; why: one line when refused
// A typed rate: within the radio's limits, not in a gap between its ranges, and the rate delivered at least the mode's minimum.
RateCheck checkManualRate(const RateLimits& L, double modeMinHz, double hz);

} // namespace dect2
