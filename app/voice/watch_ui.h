// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The watch list of the digital voice modes (core/include/dect2/voice/watch.h): follow talkgroups, radio IDs and callsigns across DMR, P25, NXDN,
// TETRA, dPMR, D-STAR, Yaesu and M17. A "Watch" view in each mode (watchView) and the alerts (tick, called every frame by the modes).
#pragma once
#include "dect2/voice/watch.h"
#include <functional>

struct App;

namespace watchui {

// every frame: loads the saved list once, keeps the record rules in step, raises the alerts of calls that started (notification, optional beeps)
void tick(App& a);
// the Watch view of `mode` (a monitor name: "dmr", "p25", "nxdn", "tetra"): the list, the add row, and the history of what was heard.
// `extra` (optional) draws more under an entry (DMR: the last GPS position and a button for the map).
void watchView(App& a, const char* mode, const std::function<void(const dect2::voice::WatchEntry&)>& extra = nullptr);

}
