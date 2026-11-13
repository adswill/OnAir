// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "My ionosphere" view of the HF beacons screen: the receptions this receiver made (FT8 / FT4 / WSPR spots with a locator, beacons and time stations
// with a known place) kept in a local file and shown as evidence: a heard grid per band and hour, a reception map with the paths coloured by SNR and the
// day / night line, a MUF-style estimate per distance (the highest band heard there), and predicted against observed open bands. Nothing is uploaded.
#pragma once
#include "app.h"
#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include <vector>

namespace ionoobs_ui {

void feedFtx(App& a, const dect2::HfdigFtxTelemetry& f);
void feedBeacons(const std::vector<dect2::hfb::Beacon>& list, const dect2::HfbTelemetry& t);
void view(App& a);

}
