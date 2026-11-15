// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #534: FT8 / FT4 / WSPR / JT / JS8 / keyboard / FreeDV / HamDRM entries are upper sideband at every dial (also 6, 4, 2 m, 70 cm, 23 cm), so
// Auto must never resolve to narrow FM for them; SSTV and RTTY keep the plain Auto (VHF SSTV is FM).
#include "dect2/audio/hf_demod.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double dials[] = {1.840, 3.573, 7.074, 14.074, 21.074, 27.265, 28.074, 50.313, 50.318, 70.154, 144.174, 144.170, 144.489, 432.065, 1296.100};
    const int masks[] = {4, 8, 16, 32};
    for (const int m : masks) {
        CHECK(hfdigSidebandOnly(m), "mask %d is sideband only", m);
        for (const double d : dials)
            CHECK(hfDemodResolveFor(HfDemodAuto, d, hfdigSidebandOnly(m)) == HfDemodUsb, "mask %d at %.3f MHz resolves to %s, not USB", m, d, hfDemodName(hfDemodResolveFor(HfDemodAuto, d, true)));
    }
    CHECK(!hfdigSidebandOnly(1) && !hfdigSidebandOnly(2) && !hfdigSidebandOnly(31), "RTTY, SSTV and the all-in-one view keep plain Auto");
    CHECK(hfDemodResolveFor(HfDemodAuto, 145.8, hfdigSidebandOnly(2)) == HfDemodNfm, "VHF SSTV Auto is NFM");
    CHECK(hfDemodResolveFor(HfDemodAuto, 14.074, false) == HfDemodUsb, "HF Auto is USB");
    CHECK(hfDemodResolveFor(HfDemodLsb, 50.313, true) == HfDemodLsb, "an explicit choice is kept");
    CHECK(hfDemodResolveFor(HfDemodNfm, 50.313, true) == HfDemodNfm, "an explicit NFM is kept");
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
