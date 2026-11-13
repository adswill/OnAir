// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Interface scale for displays where the window is measured in physical pixels (Windows, Linux with scaling): 1 = 96 dpi.
// Every hard-coded pixel size in the UI is multiplied by it; the fonts and the ImGui style are scaled with it as well.
// (On macOS the window is measured in points and the Retina scaling is handled by the framebuffer, so it stays 1.)
#pragma once
inline float gUi = 1.f;
inline int gStyleEpoch = 0;   // bumped when main.cpp rebuilds the ImGui style from the scale-1 base (a new scale, the touch interface on or off): ui2_theme.cpp then re-applies the theme
