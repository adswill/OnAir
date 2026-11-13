// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sun outage predictor (3.9, docs/SMART5-sat-3.9.md item 17). Twice a year the Sun passes behind a geostationary satellite as
// seen from your dish; its noise swamps the downlink for a few minutes a day over several days. This gives those days and
// minutes from your position, the satellite's longitude and the dish's beamwidth (70 lambda / D), so a dropout is not taken for
// a fault. The outage is counted while the Sun's centre is within half the beamwidth plus the Sun's radius (0.27 deg).
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct SunOutage { double start = 0, end = 0, peak = 0, minSepDeg = 0; };

double dishBeamwidthDeg(double diameterM, double freqHz);   // half-power beamwidth, 70 lambda / D
// angle between the Sun and the satellite seen from the site (degrees)
double sunSatSeparation(double unixSec, double latDeg, double lonDeg, double satLonDeg);
// every outage between t0 and t1 (unix seconds); beamwidthDeg is the full half-power width
std::vector<SunOutage> sunOutages(double t0, double t1, double latDeg, double lonDeg, double satLonDeg, double beamwidthDeg);
// true while an outage is in progress (for the decoders' dropout reason)
bool inSunOutage(double unixSec, double latDeg, double lonDeg, double satLonDeg, double beamwidthDeg);
std::string sunOutageText(const SunOutage& o);

} // namespace smart5
} // namespace dect2
