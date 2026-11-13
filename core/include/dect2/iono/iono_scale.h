// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Scaling of ionograms: the echo traces (group range against frequency) and the standard parameters they give.
//
// Traces: in each swept 50 kHz column the delay peaks well over the column's noise are taken (a column lit at most delays is an
// interfering carrier and is dropped); peaks of neighbouring columns at close delays are linked into traces, short runs are noise.
// Vertical incidence (a local ionosonde, virtual height h' = group range / 2): echoes near 0 km are the ground wave; a trace at twice the
// height of another is a multiple reflection; E (h' 85 - 160 km) and F traces are told apart by height, the O and X traces of F2 by the
// end frequencies (fxF2 - foF2 about fH / 2, 0.3 - 1.0 MHz). foE, foF2 are the ends of the O traces (URSI: the asymptotes), foF1 a cusp
// or a break of the F trace, h'E and h'F the lowest heights, hmF2 = h'(0.834 foF2) (parabolic layer), M(3000)F2 = 1490 / (hmF2 + 176)
// (Shimazaki) and MUF(3000)F2 = foF2 x M(3000)F2. A trace that runs to the edge of the swept data gives a lower bound only.
// Oblique incidence (a distant sounder): the ground distance from the known sounder's location and the receiver's, else from the lowest
// group path (the lowest trace taken as 1E when flat, else 1F; a mirror over a spherical earth); each trace gets the fewest hops whose
// mirror height at that distance is E (85 - 150 km) or F (150 - 500 km), F ruled out below the E MUF of as many hops; the MUF of a mode is the nose of its trace (where the low and high rays meet), the path MUF the highest of them.
#pragma once
#include "iono_dsp.h"
#include "iono_tel.h"
#include <string>
#include <vector>

namespace dect2 {
namespace iono {

constexpr double kEarthKm = 6371.0;

// A chirp sounder known by its sweep rate, schedule (period and start offset in it, 0 = not known) and location. verified = false: the
// rate, schedule or location are from second-hand reports and may be wrong (the schedule then only adds weight, it never rules out).
struct KnownSounder {
    const char* name;
    double rateHz, periodSec, offsetSec;
    double lat, lon;
    bool verified;
    const char* note;
};
const std::vector<KnownSounder>& knownSounders();

double greatCircleKm(double lat1, double lon1, double lat2, double lon2);
// group path (km) of an n-hop mode over ground distance d, mirror at virtual height h, spherical earth
double obliqueGroupPathKm(double distKm, double hKm, int hops);
// the mirror height of an n-hop group path over ground distance d (NAN if impossible)
double mirrorHeightKm(double groupKm, double distKm, int hops);
// the ground distance for a group path (inverse of the above), NAN if the path is shorter than 2 n h
double obliqueDistanceKm(double groupKm, double hKm, int hops);

struct Observer { bool have = false; double lat = 0, lon = 0; };

// The known sounder this one is (index, -1 none); match: 1 rate only (with the receiver location: the nearest whose distance agrees with
// distHintKm when that is known), 2 rate and schedule. candidates: listed sounders of this rate.
int identify(const IonoSounder& s, const Observer& obs, double distHintKm, int& match, int& candidates,
             const std::vector<KnownSounder>& list = knownSounders());

// The traces of an ionogram (kRows x kCols dB over each column's median, NAN where not swept), group range in km
std::vector<IonoTrace> extractTraces(const std::vector<float>& db);

struct ScaleOptions {
    int geometry = 0;          // 0 auto (the known sounder's distance, else the height of the lowest trace), 1 vertical, 2 oblique
    bool locked = true;        // absolute delays (else nothing but the trace shapes is meaningful: only frequencies are scaled)
    Observer obs;
};
// Scale an ionogram of sounder s; traces gets the labelled traces used
IonoScaling scaleIonogram(const std::vector<float>& db, const IonoSounder& s, const ScaleOptions& o, std::vector<IonoTrace>* traces = nullptr,
                          const std::vector<KnownSounder>& list = knownSounders());

// HF bands for "bands open now"
struct HfBand { const char* name; double loMhz, hiMhz; bool broadcast; };
const std::vector<HfBand>& hfBands();
// 1 open (below the MUF and above the lowest echo), 0 closed (above the MUF), -1 absorbed (below the lowest echo), -2 unknown
int bandState(const HfBand& b, double mufMhz, double fminMhz);
// The usable MUF of a sounder's scaling: oblique the path MUF, vertical MUF(3000)F2 (NAN none)
double scalingMuf(const IonoScaling& s);

// History as CSV (UTC time, geometry, the values), one line per point
std::string historyCsv(const std::vector<IonoHistPt>& h, const IonoSounder& s, const char* knownName);

} // namespace iono
} // namespace dect2
