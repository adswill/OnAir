// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specan/lnb_cal.h"
#include <cmath>

namespace dect2 {

LnbError lnbErrorFromBeacon(double rfHz, double loNominalHz, double measuredIfHz, bool inverted) {
    // Non-inverted: IF = RF - LO, so the expected IF is RF - nominal and the actual LO is RF - IF.
    // Inverted: IF = LO - RF, so the expected IF is nominal - RF and the actual LO is RF + IF.
    LnbError e;
    if (inverted) e.hz = measuredIfHz - (loNominalHz - rfHz);
    else          e.hz = (rfHz - loNominalHz) - measuredIfHz;
    e.ppm = loNominalHz != 0.0 ? e.hz / loNominalHz * 1e6 : 0.0;
    return e;
}

double lnbCorrectedLoHz(double loNominalHz, double ppm) {
    return loNominalHz * (1.0 + ppm * 1e-6);
}

LnbDriftTracker::LnbDriftTracker(double tauSec) : tau_(tauSec > 0 ? tauSec : 1800.0) {}

void LnbDriftTracker::reset() {
    n_ = 0;
    t_ = ppm_ = rate_ = 0;
}

void LnbDriftTracker::add(double tSec, double ppm) {
    if (!std::isfinite(tSec) || !std::isfinite(ppm)) return;
    if (n_ == 0) {
        t_ = tSec;
        ppm_ = ppm;
        rate_ = 0;
        n_ = 1;
        return;
    }
    const double dt = tSec - t_;
    if (dt <= 0) return;
    const double a = 1.0 - std::exp(-dt / tau_);
    const double prev = ppm_;
    ppm_ += a * (ppm - ppm_);
    const double instantPerHour = (ppm_ - prev) / dt * 3600.0;
    rate_ += a * (instantPerHour - rate_);
    t_ = tSec;
    n_++;
}

} // namespace dect2
