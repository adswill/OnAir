// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium > Sky: every Iridium satellite above the horizon from the user's TLEs, the ones identified by their Doppler marked with the number the
// header gives them, the receiver's frequency error that falls out of the fit, and the ground circle of the newest beams. Automatic: it needs the
// receiver position (Settings) and TLEs (View > Passes downloads them); nothing is configured here.
#include "iridium_sky.h"
#include "dect2/iridium/iridium_doppler.h"
#include "dect2/iridium/iridium_tel.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

struct SkyState {
    dect2::IridiumDoppler id;
    double lastTime = -1;           // the newest ring alert already fed (receiver time)
    double storeAt = -1e9;
    size_t storeN = 0;
    double lastLat = 1e9, lastLon = 1e9;
    std::string note;
} K;

double wallNow() { return (double)std::time(nullptr); }

void loadStore() {
    if (wallNow() - K.storeAt < 60) return;
    K.storeAt = wallNow();
    std::vector<dect2::Tle> all;
    std::error_code ec;
    const std::filesystem::path dir(reinterpret_cast<const char8_t*>((plat::dataDir() + "/tle").c_str()));
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".txt" && ext != ".tle") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        for (const dect2::Tle& t : dect2::parseTleText(ss.str())) all.push_back(t);
    }
    if (all.size() != K.storeN) { K.storeN = all.size(); K.id.setTles(all); }
}

} // namespace

void iridiumSkyTick(App& a) {
    if (!a.engine.running() || a.rx.standard != 19 || !a.engine.sampleLoss().live) return;   // a recording's clock is not today's
    loadStore();
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    if (!receiverLocationSet()) return;
    if (lat != K.lastLat || lon != K.lastLon) { K.lastLat = lat; K.lastLon = lon; K.id.setObserver({lat, lon, 0.0}); }
    const dect2::IridiumTelemetry& t = a.rx.iridium;
    const double now = wallNow();
    for (const auto& ra : t.ringAlerts) {
        if (ra.time <= K.lastTime) continue;
        const double wall = now - (t.timeSec - ra.time);   // the alert is new: its age is how far the receiver clock has run on
        K.id.addBurst(wall, ra.sat, ra.freqHz);
        if (ra.hasPos && ra.altKm < 100) K.id.addBeam(wall, ra.sat, ra.beam, ra.lat, ra.lon);
        K.lastTime = ra.time;
    }
}

void iridiumSkyView(App& a, float W, float H) {
    const double now = wallNow();
    if (!receiverLocationSet()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Set the receiver position (Settings) to see the satellites above you.")); ImGui::PopTextWrapPos(); return; }
    if (K.id.satellites() == 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No Iridium elements in the TLE files: download them in View > Passes (the Iridium group), or paste them there.")); ImGui::PopTextWrapPos(); return; }
    (void)a;
    const auto idents = K.id.identify(now);
    const auto sky = K.id.sky(now);
    const float R = std::min(W * 0.55f, H - 8 * gUi) * 0.5f;
    ImGui::BeginChild("##iri_sky_l", ImVec2(std::max(60.f, 2 * R + 12 * gUi), H));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 c(p.x + R + 4, p.y + R + 4);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int el : {0, 30, 60}) dl->AddCircle(c, R * (90 - el) / 90.f, IM_COL32(90, 95, 100, 255), 48);
        dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), IM_COL32(60, 64, 68, 255));
        dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), IM_COL32(60, 64, 68, 255));
        dl->AddText(ImVec2(c.x - 4, c.y - R - 14), IM_COL32(200, 200, 200, 255), "N");
        dl->AddText(ImVec2(c.x + R + 2, c.y - 7), IM_COL32(200, 200, 200, 255), "E");
        for (const auto& s : sky) {
            const float r = R * (float)(90 - s.elDeg) / 90.f, az = (float)(s.azDeg * M_PI / 180);
            const ImVec2 q(c.x + r * std::sin(az), c.y - r * std::cos(az));
            if (s.satId >= 0) {
                dl->AddCircleFilled(q, 5 * gUi, IM_COL32(110, 210, 120, 255));
                char b[16]; snprintf(b, sizeof b, "#%d", s.satId);
                dl->AddText(ImVec2(q.x + 7, q.y - 7), IM_COL32(230, 240, 230, 255), b);
            } else dl->AddCircleFilled(q, 3 * gUi, IM_COL32(150, 155, 160, 255));
            if (ImGui::IsMouseHoveringRect(ImVec2(q.x - 6, q.y - 6), ImVec2(q.x + 6, q.y + 6))) ImGui::SetTooltip("%s\nel %.0f az %.0f range %.0f km\nDoppler %+.1f kHz", s.name.c_str(), s.elDeg, s.azDeg, s.rangeKm, s.dopplerHz / 1e3);
        }
        ImGui::Dummy(ImVec2(2 * R + 8, 2 * R + 8));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##iri_sky_r", ImVec2(0, H));
    ImGui::Text("%zu %s", sky.size(), tr("Iridium satellites above the horizon (grey: not heard, green: identified)"));
    if (K.id.offsetKnown()) {
        const double off = K.id.offsetHz();
        ImGui::Text("%s %+.0f Hz (%+.2f ppm)", tr("Receiver frequency error from the satellites:"), off, off / 1626.27e6 * 1e6);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The common offset between the measured and the predicted Doppler of the identified satellites: a free frequency calibration at 1.6 GHz."));
    } else ImGui::TextDisabled("%s", tr("Listening: a satellite is identified after a few ring alerts spread over 20 seconds or more."));
    if (idents.empty()) ImGui::TextDisabled("%s", tr("none identified yet"));
    else if (ImGui::BeginTable("##iri_ids", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Header #")); ImGui::TableSetupColumn(tr("Satellite")); ImGui::TableSetupColumn("NORAD"); ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Fit")); ImGui::TableSetupColumn(tr("Sure")); ImGui::TableSetupColumn(tr("El / Az"));
        ImGui::TableHeadersRow();
        for (const auto& i : idents) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", i.satId);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(i.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", i.norad);
            ImGui::TableNextColumn(); ImGui::Text("%d", i.obs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f Hz", i.rmsHz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(i.sure ? tr("yes") : tr("probably"));
            ImGui::TableNextColumn(); if (i.elDeg > 0) ImGui::Text("%.0f / %.0f", i.elDeg, i.azDeg); else ImGui::TextDisabled("%s", tr("below horizon"));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    (void)now;
}
