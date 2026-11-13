// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The beginner's two buttons in the Simple view (docs/SCAN-IDENTIFY.md): "Scan" finds every signal in what the radio sees, says what each one is
// and opens the right mode in one click; "Identify" is for "I don't know what this is": it runs the identifier on the strongest signal around the
// tuned frequency, or on the region dragged on the spectrum, and lists ranked guesses with a confidence. Both use the signal identifier's own
// card (specx_ident.cpp: ranked table, Open, Add, Why?), so there is one set of detectors. Hand-set choices stay in Advanced.
#include "../app.h"

void scanIdentifyBar(App& a) {
    const bool running = a.engine.running() && a.engine.sampleRate() > 0;
    ImGui::PushID("##scanbar");
    if (!running) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.70f));
    if (ImGui::Button(tr("Scan"), ImVec2(110 * gUi, 0))) identifyAll(a);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Look for every signal in what the radio sees, find out what each one is and offer to open it. One click."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Identify"))) {
        double lo = 0, hi = 0;
        const double rc = radioCenterMhz(a) * 1e6;
        if (identifyRegionMhz(lo, hi) && hi > lo) identifyRegion(a, lo, hi);
        else identifyStart(a, rc, 0, 0, rc);   // no region: the strongest signal in the capture
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("I don't know what this is: measure the strongest signal (or the region you dragged on the spectrum) and show the most likely modes with how sure OnAir is."));
    if (!running) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Start the receiver to scan."));
    }
    ImGui::PopID();
    ImGui::Spacing();
}
