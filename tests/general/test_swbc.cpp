// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shortwave broadcast band table: complete set, sorted, no overlaps, inside the HF range, and the band-plan template agrees.
#include "dect2/general/swbc.h"
#include "dect2/bandplan/bandplan.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    size_t n = 0;
    const SwBand* b = swBroadcastBands(n);
    const char* want[] = {"120 m", "90 m", "75 m", "60 m", "49 m", "41 m", "31 m", "25 m", "22 m", "19 m", "16 m", "15 m", "13 m", "11 m"};
    CHECK(n == 14, "14 bands, got %zu", n);
    for (const char* w : want) {
        bool f = false;
        for (size_t i = 0; i < n; i++) f = f || !strcmp(b[i].name, w);
        CHECK(f, "band %s present", w);
    }
    for (size_t i = 0; i < n; i++) {
        CHECK(b[i].startKhz < b[i].endKhz, "%s start < end", b[i].name);
        CHECK(b[i].startKhz >= 2300 && b[i].endKhz <= 30000, "%s inside HF", b[i].name);
        if (i) CHECK(b[i - 1].endKhz < b[i].startKhz, "%s sorted and apart from %s", b[i].name, b[i - 1].name);
        const double c = swBandCentreKhz(b[i]);
        CHECK(swBandAt(c) == &b[i], "%s centre maps back to the band", b[i].name);
        // a whole number of 5 kHz channels fits, edges on the raster
        CHECK(std::fmod(b[i].startKhz, 5) == 0 || b[i].startKhz == 13570 || b[i].startKhz == 25670, "%s start on the raster", b[i].name);
    }
    CHECK(!swBandAt(8000) && !swBandAt(14100), "outside the bands");
    CHECK(b[0].tropical && b[1].tropical && b[3].tropical && !b[4].tropical, "tropical flags 120/90/60 m");

    // the hull is the widest region range; region 2 41 m and the missing 75 m are as ITU says
    for (size_t i = 0; i < n; i++) {
        double lo = 1e9, hi = 0;
        for (int r = 1; r <= 3; r++) { double s0, e0; if (swBandInRegion(b[i], r, s0, e0)) { lo = std::fmin(lo, s0); hi = std::fmax(hi, e0); CHECK(s0 >= b[i].startKhz && e0 <= b[i].endKhz, "%s region %d inside the hull", b[i].name, r); } }
        CHECK(lo == b[i].startKhz && hi == b[i].endKhz, "%s hull is the widest region range", b[i].name);
    }
    double s0, e0;
    CHECK(!swBandInRegion(b[2], 2, s0, e0), "75 m not broadcast in region 2");
    CHECK(swBandInRegion(b[2], 1, s0, e0) && s0 == 3950 && e0 == 4000, "75 m region 1 3950-4000");
    CHECK(swBandInRegion(b[2], 3, s0, e0) && s0 == 3900 && e0 == 4000, "75 m region 3 3900-4000");
    CHECK(swBandInRegion(b[5], 2, s0, e0) && s0 == 7300 && e0 == 7400, "41 m region 2 7300-7400");
    CHECK(swBandInRegion(b[3], 1, s0, e0) && s0 == 4750 && e0 == 5060, "60 m 4750-5060");

    // each built-in plan carries its own region's table
    dect2::bandplan::Registry reg;
    for (const auto& pl : reg.plans()) {
        const int r = pl.ituRegion ? pl.ituRegion : 1;
        size_t k = 0, want = 0;
        for (size_t i = 0; i < n; i++) { if (swBandInRegion(b[i], r, s0, e0)) want++; }
        bool hasMw = false;
        for (const auto& bd : pl.bands) if (bd.service == "am" && bd.name == "MW") hasMw = true;
        if (!hasMw) continue;
        for (const auto& bd : pl.bands)
            if (bd.service == "am" && bd.name.find(" m") != std::string::npos) {
                bool ok = false;
                for (size_t i = 0; i < n; i++)
                    if (bd.name == b[i].name && swBandInRegion(b[i], r, s0, e0)) ok = std::fabs(bd.startHz - s0 * 1e3) < 1 && std::fabs(bd.endHz - e0 * 1e3) < 1;
                CHECK(ok, "%s: plan band %s matches region %d", pl.id.c_str(), bd.name.c_str(), r);
                k++;
            }
        CHECK(k == want, "%s: %zu shortwave bands, want %zu", pl.id.c_str(), k, want);
    }
    std::printf(failures ? "swbc: %d FAILED\n" : "swbc: ok\n", failures);
    return failures ? 1 : 0;
}
