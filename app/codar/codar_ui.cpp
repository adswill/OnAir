// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar screens: the radars heard (centre, width, rate, direction, repetition, level, first / last heard, SNR history, ID),
// the chirp waterfall with the sweeps marked, the "heard on band X at time Y" history, band presets and a scan over the radar bands.
// Receive only: broadcast radar signals; nothing is decrypted or transmitted. The ID text is always labelled unverified.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/codar/codar_rx.h"
#include "dect2/codar/codar_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 radars, 1 waterfall, 2 heard
    bool loaded = false, scan = false;
    int dwellSec = 20;
    double lastHop = 0;
    int selId = 0;                       // the radar clicked: its details and its marks
    size_t scanIdx = 0;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.scan = plat::prefs().getB("cdScan", false);
    S.dwellSec = std::max(5, std::min(300, (int)plat::prefs().getI("cdDwell", 20)));
}

bool live(const App& a) { return a.rx.standard == 73; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: these are broadcast radar signals (CODAR SeaSonde, WERA and similar coastal HF radars that measure ocean surface currents); nothing is "
                        "decrypted or transmitted. A sweep is a line that climbs or falls across the spectrum about once or twice a second. Radars on one frequency take turns, "
                        "so the sweep numbers are only a hint of what a radar is (shown as possible). The call sign some radars send in Morse is read from the keyed sweep "
                        "width and is unverified: it follows the published description, not a recording. The band and time a radar was heard tell about propagation only; "
                        "no position is claimed.");
    ImGui::PopTextWrapPos();
}

const dect2::CdStation* find(const dect2::CodarTelemetry& t, int id) {
    for (const auto& s : t.stations) if (s.id == id) return &s;
    return nullptr;
}

void radarsView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to look for ocean radar sweeps"); return; }
    const dect2::CodarTelemetry& t = a.rx.codar;
    if (ImGui::Button("Clear") && live(a)) a.engine.codar().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every radar and event heard so far");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%llu sweeps counted, %llu tracks rejected (not linear, too wide or too slow)", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    const float tableH = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.6f);
    if (ImGui::BeginTable("##cdst", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("Centre"); ImGui::TableSetupColumn("Band"); ImGui::TableSetupColumn("Width"); ImGui::TableSetupColumn("Rate");
        ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Repeat"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Sweeps");
        ImGui::TableSetupColumn("First heard"); ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("ID (unverified)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& s : t.stations) {
            ImGui::TableNextRow();
            ImGui::PushID(s.id);
            const double ago = std::max(0.0, t.timeSec - s.lastSec);
            const ImVec4 c = ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TableNextColumn();
            if (ImGui::Selectable("##row", S.selId == s.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selId = S.selId == s.id ? 0 : s.id;
            ImGui::SameLine(0, 0);
            ImGui::TextColored(c, "%d", s.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nclick to show its details", s.hint.c_str());
            ImGui::TableNextColumn(); if (s.centreMhz > 0) ImGui::Text("%.4f MHz", s.centreMhz); else ImGui::Text("%+.1f kHz", s.offsetKhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.band.empty() ? "-" : s.band.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f kHz", s.bwKhz);
            ImGui::TableNextColumn(); ImGui::Text("%.0f kHz/s", s.rateKhzS);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.dir > 0 ? "rising" : "falling");
            ImGui::TableNextColumn(); if (s.periodSec > 0) ImGui::Text("%.3f s", s.periodSec); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", s.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.sweeps);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(s.firstWall).c_str());
            ImGui::TableNextColumn(); ImGui::Text(ago < 2 ? "%s (now)" : "%s", clock(s.lastWall).c_str());
            ImGui::TableNextColumn();
            if (s.callsign.empty()) ImGui::TextDisabled("-");
            else {
                ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", s.callsign.c_str()); ImGui::PopFont();
                ImGui::SameLine(0, 5 * gUi);
                ImGui::TextDisabled("unverified, %.0f wpm, seen %d times, %s", s.idWpm, s.idSeen, clock(s.idWall).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (t.stations.empty()) ImGui::TextDisabled("no radar sweep heard yet%s", t.bandNow.empty() ? "" : (std::string(" (the radio is in the ") + t.bandNow + " band)").c_str());
    const dect2::CdStation* s = find(t, S.selId);
    if (s) {
        ImGui::Separator();
        ImGui::TextWrapped("Radar %d: %s", s->id, s->hint.c_str());
        if (s->snrHist.size() >= 2) {
            ImGui::TextDisabled("SNR history (one point per second heard), best %.1f dB", s->snrMax);
            ImGui::PlotLines("##cdsnr", s->snrHist.data(), (int)s->snrHist.size(), 0, nullptr, 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi));
        }
    }
}

ImU32 ramp(uint8_t v) {
    const float x = v / 255.f;
    const float r = std::min(1.f, std::max(0.f, 3 * x - 1.0f)), g = std::min(1.f, std::max(0.f, 3 * x - 0.3f)) * 0.9f, b = std::min(1.f, 0.25f + 1.6f * x) * (x > 0.7f ? 0.6f : 1.f);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

void waterfallView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the sweeps"); return; }
    const dect2::CodarTelemetry& t = a.rx.codar;
    ImGui::TextDisabled("Spectrum over about %.0f s (%.0f kHz wide, noise floor at the bottom of the scale): each sweep is a slanted line; white ticks mark the sweeps the detector counted",
                        dect2::kCdWfRows * t.wfRowSec, t.spanKhz);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float W = std::max(100.f, avail.x), H = std::max(100.f * gUi, avail.y - 20 * gUi);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(8, 10, 16, 255));
    if (t.wf.size() == (size_t)dect2::kCdWfCols * dect2::kCdWfRows) {
        const float cw = W / dect2::kCdWfCols, rh = H / dect2::kCdWfRows;
        for (int r = 0; r < dect2::kCdWfRows; r++)
            for (int c = 0; c < dect2::kCdWfCols; c++) {
                const uint8_t v = t.wf[(size_t)r * dect2::kCdWfCols + (size_t)c];
                if (v < 30) continue;
                dl->AddRectFilled(ImVec2(p0.x + c * cw, p0.y + r * rh), ImVec2(p0.x + (c + 1) * cw + 0.5f, p0.y + (r + 1) * rh + 0.5f), ramp(v));
            }
    }
    const double total = dect2::kCdWfRows * t.wfRowSec;
    auto X = [&](double khz) { return p0.x + (float)((khz / std::max(1.0, t.spanKhz) + 0.5) * W); };
    auto Y = [&](double sec) { return p0.y + H - (float)((t.wfEndSec - sec) / total * H); };
    for (const auto& m : t.sweepMarks) {
        const dect2::CdStation* s = find(t, m.station);
        const double rate = s && s->rateKhzS > 0 ? s->rateKhzS : 100.0;
        const double dur = std::fabs(m.f1Khz - m.f0Khz) / rate;
        if (Y(m.sec + dur) < p0.y) continue;
        const bool sel = m.station == S.selId;
        dl->AddLine(ImVec2(X(m.f0Khz) - 6, Y(m.sec)), ImVec2(X(m.f0Khz) - 1, Y(m.sec)), sel ? IM_COL32(255, 220, 80, 255) : IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(X(m.f1Khz) + 1, Y(m.sec + dur)), ImVec2(X(m.f1Khz) + 6, Y(m.sec + dur)), sel ? IM_COL32(255, 220, 80, 255) : IM_COL32(255, 255, 255, 200), 1.5f);
    }
    ImGui::Dummy(ImVec2(W, H));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mp = ImGui::GetMousePos();
        const double khz = ((mp.x - p0.x) / W - 0.5) * t.spanKhz;
        ImGui::SetTooltip("%+.1f kHz from the radio's centre%s", khz, t.centerMhz > 0 ? (std::string(", ") + std::to_string(t.centerMhz + khz / 1e3).substr(0, 8) + " MHz").c_str() : "");
    }
    ImGui::TextDisabled("%+.0f kHz", -t.spanKhz / 2); ImGui::SameLine(W / 2 - 20 * gUi); ImGui::TextDisabled("centre"); ImGui::SameLine(W - 60 * gUi); ImGui::TextDisabled("%+.0f kHz", t.spanKhz / 2);
}

void heardView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to build the history"); return; }
    const dect2::CodarTelemetry& t = a.rx.codar;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Where a radar was heard and when, the way a propagation log reads (a radar first heard, or heard again after a minute's pause). Nothing here says where "
                        "the radar is.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : t.events) { char b[200]; snprintf(b, sizeof b, "%s%s\tradar %d\t%.4f MHz\t%s\t%.1f dB\n", clock(e.wall).c_str(), dect2::timefmt::sfx(), e.station, e.centreMhz, e.band.c_str(), e.snrDb); txt += b; }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##cdev", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Radar"); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Band"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : t.events) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(e.wall).c_str());
        ImGui::TableNextColumn(); ImGui::Text("#%d", e.station);
        ImGui::TableNextColumn(); if (e.centreMhz > 0) ImGui::Text("%.4f MHz", e.centreMhz); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.band.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f dB", e.snrDb);
    }
    ImGui::EndTable();
    if (t.events.empty()) ImGui::TextDisabled("nothing heard yet");
}

void hopTo(App& a, double mhz) {
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);   // no reset: the tables stay
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    loadState();
    const bool fileSrc = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].kind == DeviceInfo::File;
    if (!a.engine.running() || !live(a) || !S.scan || fileSrc) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellSec) return;
    S.lastHop = ImGui::GetTime();
    const auto plan = dect2::codar::scanPlan(a.tune.sampleRate > 0 ? a.tune.sampleRate : 2e6);
    if (plan.empty()) return;
    size_t next = 0;
    for (size_t i = 0; i < plan.size(); i++) if (std::fabs(plan[i] - a.freqMhz) < 0.01) { next = (i + 1) % plan.size(); break; }
    hopTo(a, plan[next]);
}

void tuner(App& a, bool& retune) {
    loadState();
    int nb = 0;
    const dect2::codar::Band* bands = dect2::codar::bands(nb);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band");
    ImGui::SameLine(0, 5 * gUi);
    std::string cur = dect2::codar::bandOf(a.freqMhz);
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##cdband", cur.empty() ? "other" : cur.c_str())) {
        for (int i = 0; i < nb; i++) {
            const double mid = std::round(0.5 * (bands[i].loMhz + bands[i].hiMhz) * 1000) / 1000;
            if (ImGui::Selectable(bands[i].name, cur == bands[i].name)) { tuneFreq(a, mid); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The ITU oceanographic radar bands. The radio takes in one part of the band at a time (1.8 MHz at 2 Msps, so a whole band at once from about\n"
                                                  "12 Msps for 12 - 14 MHz); a wide radio sees all radars of a band together, a narrow one needs the scan.");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("scan radar bands", &S.scan)) { plat::prefs().setB("cdScan", S.scan); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step through the radar bands in parts the sample rate takes in, listening to each for the dwell time (a radar heard on one part is not\n"
                                                  "heard on the others meanwhile). Off: single position.");
    if (S.scan) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##cddw", &S.dwellSec, 5, 300, "%d s")) { plat::prefs().setI("cdDwell", S.dwellSec); plat::prefs().flush(); }
    }
}

void tab(App& a) {
    subNav("cdv", S.view, {"Radars", "Waterfall", "Heard"});
    if (S.view == 0) radarsView(a); else if (S.view == 1) waterfallView(a); else heardView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CodarTelemetry& t = a.rx.codar;
    if (!ImGui::BeginTable("##cdl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Radar", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Width"); ImGui::TableSetupColumn("SNR");
    for (const auto& s : t.stations) {
        ImGui::TableNextRow();
        ImGui::PushID(s.id);
        const bool recent = t.timeSec - s.lastSec < 5;
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selId == s.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selId = S.selId == s.id ? 0 : s.id;
        ImGui::SameLine(0, 0);
        if (s.centreMhz > 0) ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%.3f MHz%s%s", s.centreMhz, s.callsign.empty() ? "" : " ", s.callsign.c_str());
        else ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%+.0f kHz", s.offsetKhz);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.hint.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f kHz", s.bwKhz);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f dB", s.snrDb);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CodarTelemetry& t = a.rx.codar;
    kvRow(a, "state", "%s", dect2::codarSummary(t).c_str());
    if (t.centerMhz > 0) kvRow(a, "tuned", "%.3f MHz%s%s", t.centerMhz, t.bandNow.empty() ? ", outside the radar bands" : ", ", t.bandNow.c_str());
    kvRow(a, "sweeps", "%llu counted, %llu tracks rejected, %llu broadband bursts skipped", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.wideBursts);
    kvRow(a, "span", "%.0f kHz, input %.3f Msps, %.1f dBFS", t.spanKhz, t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sweeps", !on ? 0 : a.rx.codar.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::codarSummary(a.rx.codar).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CODAR / HF ocean radar";
    if (live(a)) l2 = dect2::codarSummary(a.rx.codar);
}

} // namespace

extern const ModeUi kCodarUi;
const ModeUi kCodarUi = {
    .sideTitle = "RADARS",
    .tabName = "Radar",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
