// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Brightness and contrast sliders for a video view (analog TV, FPV; issue #239): kept per mode in the settings, applied to the copy of the
// frame that goes to the screen (dect2/video/display_adjust.h), with a Reset button. The decode is not changed.
#pragma once
#include "dect2/video/display_adjust.h"
#include "imgui.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/scale.h"
#include <string>

namespace vadj {

struct Control {
    dect2::video::DisplayAdjust adj;
    bool loaded = false;

    void load(const char* key) {   // key: "atv", "fpv": the preferences <key>Bright and <key>Contrast
        if (loaded) return;
        loaded = true;
        adj.brightness = (float)plat::prefs().getD((std::string(key) + "Bright").c_str(), 0.0);
        adj.contrast = (float)plat::prefs().getD((std::string(key) + "Contrast").c_str(), 1.0);
        adj.clamp();
    }
    void save(const char* key) const {
        plat::prefs().setD((std::string(key) + "Bright").c_str(), adj.brightness);
        plat::prefs().setD((std::string(key) + "Contrast").c_str(), adj.contrast);
    }
    // Two sliders and Reset; true when something changed (the caller draws the picture again)
    bool controls(const char* key, float width) {
        load(key);
        bool ch = false;
        const std::string k = key;
        ImGui::TextDisabled("%s", tr("Brightness")); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(width);
        ch |= ImGui::SliderFloat(("##" + k + "bright").c_str(), &adj.brightness, -1.f, 1.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lighter or darker picture on the screen. The decoding and saved pictures are not changed."));
        ImGui::TextDisabled("%s", tr("Contrast")); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(width);
        ch |= ImGui::SliderFloat(("##" + k + "contrast").c_str(), &adj.contrast, 0.f, 3.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("More or less difference between light and dark on the screen. 1 is as decoded."));
        if (!adj.identity()) {
            ImGui::SameLine(0, 8 * gUi);
            if (ImGui::SmallButton((std::string(tr("Reset")) + "##" + k + "adjreset").c_str())) { adj = dect2::video::DisplayAdjust(); ch = true; }
        }
        if (ch) { adj.clamp(); save(key); }
        return ch;
    }
};

}
