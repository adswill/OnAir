// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole-span DVB-S/S2: finding the occupied transponders in the averaged spectrum of a wide capture. A pure function: a power spectrum in, a list of
// (centre, bandwidth, symbol rate) out, so it is tested with synthetic spectra. See docs/WHOLESPAN-TV.md.
//
// The noise floor is a line fitted through the quiet parts (an LNB's IF tilts by several dB across the band); every run of bins above floor + threshold
// is a candidate. A run with a valley of at least splitDb inside it is two carriers (adjacent transponders) and is split at the valley; each part must
// be wide enough and have a flat top on its own, so two narrow spurs close together are never merged into one transponder. The 0 Hz spike of an SDR
// (a few bins wide, far above the floor around it) is removed before the search.
#pragma once
#include <vector>

namespace dect2 {

struct Transponder {
    double centerHz = 0;     // relative to the centre of the capture
    double bwHz = 0;         // occupied bandwidth, Rs (1 + roll-off)
    double rsHz = 0;         // symbol rate estimate: the -3 dB width of a root raised cosine carrier
    double rollOff = 0;      // snapped to 0.35 .. 0.05 (0: not judged)
    double levelDb = -200;   // plateau (dB, the unit of the spectrum)
    double snrDb = 0;        // plateau over the floor
    bool clipped = false;    // runs into the edge of the usable span
};

struct TransponderParams {
    double thresholdDb = 5;      // a run starts this far above the floor and ends 2 dB lower
    double splitDb = 3;          // a valley this deep inside a run separates two carriers
    double minRsHz = 1e6;        // narrower carriers are not transponders (tones, beacons, spurs)
    double maxRsHz = 1e12;
    double edgeFrac = 0.46;      // the usable part of the span: +- this fraction of the rate
    double minFlatness = 0.7;    // fraction of the top part of a carrier that must lie within 3 dB of its plateau
};

// psdDb: power per bin in dB, DC in the middle (bin N/2), N bins spanning rate Hz. Sorted by frequency.
std::vector<Transponder> detectTransponders(const std::vector<float>& psdDb, double rate, const TransponderParams& p = TransponderParams());

// The nearest of the standard roll-offs 0.35 0.25 0.20 0.15 0.10 0.05 (0: absurd value)
double snapRollOff(double a);

} // namespace dect2
