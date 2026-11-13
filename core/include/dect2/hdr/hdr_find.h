// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Finds hybrid FM HD Radio stations in a wide capture: a pair of flat digital sidebands 129 to 198 kHz either side of a channel on the 100 kHz raster, with nothing just outside them.
// Only such a channel gets an HdrReceiver (Whole span), which then proves it by locking.
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <vector>

namespace dect2 {

struct HdrSpanHit { double hz = 0; float lowerDb = 0, upperDb = 0; double lastSec = 0; };   // hz relative to the span centre; each sideband's level over the floor

class HdrFinder {
public:
    HdrFinder();
    ~HdrFinder();
    void configure(double spanRate, double centreHz = 0);
    void reset();
    void feed(const cf32* x, size_t n);
    bool takeChanged(std::vector<HdrSpanHit>& out);
    std::vector<HdrSpanHit> hits() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
