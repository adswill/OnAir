// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Contact lines on the maps of the QSO-style modes (FT8 and the other HF digital modes, JS8Call): the options (one setting for every map),
// the "contact lines" button with its menu, picking a station by a click, and drawing the lines of a dect2::ContactGraph as great-circle
// arcs on the shared map (adsbmap). Call after adsbmap::draw(), inside the map's clip rectangle.
#pragma once
#include "imgui.h"
#include "dect2/map/contact_graph.h"

namespace contactlines {

struct Options {
    bool lines = true;          // show the lines
    bool fade = true;           // older lines fainter
    bool highlightDone = true;  // completed contacts (73 / RR73 / RRR) in green and thicker
    bool onlySel = false;       // only the lines of the selected station
    bool labels = false;        // "A>B" at the middle of a line
    int colour = 0;             // 0 by band, 1 by time
    int keepMin = 15;           // minutes a line stays after the last message
    std::string sel;            // the selected station ("" none)
};
Options& options();             // loaded from the preferences on the first call
void controls();                // the "contact lines" button and its menu (SameLine() before it)
void pickAt(const dect2::ContactGraph& g, ImVec2 click);   // select the station nearest to a click (within a few pixels), or none
// Draw the lines; `now` is the newest message time (the graph's time), `mapLon` the map's centre (lines are drawn on the world copy nearest to it).
// Returns the number of lines drawn.
size_t draw(ImDrawList* dl, const dect2::ContactGraph& g, double now, double mapLon);
// The selected station's ring; call for each station drawn at screen position q
void markSelected(ImDrawList* dl, const std::string& call, ImVec2 q);

}
