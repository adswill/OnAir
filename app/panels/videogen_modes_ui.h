// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Digital video generator, tab "Digital Television Mode Selector": which DVB-T, DVB-T2, DVB-S, DVB-S2 and DVB-S2X modes carry a
// bitrate in the bandwidth on the spectrum at the C/N seen on the spectrum analyser, and the most optimal one. The modes and the fit are
// core/src/tv/dvb_modes.cpp; this file is the tab.
#pragma once

// Draws the tab. generatorKbps: the mux rate of the generator's settings, offered as the bitrate to carry.
void dvbModeSelectorTab(int generatorKbps);
