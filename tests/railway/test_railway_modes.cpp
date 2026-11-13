// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tuning/active distinction is an engine contract, not an off-by-one bug.
#include "dect2/railway/railway_rx.h"
#include "../../core/src/engine/engine_internal.h"
#include "../common/test_util.h"
#include <string>
using namespace dect2;
using namespace dect2::railway;
int main() {
    const auto klw = klwTuning(), tdcs = tdcsTuning();
    CHECK(klw.stdMode == 148 && tdcs.stdMode == 149, "stable engine standards");
    CHECK(activeStdFor(klw.stdMode) == kKlwActiveStandard && activeStdFor(tdcs.stdMode) == kTdcsActiveStandard, "tuning follows the engine's activeStdFor conversion");
    CHECK(std::string(klw.id) == "klw" && std::string(tdcs.id) == "tdcs" && klw.defMhz == 866.2375 && tdcs.defMhz == 457.55, "each standard has its own identity and default frequency");
    // A panel can show the current receiver, but never a stale snapshot while
    // switching modes (including the initial no-input snapshot).
    CHECK(matchesMode(kKlwFamily, activeStdFor(klw.stdMode)), "KLW panel accepts KLW");
    CHECK(matchesMode(kTdcsFamily, activeStdFor(tdcs.stdMode)), "TDCS panel accepts TDCS");
    CHECK(!matchesMode(kKlwFamily, activeStdFor(tdcs.stdMode)) && !matchesMode(kTdcsFamily, activeStdFor(klw.stdMode)), "mode switch rejects the other receiver's snapshot");
    CHECK(!matchesMode(kKlwFamily, 0) && !matchesMode(kTdcsFamily, 82) && !matchesMode(81, kKlwActiveStandard), "unrelated UI/telemetry modes cannot display railway data");
    return testSummary("test_railway_modes");
}
