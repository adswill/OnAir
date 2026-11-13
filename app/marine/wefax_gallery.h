// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax autosave and gallery (the rules are in core/include/dect2/marine/wefax_archive.h).
#pragma once
#include "dect2/marine/marine_fax.h"
#include <string>

struct App;

namespace wefaxgal {

struct Capture {
    dect2::FaxImage im;
    double dialKhz = 0;
    int ioc = 576, lpm = 120;
    long long startUtc = 0, stopUtc = 0;
    double snrDb = 0;
    int phasingLines = 0;
    const char* reason = "stop tone";   // "stop tone", "timeout", "new start", "manual"
};

// Saves a finished chart (PNG + JSON side-car) when autosave is on, then trims the folder to the disk budget. Returns a short message
// ("" when autosave is off or the picture is too small).
std::string autosave(const Capture& c);

// The three settings (autosave on/off, folder, disk budget) as one block.
void settingsBlock();

// The gallery section: thumbnail grid, sort, search, large view with zoom and pan, star, delete with undo, export.
void galleryView(App& a);

} // namespace wefaxgal
