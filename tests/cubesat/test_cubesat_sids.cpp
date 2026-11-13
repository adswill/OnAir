// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SatNOGS DB live forwarding (round 4 #30): the SiDS form as gr-satellites' telemetry forwarder posts it (field names, upper-case hex frame,
// millisecond UTC time with Z, N/S and E/W suffixes, URL encoding of the callsign), and the packets that must not go out.
#include "dect2/cubesat/cubesat_smart.h"
#include "../common/test_util.h"
using namespace dect2::cubesat;

int main() {
    SmartPacket p;
    p.norad = 43803; p.wallTime = 1'800'000'000; p.data = {0x8a, 0xa6, 0x0f, 0xff, 0x00};
    const std::string f = sidsForm(p, "LY2XX/P", 54.6872, -25.2797, 1'800'000'000'123LL);
    CHECK(f == "noradID=43803&source=LY2XX%2FP&timestamp=2027-01-15T08%3A00%3A00.123Z&frame=8AA60FFF00&locator=longLat&longitude=25.2797W&latitude=54.6872N&version=OnAir%203.9", "form: %s", f.c_str());
    CHECK(sidsForm(p, "LY2XX", -33.9, 151.2).find("&longitude=151.2000E&latitude=33.9000S") != std::string::npos && sidsForm(p, "LY2XX", 0, 0).find("timestamp=2027-01-15T08%3A00%3A00.000Z") != std::string::npos, "south, east, whole seconds");
    SmartPacket file = p; file.wallTime = 0;
    SmartPacket noNorad = p; noNorad.norad = 0;
    SmartPacket empty = p; empty.data.clear();
    CHECK(sidsForm(file, "LY2XX", 1, 1).empty() && sidsForm(noNorad, "LY2XX", 1, 1).empty() && sidsForm(empty, "LY2XX", 1, 1).empty() && sidsForm(p, "", 1, 1).empty(), "not sent: a file, no NORAD, no bytes, no callsign");
    CHECK(std::string(kSidsSatnogsUrl) == "https://db.satnogs.org/api/telemetry/", "address");
    return testSummary("cubesat_sids");
}
