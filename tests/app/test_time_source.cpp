// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One clock for all modes (core/include/dect2/common/time_source.h): the computer clock wins when a result was stamped, the signal
// time is converted otherwise, and the ISO 8601 text carries milliseconds and the UTC offset.
#include "dect2/common/time_source.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace dect2::timesrc;

int main() {
    // Computer: a stamped result keeps its stamp, whatever the signal clock did (lost samples, Stop / Start)
    CHECK(epoch(Source::Computer, 1000.5, 10, 500, 2000) == 1000.5, "stamp wins");
    CHECK(epoch(Source::Computer, 0, 10, 500, 2000) == 2000 - 490, "unstamped falls back to the signal time");
    CHECK(epoch(Source::Signal, 1000.5, 10, 500, 2000) == 1510, "signal mode ignores the stamp");
    CHECK(epoch(Source::Computer, 0, -1, 500, 2000) == 0 && epoch(Source::Signal, 5, -1, 5, 5) == 0, "unknown stays 0");
    CHECK(stamp(Source::Signal) == 0 && stamp(Source::Computer) > 1.7e9, "stamp");
    setSource(Source::Signal); CHECK(source() == Source::Signal && stamp() == 0, "global signal");
    setSource(Source::Computer); CHECK(source() == Source::Computer, "global computer");
    // ISO 8601
    const std::string u = iso8601(1760000000.123, dect2::timefmt::Zone::Utc);
    CHECK(u == "2025-10-09T08:53:20.123+00:00", "utc: %s", u.c_str());
    CHECK(iso8601(1760000000.9996, dect2::timefmt::Zone::Utc) == "2025-10-09T08:53:21.000+00:00", "ms rounds up into the next second");
    CHECK(iso8601(0, dect2::timefmt::Zone::Utc).empty(), "not set");
    const std::string l = iso8601(1760000000.0, dect2::timefmt::Zone::Local);
    CHECK(l.size() == 29 && (l[23] == '+' || l[23] == '-'), "local has an offset: %s", l.c_str());
    if (failures == 0) std::printf("time_source: ok\n");
    return failures ? 1 : 0;
}
