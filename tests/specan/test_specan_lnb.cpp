// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LNB calibration from a beacon (synthetic offsets) and the drift tracker (synthetic linear drift).
#include "dect2/specan/lnb_cal.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rf = 10489.750e6;

    // Standard LNB, nominal 9750 MHz, real oscillator +20 ppm (+195 kHz): the beacon IF is 195 kHz low.
    const double nomStd = 9750e6;
    const double ifStd = (rf - nomStd) - 195000.0;
    LnbError a = lnbErrorFromBeacon(rf, nomStd, ifStd, false);
    CHECK(std::fabs(a.hz - 195000.0) < 0.5, "std LNB hz %.3f, expected 195000", a.hz);
    CHECK(std::fabs(a.ppm - 20.0) < 1e-6, "std LNB ppm %.6f, expected 20", a.ppm);

    // Perfect LNB: no error.
    LnbError z = lnbErrorFromBeacon(rf, nomStd, rf - nomStd, false);
    CHECK(std::fabs(z.hz) < 1e-6 && std::fabs(z.ppm) < 1e-9, "ideal LNB gives %.3f Hz", z.hz);

    // Inverted LNB (LO 10600 MHz above the downlink), real oscillator -10 ppm (-106 kHz): IF = LO - RF.
    const double nomInv = 10600e6;
    const double ifInv = (nomInv - 106000.0) - rf;
    LnbError b = lnbErrorFromBeacon(rf, nomInv, ifInv, true);
    CHECK(std::fabs(b.hz + 106000.0) < 0.5, "inverted LNB hz %.3f, expected -106000", b.hz);
    CHECK(std::fabs(b.ppm + 10.0) < 1e-6, "inverted LNB ppm %.6f, expected -10", b.ppm);

    // Correction: the tuner LO set to the corrected value puts the beacon IF back at its nominal place.
    const double corrLo = lnbCorrectedLoHz(nomStd, a.ppm);
    CHECK(std::fabs(corrLo - (nomStd + 195000.0)) < 1.0, "corrected LO %.1f Hz", corrLo);
    CHECK(std::fabs(lnbCorrectedLoHz(nomStd, 0.0) - nomStd) < 1e-6, "zero ppm keeps the LO");

    // Drift tracker: ppm = 3 + 0.05 ppm/h, sampled every 10 s for 4 h, with a 30 min time constant.
    LnbDriftTracker t(1800.0);
    CHECK(!t.valid(), "new tracker is empty");
    t.add(0.0, 3.0);
    CHECK(t.valid() && t.samples() == 1, "one sample is valid");
    t.add(-5.0, 99.0);   // out of order: ignored
    CHECK(t.samples() == 1, "earlier sample ignored");
    for (int k = 1; k <= 4 * 360; k++) {
        const double sec = 10.0 * k;
        t.add(sec, 3.0 + 0.05 * sec / 3600.0);
    }
    CHECK(std::fabs(t.ppmPerHour() - 0.05) < 0.002, "drift %.4f ppm/h, expected 0.05", t.ppmPerHour());
    const double trueNow = 3.0 + 0.05 * 14400.0 / 3600.0;
    CHECK(std::fabs(t.ppm() - trueNow) < 0.05, "filtered ppm %.4f, true %.4f", t.ppm(), trueNow);

    // A constant offset gives no drift.
    LnbDriftTracker s(600.0);
    for (int k = 0; k < 500; k++) s.add(k * 5.0, -2.0);
    CHECK(std::fabs(s.ppmPerHour()) < 1e-6 && std::fabs(s.ppm() + 2.0) < 1e-6, "constant offset: drift %.9f, ppm %.6f", s.ppmPerHour(), s.ppm());

    return testSummary("specan_lnb");
}
