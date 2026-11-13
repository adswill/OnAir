// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The command palette (Ctrl+K, Cmd+K on a Mac): a centred popup with a search box and a filtered list of actions. The list holds the
// tabs of the mode on screen, the panels of the View menu, every mode, start / stop, mute, Credits and the Help links. Each entry runs
// the same function its menu item or button uses. Arrow keys move, Enter runs, Escape closes.
#pragma once
#include "../app.h"

// Once a frame at the end of drawUI() (after the windows): opens or closes the palette on Ctrl+K and draws it while it is open.
void commandPalette(App& a);

// tabItem() reports each tab it draws (in the frame it is drawn), so that the palette can list the tabs of the mode on screen.
void paletteNoteTab(const char* name);
