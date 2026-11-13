// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Unattended satellite observation planning: given the passes of the satellites the user wants, the time the SDR is already
// promised to other jobs and a priority per satellite, choose which passes to take. One SDR serves one pass at a time:
// overlapping passes are settled by priority (then by the higher maximum elevation, then the earlier rise); a lower priority pass
// is trimmed to the part the SDR is free for when that part is still worth keeping.
#pragma once
#include "dect2/geo/passes.h"
#include <string>
#include <vector>

namespace dect2 {

struct SchedPass {
    int sat = 0;          // index the caller uses for the satellite
    int priority = 0;     // higher wins an overlap
    Pass pass;
};

struct BusyWindow { double start = 0, end = 0; };   // UNIX seconds: the SDR is used by another job

struct SchedParams {
    double leadSec = 20;          // the SDR is claimed this long before the slot starts (tune, start the decoder)
    double minUsefulSec = 120;    // a slot shorter than this is not worth taking
    double minElDeg = 0;          // passes whose maximum elevation is below this are not planned
};

struct SchedSlot {
    SchedPass sp;
    double start = 0, end = 0;   // the part of the pass that is taken (start >= AOS, end <= LOS)
    bool trimmed = false;
};

enum class DropReason { Conflict, Busy, TooShort, LowElevation };

struct SchedDropped { SchedPass sp; DropReason why = DropReason::Conflict; };

struct SchedPlan {
    std::vector<SchedSlot> slots;        // by start time, no two overlapping (the lead time included)
    std::vector<SchedDropped> dropped;   // by AOS
};

// Passes that end before nowSec are ignored.
SchedPlan planPasses(const std::vector<SchedPass>& passes, const std::vector<BusyWindow>& busy, double nowSec, const SchedParams& prm = SchedParams());

// The daily windows [fromMin, toMin) (minutes after local midnight, may wrap past midnight) expanded to UNIX seconds between t0 and t1.
// utcOffsetSec is local time minus UTC.
std::vector<BusyWindow> dailyBusy(int fromMin, int toMin, double t0, double t1, double utcOffsetSec);

}
