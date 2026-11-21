// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General / Auto labels on the spectrum: when a click on the plot counts as a click on a label (#595). The label buttons are drawn by hand over
// the plot, so the click must belong to the plot: the press began on the plot itself (not on a combo list, a menu or a floating panel that
// sits over it, such as the Tuning step list or the Auto notch switch) and the release is over the plot with nothing in front of it. Without the
// press check, picking an entry of a list drawn over a "Decode" button released the mouse on that button and the app opened the decoder
// (Weather Fax at 7.793 MHz) by itself.
#pragma once

namespace dect2 {
namespace genauto {

struct ClickGate {
    bool pressOnPlot = false;
    // every frame: mousePressed = the left button went down this frame; plotHovered = the mouse is over the plot and nothing covers it
    void frame(bool mousePressed, bool plotHovered) { if (mousePressed) pressOnPlot = plotHovered; }
    // the frame the left button is released: true when this release is a click of the plot
    bool accept(bool released, bool plotHovered) { const bool ok = released && plotHovered && pressOnPlot; if (released) pressOnPlot = false; return ok; }
};

} // namespace genauto
} // namespace dect2
