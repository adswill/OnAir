// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Panorama: a wide overview (5 / 10 / 20 MHz) for a radio whose sample rate is smaller than that (RTL-SDR: 2.4 Msps). It is the sweep scanner
// (sweep.h) run on a fixed span around the tuned frequency: the radio is retuned in steps of its own sample rate, the 10 % at each edge of every
// step (filter roll-off) is dropped and the DC spike is filled in, and the steps are stitched into one trace (runSweep does that). These are the
// pure parts: the config for a span and the reduction of a trace to the columns of a strip.
#pragma once
#include "dect2/sweep/sweep.h"
#include <vector>

namespace dect2 {
namespace sweep {

// The spans offered in the app (MHz)
constexpr double kPanoramaSpansMhz[3] = {5, 10, 20};

// Sweep config for a panorama of spanHz centred on centreHz (the start is kept above 0 Hz). Bins are span / ~1000 wide; 10 % is cropped at each
// edge of a step; the dwell is short (a few FFTs per step) so the whole span refreshes about once a second on an RTL-SDR.
SweepConfig panoramaConfig(double centreHz, double spanHz);

// The trace as `cols` columns, each the strongest bin that falls in it (dBFS; -150 where no bin falls). The columns cover [startHz, startHz + bins * binHz).
std::vector<float> panoramaColumns(const Sweep& s, int cols);

// How many retunes one panorama takes at this sample rate (0 for a rate of 0)
int panoramaSteps(double spanHz, double rateHz);

} // namespace sweep
} // namespace dect2
