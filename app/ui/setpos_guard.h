// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "imgui.h"
#include "imgui_internal.h"

namespace ui {

// Call right before EndChild()/End() of a container whose content is drawn by many modes. When that content ends with
// SetCursorPos()/SetCursorScreenPos() and no item after it, ImGui 1.92 reports "uses SetCursorPos() to extend
// window/parent boundaries" and asserts in a Debug build (#104). A zero-size Dummy at the cursor is what ImGui asks
// for: the container then grows to the cursor, as it did before 1.89.
inline void closeCursorMove() {
    ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    if (w && w->DC.IsSetPos) ImGui::Dummy(ImVec2(0, 0));
}

} // namespace ui
