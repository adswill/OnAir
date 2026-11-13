// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Recent voice clips": the list of WAV clips a voice mode saved, with Play / Stop inside OnAir, star, save as, delete, open folder and
// automatic retention (core/include/dect2/voice/clip_history.h). One component for every voice mode that saves calls as WAV. docs/VOICE-CLIPS.md.
#pragma once
#include "dect2/voice/clip_history.h"
#include <string>
#include <vector>

struct App;

namespace voiceclips {

// The history of a mode ("iridium", "dmr", ...), created on first use; the shared retention settings and the stars are loaded from the
// app settings. Prefer panel().
dect2::voice::ClipHistory& history(const std::string& mode);

// Draws the list. `folder` is where the mode saves clips (scanned, so clips of earlier runs show too, and the folder stays within the limits);
// `prefix` limits the scan to file names starting with it; `fresh` are clips with richer details (time, frequency / talkgroup / ID) the mode
// knows about its own calls. Call every frame inside the mode's panel; `id` must be unique on the screen.
void panel(App& a, const char* mode, const char* id, const std::string& folder, const std::string& prefix, const std::vector<dect2::voice::Clip>& fresh);

// Stops a clip that is playing (a mode calls it when its tab goes away).
void stopPlayback();

} // namespace voiceclips
