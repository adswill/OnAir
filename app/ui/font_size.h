// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ImGui 1.92: PushFont(font, size) takes the size BEFORE the style's FontScaleDpi (the interface scale, gUi, set in main.cpp) and multiplies
// it itself; GetFontSize() is the size AFTER. A size taken from GetFontSize() or a pixel size times gUi was scaled twice: at 150 % such text
// was 1.5x too large and outgrew its row (#502). Pass fontBase() * k for "k times the current text", or the bare pixel size.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"

inline float fontBase() { return ImGui::GetCurrentContext()->FontSizeBase; }   // the current font's size before the interface scale
