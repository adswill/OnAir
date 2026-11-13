// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The sea distress incident cards (core/include/dect2/marine/marine_incident.h): drawn in the Marine DSC view and the AIS Alerts view.
#pragma once
struct App;
void incidentBoardUi(App& a, bool compact = false);
