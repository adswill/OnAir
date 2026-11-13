// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Journaline page viewer (ETSI TS 102 979): breadcrumb, menu / text / list pages, back and home, a progress bar of the objects received.
// Shared by the DAB Journaline tab and the DRM data panel.
#pragma once
#include "dect2/dab/dab_journaline.h"
#include <cstdint>
#include <vector>

struct JlViewState {
    std::vector<uint16_t> path;     // object IDs from the entry point to the page shown
    int entry = 0;                  // the point of entry the path starts with
    void reset(int e) { path.clear(); entry = e < 0 ? 0 : e; path.push_back((uint16_t)entry); }
};
// Draws the viewer. `id` makes the ImGui ids unique. entry: the point of entry object (-1: the root 0x0000).
void journalineView(const char* id, dect2::JournalineDecoder& jl, JlViewState& st, int entry);
