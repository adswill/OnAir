// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF propagation dashboard (the "Propagation" view of the HF beacons screen): the model of core/include/dect2/hfbeacon/hfbeacon_prop.h fed by the
// beacon monitor and by the FT8 / FT4 / FT2 / WSPR decodes of the HF digital receiver. Own receiver only: nothing is downloaded.
#pragma once
#include "app.h"
#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/hfbeacon/hfbeacon_prop.h"
#include <vector>

namespace hfprop_ui {

dect2::hfp::Model& model();
// New detections of the beacon monitor (call every tick while it is running)
void feedBeacons(const std::vector<dect2::hfb::Beacon>& list, const dect2::HfbTelemetry& t);
// New decodes of the HF digital receiver (call every tick while it is running)
void feedFtx(App& a, const dect2::HfdigFtxTelemetry& f);
void view(App& a);

} // namespace hfprop_ui
