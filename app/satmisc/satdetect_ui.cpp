// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Screens of the satellite carrier and burst detector modes (core/src/satmisc). One screen, driven by the system table: it says plainly what each
// system is and what is not known about it. Elements come from the same folder as the tracker's.
#include "app.h"
#include "dect2/satmisc/sat_elements.h"
#include "dect2/satmisc/satdetect_live.h"
#include "dect2/satmisc/satdetect_tel.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <vector>

// the elements for this screen and the meteor scatter one (declared there)
namespace satui {

struct Elements {
    std::mutex mu;
    std::vector<dect2::Tle> tles;
    uint64_t version = 0;
    double loadedAt = -1e9;
    std::future<void> job;
};
Elements gE;

// Every frame: reload from the tracker's folder at most every ten minutes, in the background
void elementsTick(std::vector<dect2::Tle>& out, uint64_t& seenVersion) {
    const double now = ImGui::GetTime();
    if (now - gE.loadedAt > 600 && (!gE.job.valid() || gE.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        gE.loadedAt = now;
        const std::string dir = plat::dataDir() + "/tle";
        gE.job = std::async(std::launch::async, [dir] {
            std::vector<dect2::Tle> v = dect2::satmisc::loadAllElements(dir);
            std::lock_guard<std::mutex> k(gE.mu);
            gE.tles = std::move(v); gE.version++;
        });
    }
    std::lock_guard<std::mutex> k(gE.mu);
    if (seenVersion != gE.version) { out = gE.tles; seenVersion = gE.version; }
}

} // namespace satui

namespace {

struct State {
    std::vector<dect2::Tle> tles;
    uint64_t seen = 0, pushed = 0;
    double lastHop = 0, lastActive = 0;
    bool hop = true;
};
State S;

bool live(const App& a) { return a.rx.standard == a.family + 1; }
const dect2::satmisc::SatSystem* sys(const App& a) { return dect2::satmisc::satSystem(a.family + 2); }
constexpr int kIssFamily = 562;

void pushElements(App& a) {
    const auto* s = sys(a);
    if (!s || !s->tleMatch) return;
    satui::elementsTick(S.tles, S.seen);
    if (S.seen != S.pushed && a.engine.running()) {
        double lat, lon;
        receiverLocation(lat, lon);
        a.engine.satdetect().setSatellites(S.tles, lat, lon, receiverAltitudeM() / 1000);
        S.pushed = S.seen;
    }
}

void openAs(App& a, int family, double mhz) {
    setFamily(a, family);
    a.freqMhz = mhz;
    if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
}

// the protocol an ISS channel hands to
int familyForChannel(const std::string& c) {
    if (c == "APRS packet") return 24;
    if (c == "FM voice / SSTV" || c == "Voice repeater" || c == "UHF simplex") return 41;
    return -1;
}

void linesView(App& a, const dect2::SatDetectTelemetry& t) {
    const bool issMode = a.family == kIssFamily;
    if (t.lines.empty()) { ImGui::TextDisabled("%s", tr("No carrier yet.")); return; }
    if (!ImGui::BeginTable("##sdlines", issMode ? 8 : 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("SNR")); ImGui::TableSetupColumn(tr("Drift Hz/s")); ImGui::TableSetupColumn(tr("Seconds"));
    ImGui::TableSetupColumn(tr("Channel")); ImGui::TableSetupColumn(tr("Satellite")); ImGui::TableSetupColumn(tr("Fit Hz"));
    if (issMode) ImGui::TableSetupColumn("##open");
    ImGui::TableHeadersRow();
    int row = 0;
    for (const auto& l : t.lines) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (l.mhz > 0) ImGui::Text("%.4f", l.mhz); else ImGui::Text("%+.0f Hz", l.offsetHz);
        if (l.active) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "*"); }
        ImGui::TableNextColumn(); ImGui::Text("%.0f", l.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%+.1f", l.driftHzS);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", l.durSec);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(l.channel.c_str());
        ImGui::TableNextColumn();
        if (l.sat.empty()) ImGui::TextDisabled("-");
        else if (l.ambiguous) ImGui::Text("%s (%s)", l.sat.c_str(), tr("or a neighbour"));
        else ImGui::TextUnformatted(l.sat.c_str());
        ImGui::TableNextColumn();
        if (!l.sat.empty()) ImGui::Text("%.0f", l.rmsHz);
        if (issMode) {
            ImGui::TableNextColumn();
            const int f = familyForChannel(l.channel);
            if (f >= 0 && l.mhz > 0) {
                char lb[32]; snprintf(lb, sizeof lb, "%s##o%d", f == 24 ? tr("Decode") : tr("Listen"), row);
                if (ImGui::SmallButton(lb)) { openAs(a, f, f == 24 ? 145.825 : std::round(l.mhz * 1000) / 1000); ImGui::EndTable(); return; }
            }
        }
        row++;
    }
    ImGui::EndTable();
}

void channelsView(App&, const dect2::SatDetectTelemetry& t) {
    if (t.channels.empty()) { ImGui::TextDisabled("%s", tr("No active channel yet.")); return; }
    if (!ImGui::BeginTable("##sdchan", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Width kHz")); ImGui::TableSetupColumn(tr("SNR")); ImGui::TableSetupColumn(tr("Duty %")); ImGui::TableSetupColumn(tr("Bursts")); ImGui::TableSetupColumn(tr("Channel"));
    ImGui::TableHeadersRow();
    for (const auto& c : t.channels) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (c.mhz > 0) ImGui::Text("%.3f", c.mhz); else ImGui::Text("%+.0f kHz", c.offsetHz / 1e3);
        if (c.active) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "*"); }
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.widthHz / 1e3);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.duty * 100);
        ImGui::TableNextColumn(); ImGui::Text("%u", c.bursts);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.channel.c_str());
    }
    ImGui::EndTable();
}

void framesView(const dect2::SatDetectTelemetry& t) {
    for (const auto& f : t.frames) {
        ImGui::TextColored(f.detected ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", f.detected ? tr("Detected") : tr("Not detected"));
        ImGui::Text("%s: %.3f Hz, %.1f dB over the background", tr("Repetition"), f.measuredHz, f.levelDb);
    }
}

void tab(App& a) {
    const auto* s = sys(a);
    if (!s) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(s->note));
    ImGui::PopTextWrapPos();
    if (a.family == kIssFamily) {
        ImGui::Checkbox(tr("Change band by itself when nothing is heard"), &S.hop);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Goes between the VHF downlinks (145.800 and 145.825 in one view) and the UHF repeater every 20 seconds until a carrier is found."));
    }
    if (s->tleMatch && S.tles.empty()) ImGui::TextColored(pal::warnAmber(), "%s", tr("No orbital elements yet: open View > Passes once to download them."));
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::SatDetectTelemetry& t = a.rx.satdetect;
    if (!t.spectrum.empty()) ImGui::PlotLines("##sdspec", t.spectrum.data(), (int)t.spectrum.size(), 0, nullptr, -3.f, 40.f, ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi));
    if (s->method == dect2::satmisc::SdMethod::Carrier) linesView(a, t);
    else if (s->method == dect2::satmisc::SdMethod::Channel) channelsView(a, t);
    else framesView(t);
}

void tick(App& a) {
    pushElements(a);
    if (a.family != kIssFamily || !S.hop || !live(a) || !a.engine.running()) return;
    const double now = ImGui::GetTime();
    if (a.rx.satdetect.dataValid) S.lastActive = now;
    if (now - std::max(S.lastActive, S.lastHop) < 20) return;
    S.lastHop = now;
    a.freqMhz = dect2::satmisc::issNextCentreMhz(a.freqMhz);
    a.tune.centerHz = a.freqMhz * 1e6;
    a.engine.retune(a.tune);
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& t = a.rx.satdetect;
    ImGui::TextUnformatted(dect2::sdSummary(t).c_str());
    ImGui::Text("%s: %.2f Msps, %.1f Hz", tr("Input"), t.inputRate / 1e6, t.binHz);
    if (sys(a) && sys(a)->tleMatch) ImGui::Text("%s: %d", tr("Satellites with elements"), t.catalog);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(!live(a) ? (a.engine.running() ? tr("starting") : tr("stopped")) : dect2::sdSummary(a.rx.satdetect).c_str());
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const auto* s = sys(a);
    l1 = s ? s->name : "";
    if (live(a)) l2 = dect2::sdSummary(a.rx.satdetect);
}

void synth(App& a, bool& changed) {
    const char* names[] = {tr("Signal"), tr("Noise only")};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 1) o = 0;
    for (int i = 0; i < 2; i++) { if (i) ImGui::SameLine(); if (ImGui::RadioButton(names[i], o == i)) { o = i; changed = true; } }
}

} // namespace

extern const ModeUi kSatDetectUi;
const ModeUi kSatDetectUi = {
    .sideTitle = "SIGNALS",
    .tabName = "Signals",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};
