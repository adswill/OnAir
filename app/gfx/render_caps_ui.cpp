// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The windows of render_caps.h that belong to OnAir's interface: the software-OpenGL hint and View > Low-power drawing. The rest of
// render_caps.cpp is in the shared interface library (onair_uikit), also used by onair-videogen.
#include "render_caps.h"
#include "app.h"

namespace rcaps {
int savedChoice();
int envForce();

void hintWindow(App&) {
    if (!showHint(software(), savedChoice(), envForce())) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(vp->WorkSize.x - 16 * gUi, 460 * gUi);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8 * gUi, vp->WorkPos.y + vp->WorkSize.y - 40 * gUi), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (!ImGui::Begin("##swrenderhint", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) { ImGui::End(); return; }
    ImGui::TextUnformatted(tr("Performance"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(tr("OpenGL runs in software here (%s): the processor draws every frame, beside the receiver."), rendererName());
    ImGui::TextUnformatted(tr("Low-power drawing lowers the frame rate and simplifies the aircraft map (no trails, labels only for the selected aircraft). "
                              "Installing the graphics driver for your GPU is the real fix."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(tr("Use low-power drawing"))) setLowPowerChoice(On);
    ImGui::SameLine();
    if (ImGui::Button(tr("Keep full drawing"))) setLowPowerChoice(Off);
    ImGui::TextDisabled("%s", tr("View > Low-power drawing changes it later."));
    ImGui::End();
}

void viewMenuItem() {
    bool on = lowPowerDraw();
    ImGui::BeginDisabled(envForce() >= 0);
    if (ImGui::MenuItem(tr("Low-power drawing"), nullptr, &on)) setLowPowerChoice(on ? On : Off);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", envForce() >= 0 ? tr("Set by ONAIR_LOW_POWER_DRAW or --low-power-draw for this run.")
                                             : tr("A lower frame rate cap and a simpler aircraft map (no trails). For computers without a graphics driver (software OpenGL)."));
}

}   // namespace rcaps
