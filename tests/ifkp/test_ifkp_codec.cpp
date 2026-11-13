// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IFKP against FSQ: what differs (idle, space, line feed) and that the two modes cannot be mixed up by their tone spacing.
#include "dect2/fsq/kbd33.h"
#include "dect2/fsq/kbd33_station.h"
#include "dect2/ifkp/ifkp_rx.h"
#include <cmath>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ifkpTuning();
    CHECK(mt.stdMode == 118 && modeTuningById("ifkp") && modeTuningById("ifkp")->stdMode == 118, "tuning table");
    CHECK(modeTuningById("fsq") && modeTuningById("fsq")->stdMode == 115, "FSQ tuning table");
    int s2;
    CHECK(kbd33::idleSymbol(true, s2) == 0 && s2 == -1, "IFKP idle is one symbol 0");
    CHECK(kbd33::idleSymbol(false, s2) == 28 && s2 == 30, "FSQ idle is 28 then 30");
    // the idle character is not text in either mode
    for (bool ifkp : {false, true}) {
        kbd33::CharDecoder d(ifkp);
        int s, t;
        s = kbd33::idleSymbol(ifkp, t);
        d.push(s);
        int c = t >= 0 ? d.push(t) : d.push(0);
        CHECK(c == 0, "idle gave %d", c);
    }
    // the same symbols are different characters
    {
        kbd33::CharDecoder f(false), i(true);
        f.push(28); i.push(28);
        CHECK(f.push(0) == '\n' && i.push(0) == ' ', "28 is a line feed in FSQ and a space in IFKP");
    }
    CHECK(std::fabs(kbd33::ifkpSpec(1).spacingHz / kbd33::fsqSpec(2).spacingHz - 4.0 / 3.0) < 1e-9, "spacing ratio");
    int n = 0;
    const IfkpPreset* p = ifkpPresets(n);
    CHECK(n >= 4 && p[0].mhz > 1 && p[0].mhz < 30, "presets");
    return testSummary("ifkp codec");
}
