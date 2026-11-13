// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ILS channels (ICAO Annex 10 Volume I, 3.1.6.1): the 40 localizer channels (108.10 - 111.95 MHz, odd tenths, 50 kHz steps) and the
// glide path frequency paired with each (329.15 - 335.00 MHz), and the display conventions.
#pragma once

namespace dect2 {

constexpr double kIlsLocFullScaleDdm = 0.155;   // localizer: full needle deflection (Annex 10: DDM 0.155 at the course sector edge)
constexpr double kIlsGsFullScaleDdm = 0.175;    // glide slope: full needle deflection (DDM 0.175 at the sector edge)
constexpr double kIlsMarkerMhz = 75.0;

bool ilsIsLocalizer(double mhz);                // one of the 40 localizer channels (within 5 kHz)
double ilsGlideslopeFor(double locMhz);         // the paired glide path frequency, 0 when not a localizer channel
double ilsLocalizerFor(double gsMhz);           // the reverse, 0 when not a glide path channel
int ilsChannelCount();
void ilsChannel(int i, double& locMhz, double& gsMhz);

} // namespace dect2
