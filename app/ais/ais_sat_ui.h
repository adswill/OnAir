// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS: the channel plan controls and the Satellite AIS panel (ais_sat_ui.cpp), called from ais_ui.cpp.
#pragma once
struct App;
void aisChannelControls(App& a, bool& retune);   // the tuner panel: which channels the capture holds, presets, the hop plan, satellite mode
void aisSatTick(App& a);                         // every frame while AIS is selected
void aisSatPanel(App& a);                        // the Satellite view of the AIS tab
