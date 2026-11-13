// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The transponders the network announces about itself (the NIT of the stream received), as rows to tune. Pure, no I/O.
#pragma once
#include "dect2/tv/si_tables.h"
#include <vector>

namespace dect2::ease {

struct NitRow {
    double mhz = 0;          // as announced (for a satellite: the downlink)
    double tuneMhz = 0;      // what to tune the radio to; 0 when it cannot be derived (a satellite list without the transponder being received)
    int pol = -1, modulation = -1, fec = -1, services = 0, kind = 0;
    double symbolRateK = 0;
    bool s2 = false, current = false;
};
// kind: 1 satellite (also S2X rows), 2 cable. A satellite's frequencies are mapped through the LNB by comparing the announced frequency of the
// current transponder (matched by transport stream id) with what the radio is tuned to; inverted is a high-side oscillator.
std::vector<NitRow> nitRows(const SiSnapshot& si, int kind, int ownTsid, double ownTuneMhz, bool inverted);

} // namespace dect2::ease
