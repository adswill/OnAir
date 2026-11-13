// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whether two views of the receiver are the same multiplex: decides when the live service list must be emptied.
// A different transponder (frequency, symbol rate, family) or a different network / transport stream id means the services of the
// old one are stale. An id or rate of 0 means "not known yet" and never counts as a change.
#pragma once
#include <algorithm>
#include <cmath>

namespace dect2 {

struct MuxId {
    double freqHz = 0;       // tuned centre frequency
    double symRateHz = 0;    // symbol rate in Bd (DVB-S/S2, DVB-C); 0 = auto / not applicable
    int family = 0;          // receiver family: a switch of mode is a different multiplex
    int onid = 0, tsid = 0;  // original network id and transport stream id from the PAT / SDT; 0 = not heard yet
};

inline bool isDifferentMux(const MuxId& prev, const MuxId& cur, double freqTolHz = 100e3) {
    if (prev.family != cur.family) return true;
    if (std::fabs(prev.freqHz - cur.freqHz) > freqTolHz) return true;
    if (prev.symRateHz > 0 && cur.symRateHz > 0 && std::fabs(prev.symRateHz - cur.symRateHz) > std::max(1e3, 0.005 * prev.symRateHz)) return true;
    if (prev.onid != 0 && cur.onid != 0 && prev.onid != cur.onid) return true;
    if (prev.tsid != 0 && cur.tsid != 0 && prev.tsid != cur.tsid) return true;
    return false;
}

}   // namespace dect2
