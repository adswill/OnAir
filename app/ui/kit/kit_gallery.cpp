// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A page with every kit component, for a hidden screenshot per look: `dect2 --kitgallery --hidden --shot x.png --theme 1|3|4` (docs/UI-KIT.md).
#include "kit.h"
#include "../../app.h"

namespace kit {

void gallery(App& a, ImVec2 disp) {
    (void)a;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("##kitgallery", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const Tokens& k = tokens();
    static bool open1 = true, tg = true, tg2 = false, ck = true, swv = true;
    static float sl = 42.f; static int cb = 1;
    static const char* items[3] = {"Auto", "Manual", "Off"};
    static const char* heads[3] = {"Frequency", "Station", "SNR"};
    panelHeader("Panel header (click folds)", &open1, "right text");
    if (open1) {
        sectionHeader("Section header");
        beginFlow();
        flowItem(80); button("Button");
        flowItem(80); button("Primary", true);
        flowItem(90); toggleButton("Toggle on", &tg);
        flowItem(90); toggleButton("Toggle off", &tg2);
        flowItem(90); checkbox("Checkbox", &ck);
        flowItem(90); sw("Switch", &swv);
        flowItem(40); iconButton("##i1", Ic::Play, "Icon button", false);
        flowItem(40); iconButton("##i2", Ic::Record, "Active icon button", true);
        endFlow();
        sectionHeader("Slider, combo, meter");
        sliderValue("##sl", &sl, 0, 100, "%.0f dB", px(220));
        ImGui::SameLine(); combo("##cb", &cb, items, 3, px(140));
        meter(0.3f, px(200)); ImGui::SameLine(); meter(0.85f, px(200)); ImGui::SameLine(); meter(0.97f, px(200));
        labelValue("SNR", "%.1f dB", 24.3f);
    }
    sectionHeader("Wrapping row (50 items)");
    beginFlow();
    for (int i = 0; i < 50; i++) { char b[24]; snprintf(b, sizeof b, "item %d", i); flowItem(px(60)); button(b); }
    endFlow();
    sectionHeader("Table");
    ImGui::BeginChild("##gt", ImVec2(0, px(100)));
    if (beginTable("##t", 3, heads)) {
        for (int r = 0; r < 6; r++) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%.3f", 88.0 + r * 0.8); ImGui::TableNextColumn(); ImGui::Text("Station %d", r); ImGui::TableNextColumn(); ImGui::Text("%d dB", 20 + r * 3); }
        endTable();
    }
    ImGui::EndChild();
    sectionHeader("Tabs");
    if (beginTabs("##gtabs")) {
        if (tab("Overview")) { ImGui::TextDisabled("tab one"); endTab(); }
        if (tab("Receiver")) { ImGui::TextDisabled("tab two"); endTab(); }
        if (tab("History")) { ImGui::TextDisabled("tab three"); endTab(); }
        endTabs();
    }
    (void)k;
    ImGui::End();
}

}  // namespace kit
