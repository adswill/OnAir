// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The one place the look of the interface is defined: spacing scale, row heights, radii, border widths, font sizes and the colour roles.
// The theme presets (Classic, Dev A, Dev B in ui2_theme.cpp) fill the colour roles in syncTokens(); every kit component (kit.h) reads only
// these, scaled for HiDPI in one place (kit::px). Panels use the kit or these tokens, never a literal colour or pixel size.
#pragma once
#include "imgui.h"
#include "../scale.h"

namespace kit {

struct Tokens {
    // colour roles
    ImVec4 bg0, bg1, bg2, bg3;      // window, panel / bars, controls, hovered controls
    ImVec4 border, text, textDim;
    ImVec4 accent, ok, warn, error; // accent = selection and the active thing only; ok / warn / error carry status
    ImVec4 selection, hover;
    // geometry, unscaled points (kit::px() scales them)
    float s2 = 2, s4 = 4, s6 = 6, s8 = 8, s12 = 12, s16 = 16;   // the spacing scale
    float rowH = 19;       // table and list rows
    float controlH = 20;   // buttons, combos, tab strips
    float headerH = 20;    // panel header bars
    float radius = 0;      // corner radius (0 in the dev looks)
    float border1 = 1;     // every rule is one pixel
    bool devLook = false;  // Dev A / Dev B: the components take their flat tool-window form
    float fontSmall = 11, fontBody = 13, fontTitle = 14;   // sizes; the font itself is the one the typography setting loaded
};

Tokens& tokens();                           // the live tokens
inline float px(float v) { return v * gUi; }   // HiDPI: every size passes through here
inline ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
void syncTokens();                          // call after a preset changed ui2::T (ui2_theme.cpp does)

}  // namespace kit
