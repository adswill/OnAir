// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar screens: the 13 CDMA channels (power, SNR, occupancy, Doppler), the Doppler track against time with the pass planner's satellites drawn over it
// (predicted Doppler of the Globalstar satellites above the horizon, from the TLEs the planner loaded), and the event log. Receive only: the L-band signal (the terminal uplink) is
// spread and encrypted and nothing of its content is decoded.
#include "app.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/globalstar/globalstar_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 channels, 1 Doppler, 2 events
    float windowSec = 300;
    bool overlay = true;
    double predAt = 0;                   // when the satellite overlay was last computed (wall clock)
    std::map<std::string, std::vector<ImVec2>> pred;   // satellite -> (seconds before now, Doppler Hz)
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 63; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string clock(int64_t t, double sigSec) {
    char b[24];
    if (t <= 0) { snprintf(b, sizeof b, "%.1f s", sigSec); return b; }
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

ImU32 chanCol(int c) {
    const float h = std::fmod(c * 0.173f + 0.05f, 1.f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.65f, 0.95f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

void channelsView(App& a) {
    const bool on = live(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only. The Globalstar L-band signal (1610 - 1626.5 MHz, the terminal uplink; the satellite downlink is S-band, 2483.5 - 2500 MHz, and is not received here) is spread (CDMA) and encrypted: this screen shows which of its 1.23 MHz channels carry a signal, how strongly and how often, and the Doppler shift. "
                        "Nothing is decoded, and nothing can be: the content is not for listening.");
    ImGui::PopTextWrapPos();
    if (!on) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::GlobalstarTelemetry& t = a.rx.globalstar;
    if (ImGui::BeginTable("##gsc", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Power");
        ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Occupancy", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Doppler"); ImGui::TableSetupColumn("Events");
        ImGui::TableHeadersRow();
        for (const dect2::GsChannel& c : t.chan) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (c.inView) ImGui::PushStyleColor(ImGuiCol_Text, c.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Text("%d", c.index + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", c.centreMhz);
            if (!c.inView) { ImGui::TableNextColumn(); ImGui::TextUnformatted("outside the band sampled"); ImGui::PopStyleColor(); continue; }
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.powerDb);
            ImGui::TableNextColumn(); ImGui::Text(c.active ? "%.1f dB" : "-", c.snrDb);
            ImGui::TableNextColumn(); ImGui::ProgressBar(std::min(1.f, c.occupancy), ImVec2(-1, 0), "");
            ImGui::TableNextColumn(); ImGui::Text(c.active ? "%+.1f kHz" : "-", c.dopplerHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%u", c.events);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

void refreshPred(const App& a, const dect2::GlobalstarTelemetry& t) {
    const double wall = (double)std::time(nullptr);
    if (wall - S.predAt < 5) return;
    S.predAt = wall;
    S.pred.clear();
    const double f = (t.dialMhz > 0 ? t.dialMhz : 1618.25) * 1e6;
    (void)a;
    for (double back = S.windowSec; back >= 0; back -= std::max(5.0, S.windowSec / 60.0))
        for (const auto& [name, hz] : passesGlobalstarDoppler(wall - back, f)) S.pred[name].push_back(ImVec2((float)back, (float)hz));
}

void dopplerView(App& a) {
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat("##gsw", &S.windowSec, 60, 600, "%.0f s");
    flowNext();
    ImGui::Checkbox("Pass planner overlay", &S.overlay);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The predicted Doppler of every Globalstar satellite above the horizon, from the TLEs of the pass planner (View > Passes: download, and set your position in the ADS-B screen)");
    const bool on = live(a);
    const dect2::GlobalstarTelemetry empty;
    const dect2::GlobalstarTelemetry& t = on ? a.rx.globalstar : empty;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = std::max(100.f, avail.x), H = std::max(120.f, avail.y - 4);
    const ImVec2 p1(p0.x + W, p0.y + H);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##gsd", ImVec2(W, H));
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const float yMax = 45000.f;
    auto Y = [&](float hz) { return p0.y + H * (0.5f - hz / (2 * yMax)); };
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f);
    for (int k = -40; k <= 40; k += 20) {
        dl->AddLine(ImVec2(p0.x, Y(k * 1000.f)), ImVec2(p1.x, Y(k * 1000.f)), grid);
        char b[16]; snprintf(b, sizeof b, "%+d kHz", k);
        dl->AddText(ImVec2(p0.x + 3, Y(k * 1000.f) - 14 * gUi), ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
    }
    if (!on) return;
    auto X = [&](double secBack) { return p1.x - (float)(secBack / S.windowSec) * W; };
    if (S.overlay) {
        refreshPred(a, t);
        const double wall = (double)std::time(nullptr);
        int row = 0;
        for (const auto& [name, pts] : S.pred) {
            std::vector<ImVec2> v;
            for (const ImVec2& q : pts) v.push_back(ImVec2(X(q.x - (float)(wall - S.predAt)), Y(q.y)));
            if (v.size() >= 2) dl->AddPolyline(v.data(), (int)v.size(), IM_COL32(200, 200, 200, 110), 0, 1.f * gUi);
            if (!v.empty() && row < 8) { dl->AddText(ImVec2(std::min(v.back().x, p1.x - 90 * gUi), v.back().y - 14 * gUi), IM_COL32(200, 200, 200, 160), name.c_str()); row++; }
        }
    }
    for (const dect2::GsDopplerPoint& p : t.track) {
        const double back = t.timeSec - p.timeSec;
        if (back < 0 || back > S.windowSec) continue;
        dl->AddCircleFilled(ImVec2(X(back), Y(std::max(-yMax, std::min(yMax, p.hz)))), 2.5f * gUi, chanCol(p.chan));
    }
    // channel legend
    float lx = p0.x + 6;
    for (const dect2::GsChannel& c : t.chan) {
        if (!c.inView) continue;
        char b[8]; snprintf(b, sizeof b, "%d", c.index + 1);
        dl->AddCircleFilled(ImVec2(lx, p0.y + 8 * gUi), 4 * gUi, chanCol(c.index));
        dl->AddText(ImVec2(lx + 7 * gUi, p0.y + 1), ImGui::GetColorU32(ImGuiCol_Text), b);
        lx += 28 * gUi;
    }
}

void eventsView(App& a) {
    const bool on = live(a);
    const std::vector<dect2::GsEvent> none;
    const std::vector<dect2::GsEvent>& ev = on ? a.rx.globalstar.events : none;
    if (ImGui::SmallButton("Clear")) a.engine.globalstar().clearResults();
    if (ImGui::BeginTable("##gse", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Duration");
        ImGui::TableSetupColumn("Peak SNR"); ImGui::TableSetupColumn("Doppler start to end", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::GsEvent& e : ev) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(e.wallTime, e.startSec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", e.chan + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", dect2::globalstarChannelHz(e.chan) / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f s", e.durationSec);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", e.peakSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f to %+.1f kHz", e.dopplerStartHz / 1e3, e.dopplerEndHz / 1e3);
        }
        ImGui::EndTable();
    }
    if (ev.empty()) ImGui::TextDisabled("%s", on ? "no activity yet" : a.engine.running() ? "starting" : "stopped");
}

void tab(App& a) {
    subNav("gsv", S.view, {"Channels", "Doppler", "Events"});
    if (S.view == 0) channelsView(a); else if (S.view == 1) dopplerView(a); else eventsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::GlobalstarTelemetry& t = a.rx.globalstar;
    if (!ImGui::BeginTable("##gsl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Doppler");
    int n = 0;
    for (const dect2::GsChannel& c : t.chan) {
        if (!c.active) continue;
        n++;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "%d  %.2f MHz", c.index + 1, c.centreMhz);
        ImGui::TableNextColumn(); ImGui::Text("%+.1f kHz", c.dopplerHz / 1e3);
    }
    ImGui::EndTable();
    if (!n) ImGui::TextDisabled("no channel active");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::GlobalstarTelemetry& t = a.rx.globalstar;
    int inView = 0;
    for (const auto& c : t.chan) inView += c.inView;
    kvRow(a, "state", "%s", dect2::globalstarSummary(t).c_str());
    kvRow(a, "channels", "%d of 13 in the band sampled (%.2f Msps), %d active now", inView, t.inputRate / 1e6, t.activeNow);
    kvRow(a, "events", "%llu activity stretches logged", (unsigned long long)t.blocksOk);
    kvRow(a, "dial", t.dialMhz > 0 ? "%.3f MHz" : "unknown (a file): the band centre 1618.25 MHz is assumed", t.dialMhz);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Doppler is the centroid of a channel's spectrum against its nominal centre; two satellites on one channel give a mixture. "
                        "Receive only: no decoding, no decryption. Check your local rules before listening to satellite phone and data links.");
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Activity", !on ? 0 : a.rx.globalstar.activeNow > 0 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::globalstarSummary(a.rx.globalstar).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Globalstar";
    if (live(a)) l2 = dect2::globalstarSummary(a.rx.globalstar);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    static const char* const names[3] = {"a satellite pass", "steady channel", "silence"};
    const int cur = std::max(0, std::min(2, sc.modeOpt[0]));
    if (ImGui::BeginCombo("##gssy", names[cur])) {
        for (int k = 0; k < 3; k++) if (ImGui::Selectable(names[k], cur == k)) { sc.modeOpt[0] = k; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kGlobalstarUi;
const ModeUi kGlobalstarUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Activity",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
};
