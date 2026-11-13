// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The voice monitor on screen (dect2/voice/voice_monitor.h), the same in every digital voice mode: a strip at the top of the mode's tab
// with the state (locked, codec, encrypted, hold, recording), the big "now talking" card with its error meter and quick buttons, and
// the Voice monitor window (Help menu or the strip's button) with the calls, the history search, the filters and the sound settings.
#pragma once

struct App;

namespace voiceui {

// What the mode knows that the monitor does not
struct Link {
    bool live = false;           // the receiver of this mode is running
    bool locked = false;         // a signal of the mode is being decoded
    bool codecReady = false;     // the voice codec is loaded (for codec2 modes: always)
    const char* codecName = "";  // "AMBE+2", "IMBE", "TETRA ACELP", "Codec 2"
    const char* codecId = nullptr;   // the plug-in id ("ambe2", "ambe", "imbe", "ysf", "tetra"): an Install codec button while missing (null: bundled)
    float errorRate = -1.f;      // the channel's bit error rate 0..1 (-1: not known)
};

// mode: the monitor's name ("dmr", "p25", "nxdn", "dpmr", "dstar", "ysf", "tetra", "m17"); title: what the user sees ("DMR")
void strip(App& a, const char* mode, const char* title, const Link& l);
// The Voice monitor window; call every frame from the shell (draws nothing while closed)
void window(App& a);
void open(const char* mode = nullptr);   // opens the window (on a mode; null: the last one)
// The settings of every mode: loaded once from the data folder, saved there on every change
void load();
void save();

} // namespace voiceui
