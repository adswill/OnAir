// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio clips: the app half (dialog, button, hotkey, settings). The pure half is clips.h.
#pragma once
#include <vector>
struct App;
namespace clipsui {
void feed(const std::vector<float>& dbfs);   // every spectrum frame: the waterfall history
void tick(App& a);              // every frame: settings, the audio ring, the hotkey C, the dialog
void button(App& a);            // the "Clip" button (the Source panel)
void open();
}
