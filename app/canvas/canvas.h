// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Workspace layout (View > Layout > Workspace, docs/WORKSPACE-4.0.md): a blank canvas the user fills with blocks (radio sources,
// spectrum and waterfall, demodulators, audio, decoders, the map, the satellite tracker, the rotator, the transmitter ...), each a
// movable, resizable window drawn with the panels of the other layouts. The model is canvas_model.h; this is the entry for the shell.
#pragma once
#include "../app.h"
#include <string>

namespace canvas {

bool on();                    // the Workspace layout is chosen (saved; off by default)
void setOn(bool v);
bool active(const App& a);    // chosen and usable: the new interface, not the touch interface
// the canvas and its blocks, instead of the body of the interface: from y = top to the bottom of disp (ui2.cpp, after the menu bar)
void frame(App& a, ImVec2 disp, float top);
void layoutMenuItem(App& a);  // the "Workspace" entry of View > Layout
void devStart(const std::string& arg);   // --workspace <0..3 | file>: start in the Workspace layout on a template or a workspace file

}  // namespace canvas
