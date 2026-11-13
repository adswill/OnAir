// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OpenSky identifier screen: the measured symbol rate, levels, width and burst pattern, and whether the signal fits OpenSky. Nothing is decoded: the air interface is not public and the traffic is encrypted.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/opensky/opensky_rx.h"
#include "dect2/opensky/opensky_tel.h"
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 323; }   // family 322, engine standard 324, active standard 323

void tab(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "tune to the channel and start the receiver")); return; }
    const dect2::OpenSkyTelemetry& t = a.rx.opensky;
    ImGui::TextUnformatted(tr("OpenSky identifier: the channel is measured, nothing is decoded"));
    ImGui::TextDisabled("%s", tr("OpenSky's air interface is not public and its traffic is encrypted."));
    ImGui::Spacing();
    if (!t.dataValid) { ImGui::TextDisabled("%s", tr("analysing (about 1.5 s of signal)")); return; }
    ImGui::Text("%s: %.0f baud (%.1f dB above the floor)", tr("Symbol rate"), t.baud, t.lineDb);
    ImGui::Text("%s: %d", tr("Frequency levels"), t.levels);
    ImGui::Text("%s: %.0f Hz", tr("Level spread"), t.deviationHz);
    ImGui::Text("%s: %.1f kHz", tr("Occupied width (90 %)"), t.occupiedHz / 1e3);
    if (t.burstPeriodMs > 0) ImGui::Text("%s: %.1f ms, %s %.0f %%", tr("Burst period"), t.burstPeriodMs, tr("carrier on"), t.burstDuty * 100); else ImGui::TextDisabled("%s", tr("Continuous carrier"));
    ImGui::Spacing();
    ImGui::TextColored(t.looksLikeOpenSky ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", t.verdict.c_str());
}

void receiver(App& a) { if (live(a)) ImGui::TextUnformatted(dect2::openSkySummary(a.rx.opensky).c_str()); }
void status(App& a) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(); ImGui::TextUnformatted(live(a) ? dect2::openSkySummary(a.rx.opensky).c_str() : tr(a.engine.running() ? "starting" : "stopped")); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = "OpenSky identifier"; if (live(a)) l2 = dect2::openSkySummary(a.rx.opensky); }

} // namespace

extern const ModeUi kOpenSkyUi;
const ModeUi kOpenSkyUi = { .sideTitle = "", .tabName = "OpenSky", .tabIcon = Ic::Radio, .tab = tab, .receiver = receiver, .status = status, .summary = summary };
