// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small helpers that several screens repeated line for line: a UTC clock text, a "key value" row, the IQ lamp and
// "State" text of a mode's status line, the zoom buttons of a mode map, and a tooltip with wrapped text.
// Used so far by the wifi, airband, dstar and orbcomm screens and the top bar / mesh code.
#pragma once
#include "../app.h"
#include "../adsb/adsb_map.h"

// "HH:MM:SS" in UTC for an epoch time, "-" when it is not set (<= 0)
std::string clockText(int64_t t);

// a dim key and its value in the mono font at the value column (wraps in a narrow pane); the value is a printf format
void kvRow(const App& a, const char* k, const char* fmt, ...);

// The IQ lamp that opens every mode's status line: grey when stopped, green for a good ADC level, amber when high or low,
// red on overload. Call it right after the StatusPanel is made.
void statusIqLamp(const App& a);

// The end of a status line: a separator, then "State" and the mode's summary in the mono font ("starting" / "stopped" while the
// mode is not live). summaryText is called only when the mode is live.
void statusStateLabel(const App& a, bool live, const std::string& summaryText);
template <class F> void statusState(const App& a, bool live, F summaryText) { statusStateLabel(a, live, live ? std::string(summaryText()) : std::string()); }

// The map's corner buttons: zoom in, zoom out, "fit" (follow the data again), then the tile controls (their note says what is never sent).
void mapCornerButtons(adsbmap::View& map, bool& follow, const char* tileNote);

// A tooltip of wrapped text, width in pixels (already scaled by gUi), for the hovered item
void wrapTooltip(float width, const char* text);
