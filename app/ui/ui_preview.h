// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The hover preview of the appearance choices (docs/UI-PREVIEW-4.0.md). Call uiprev::item() right after a menu item, combo row, radio
// button or checkbox that picks a look: while that item is hovered, or has the keyboard focus, a tooltip shows a live miniature of the
// interface with that choice (a window with its title bar, side panel, tabs, spectrum, waterfall, a button and text lines) drawn with
// ImGui draw lists in the choice's colours, spacing, font size and layout. Nothing is changed until the item is picked.
// The pure half (option -> description) is ui_preview_logic.h.
#pragma once
#include "ui_preview_logic.h"

struct App;
struct ImDrawList;

namespace uiprev {

bool& enabled();                // Settings > Appearance > Previews of the looks (on unless turned off; saved as "uiPreviews")
Look lookOf(const App& a);      // the settings in use now

// the tooltip for value v of option o, when the last item is hovered or keyboard-focused; `note` is a line under the picture
void item(const App& a, Opt o, double v, const char* note = nullptr);

// the miniature of `d` in the rectangle (x, y, w, h). `unit` is pixels per pixel of the real interface at scale 1 (the shrink factor
// times the display scale); `turned`: this frame's draw lists are turned over by the Light pass afterwards (frameTurned of the look in use)
void draw(ImDrawList* dl, float x, float y, float w, float h, const Desc& d, float unit, bool turned);

// dev: `dect2 --previewgallery` shows a page with every picture of kGallery instead of the main window (for a hidden screenshot)
bool& galleryOn();
void gallery(const App& a, float dispW, float dispH);

}   // namespace uiprev

namespace ui2 {
bool userAccent(float rgb[3]);  // ui2_theme.cpp: true and the colour when Own accent colour is on
}
