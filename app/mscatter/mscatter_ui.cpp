// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter screens: the pings counted (start, duration, peak level, Doppler, drift, type) with the level profile of the selected one, the ping
// rate per hour and per day with the annual showers marked, and the echo waterfall (the +-500 Hz around the beacon line against time). A forward-scatter
// detector: a fixed beacon (GRAVES, 143.050 MHz) is heard only when a meteor trail reflects it. Pings can be appended to a CSV file for unattended runs.
#include "app.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/mscatter/mscatter_dsp.h"
#include "dect2/mscatter/mscatter_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace satui { void elementsTick(std::vector<dect2::Tle>& out, uint64_t& seenVersion); }   // satmisc/satdetect_ui.cpp

namespace {

struct State {
    int view = 0;                 // 0 pings, 1 rates, 2 echoes
    uint32_t sel = 0;             // id of the ping shown in detail
    int thr = 13;                 // detection level, dB over the background
    int pushedThr = -1;
    bool logOn = true;
    int pushedLog = -1;
    std::vector<dect2::Tle> tles;
    uint64_t seen = 0, pushed = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 105; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::tm utc(int64_t t) {
    std::tm r{};
    const time_t tt = (time_t)t;
#ifdef _WIN32
    gmtime_s(&r, &tt);
#else
    gmtime_r(&tt, &r);
#endif
    return r;
}

std::string clock(int64_t sec) {
    const std::tm t = utc(sec);
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

std::string stamp(int64_t sec) {
    const std::tm t = utc(sec);
    char b[32];
    snprintf(b, sizeof b, "%02d-%02d %02d:%02d:%02d", t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

std::string logFile() { return plat::dataDir() + "/meteor_scatter.csv"; }

void pushSettings(App& a) {
    if (S.thr != S.pushedThr) { a.engine.mscatter().setThresholdDb(S.thr); S.pushedThr = S.thr; }
    const int want = S.logOn ? 1 : 0;
    if (want != S.pushedLog) { a.engine.mscatter().setLogFile(S.logOn ? logFile() : std::string()); S.pushedLog = want; }
}

void tick(App& a) {
    pushSettings(a);
    satui::elementsTick(S.tles, S.seen);
    if (S.seen != S.pushed && a.engine.running()) {
        double lat, lon;
        receiverLocation(lat, lon);
        a.engine.mscatter().setSatelliteMatch(S.tles, lat, lon, receiverAltitudeM() / 1000);
        S.pushed = S.seen;
    }
}

ImU32 heat(float v) {   // 0..1: dark, blue, cyan, yellow, white
    const float r = std::min(1.f, std::max(0.f, (v - 0.5f) * 2.f)), g = std::min(1.f, std::max(0.f, v * 1.6f - 0.3f)), b = std::min(1.f, 0.25f + v * 1.4f) * (v > 0.75f ? 1.f - (v - 0.75f) * 2.f : 1.f);
    return IM_COL32((int)(255 * r), (int)(255 * g * 0.95f), (int)(255 * std::max(0.f, b) * (v < 0.02f ? 0.35f : 1.f)), 255);
}

// a bar chart of counts; marks: bar index and a one-letter label drawn over the bar (the showers)
void barChart(const char* id, const std::vector<int>& v, float h, const std::vector<std::string>& labels, const std::vector<std::pair<int, std::string>>& marks) {
    const float w = std::max(80.f * gUi, ImGui::GetContentRegionAvail().x);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(14, 16, 20, 255), 3.f);
    int mx = 1;
    for (int c : v) mx = std::max(mx, c);
    const float bw = w / (float)std::max<size_t>(1, v.size()), top = 16 * gUi, bot = 14 * gUi;
    for (size_t i = 0; i < v.size(); i++) {
        const float x0 = p.x + bw * (float)i, hh = (h - top - bot) * (float)v[i] / (float)mx;
        dl->AddRectFilled(ImVec2(x0 + 1, p.y + h - bot - hh), ImVec2(x0 + bw - 1, p.y + h - bot), IM_COL32(120, 160, 235, 255));
        if (!labels[i].empty()) dl->AddText(ImVec2(x0, p.y + h - bot + 1), IM_COL32(150, 156, 170, 255), labels[i].c_str());
    }
    for (const auto& m : marks) {
        const float x = p.x + bw * ((float)m.first + 0.5f);
        dl->AddLine(ImVec2(x, p.y + top), ImVec2(x, p.y + h - bot), IM_COL32(240, 190, 80, 200), 1.5f);
        dl->AddText(ImVec2(x + 2, p.y + 1), IM_COL32(240, 190, 80, 255), m.second.c_str());
    }
    char mxs[16];
    snprintf(mxs, sizeof mxs, "%d", mx);
    dl->AddText(ImVec2(p.x + 3, p.y + 1), IM_COL32(150, 156, 170, 255), mxs);
    if (hov) {
        const int i = std::max(0, std::min((int)v.size() - 1, (int)((ImGui::GetIO().MousePos.x - p.x) / bw)));
        ImGui::SetTooltip("%s: %d", labels.empty() ? "" : (labels[(size_t)i].empty() ? "" : labels[(size_t)i].c_str()), v[(size_t)i]);
    }
}

void controls(App& a) {
    const dect2::MsTelemetry& t = a.rx.mscatter;
    if (ImGui::Button(tr("Clear"))) a.engine.mscatter().clearResults();
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy"))) {
        std::string txt = dect2::ms::csvHeader();
        const double bhz = t.centreHz > 0 ? t.centreHz + t.cfoHz : 0.0;
        for (auto it = t.pings.rbegin(); it != t.pings.rend(); ++it) txt += dect2::ms::csvLine(*it, bhz);
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies the pings as CSV: id, UTC time, duration, peak level, Doppler, drift, type, beacon frequency."));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Log to file"), &S.logOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Appends every ping to meteor_scatter.csv in the data folder, also when nobody watches the screen."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderInt("##msthr", &S.thr, 8, 24, tr("threshold %d dB"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far above its long-term level a spectrum bin must rise to count as an echo. Lower finds weaker pings and more false ones."));
}

void pingsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the pings")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    controls(a);
    ImGui::TextDisabled("%llu pings, %.0f per hour, %llu bursts rejected", (unsigned long long)t.blocksOk, (double)t.ratePerHour, (unsigned long long)t.rejected);
    if (t.state == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("No beacon line found. Tune to the beacon (GRAVES: 143.050 MHz) so that it lies within 4 kHz of the centre, with 1 Msps or more."));
        ImGui::PopTextWrapPos();
    }
    const float h = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("##mspings", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (UTC)")); ImGui::TableSetupColumn(tr("Duration")); ImGui::TableSetupColumn(tr("Peak"));
        ImGui::TableSetupColumn(tr("Doppler")); ImGui::TableSetupColumn(tr("Drift")); ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& p : t.pings) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((stamp(p.wallTime) + "##" + std::to_string(p.id)).c_str(), S.sel == p.id, ImGuiSelectableFlags_SpanAllColumns)) S.sel = p.id;
            ImGui::TableNextColumn(); ImGui::Text("%.2f s", (double)p.durationSec);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", (double)p.peakSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", (double)p.dopplerHz);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f Hz/s", (double)p.driftHzPerSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.kind ? tr("overdense") : tr("underdense"));
        }
        ImGui::EndTable();
    }
    if (t.pings.empty()) ImGui::TextDisabled("%s", tr("no ping yet: on GRAVES a quiet hour brings a few, a shower peak hundreds"));
    for (const auto& p : t.pings) {
        if (p.id != S.sel) continue;
        ImGui::Spacing();
        ImGui::Text("%s UTC", stamp(p.wallTime).c_str());
        kvRow(a, tr("shape"), "%s, %.2f s, %.1f dB over the background", p.kind ? tr("overdense: long, flat top") : tr("underdense: sharp rise, exponential decay"), (double)p.durationSec, (double)p.peakSnrDb);
        kvRow(a, tr("Doppler"), "%+.0f Hz at the peak, %+.1f Hz/s", (double)p.dopplerHz, (double)p.driftHzPerSec);
        if (p.profile.size() > 1) ImGui::PlotLines("##msprof", p.profile.data(), (int)p.profile.size(), 0, nullptr, -2.f, std::max(20.f, p.peakSnrDb + 3), ImVec2(ImGui::GetContentRegionAvail().x, 60 * gUi));
        break;
    }
}

void ratesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the rates")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    ImGui::TextDisabled("%s", tr("pings per hour (UTC), the last 24 hours"));
    std::vector<std::string> hl(t.hourly.size());
    for (size_t i = 0; i < hl.size(); i++) {
        const int hr = (int)(((t.hourStart / 3600 + (int64_t)i) % 24 + 24) % 24);
        char b[8];
        if (i % 3 == 0) snprintf(b, sizeof b, "%02d", hr); else b[0] = 0;
        hl[i] = b;
    }
    barChart("##mshour", t.hourly, std::max(90.f * gUi, ImGui::GetContentRegionAvail().y * 0.3f), hl, {});
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("pings per day (UTC), the last 30 days; showers marked at their peak"));
    std::vector<std::string> dl(t.daily.size());
    std::vector<std::pair<int, std::string>> marks;
    for (size_t i = 0; i < dl.size(); i++) {
        const std::tm d = utc(t.dayStart + (int64_t)i * 86400);
        char b[8];
        if (i % 5 == 0 || i + 1 == dl.size()) snprintf(b, sizeof b, "%d/%d", d.tm_mon + 1, d.tm_mday); else b[0] = 0;
        dl[i] = b;
    }
    for (const auto& s : t.showers) {
        const int idx = 29 + s.peakDayDelta;
        if (idx >= 0 && idx < 30) marks.push_back({idx, s.name.substr(0, 3)});
    }
    barChart("##msday", t.daily, std::max(110.f * gUi, ImGui::GetContentRegionAvail().y * 0.45f), dl, marks);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("meteor showers now and near (peak date, typical hourly rate for an eye observer)"));
    if (ImGui::BeginTable("##msshow", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Shower")); ImGui::TableSetupColumn(tr("Peak")); ImGui::TableSetupColumn(tr("Rate")); ImGui::TableSetupColumn(tr("Now"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int n = 0;
        for (const auto& s : t.showers) {
            if (n++ >= 6) break;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            if (s.peakDayDelta == 0) ImGui::TextUnformatted(tr("today")); else ImGui::Text(s.peakDayDelta > 0 ? tr("in %d days") : tr("%d days ago"), std::abs(s.peakDayDelta));
            ImGui::TableNextColumn(); ImGui::Text("%d", s.zhr);
            ImGui::TableNextColumn();
            if (s.active) ImGui::TextColored(pal::okGreen(), "%s%s", tr("active"), s.daytime ? tr(", daytime: best for radio") : "");
            else ImGui::TextDisabled("%s", tr("not active"));
        }
        ImGui::EndTable();
    }
}

void echoView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the echoes")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    if (t.state == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("No beacon line found. Tune to the beacon (GRAVES: 143.050 MHz) so that it lies within 4 kHz of the centre, with 1 Msps or more."));
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("%s", tr("echoes: +-500 Hz around the beacon line, the last 30 seconds (brighter = stronger over the background)"));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = std::max(120.f * gUi, ImGui::GetContentRegionAvail().x);
    const float h = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 90 * gUi);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mswf", ImVec2(w, h));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(8, 9, 14, 255));
    if (t.waterfall.size() == (size_t)dect2::kMsWfRows * dect2::kMsWfCols) {
        const float cw = w / dect2::kMsWfCols, ch = h / dect2::kMsWfRows;
        for (int r = 0; r < dect2::kMsWfRows; r++)
            for (int c = 0; c < dect2::kMsWfCols; c++) {
                const uint8_t v = t.waterfall[(size_t)r * dect2::kMsWfCols + c];
                if (v < 30) continue;
                dl->AddRectFilled(ImVec2(p.x + cw * c, p.y + ch * r), ImVec2(p.x + cw * (c + 1) + 0.5f, p.y + ch * (r + 1) + 0.5f), heat(v / 255.f));
            }
    }
    const float cx = p.x + w * 0.5f;
    dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + h), IM_COL32(120, 200, 140, 120));
    dl->AddText(ImVec2(p.x + 3, p.y + h - 14 * gUi), IM_COL32(150, 156, 170, 255), "-500 Hz");
    dl->AddText(ImVec2(cx + 3, p.y + h - 14 * gUi), IM_COL32(150, 156, 170, 255), tr("beacon"));
    dl->AddText(ImVec2(p.x + w - 56 * gUi, p.y + h - 14 * gUi), IM_COL32(150, 156, 170, 255), "+500 Hz");
    if (t.active) dl->AddText(ImVec2(p.x + 4, p.y + 2), IM_COL32(255, 220, 120, 255), tr("ping"));
    if (!t.spectrum.empty()) {
        ImGui::TextDisabled("%s", tr("long-term spectrum around the beacon (dB over the noise)"));
        ImGui::PlotLines("##msspec", t.spectrum.data(), (int)t.spectrum.size(), 0, nullptr, 0.f, 40.f, ImVec2(ImGui::GetContentRegionAvail().x, 56 * gUi));
    }
}

void satView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Satellites seen in the GRAVES radar's reflection: lines that last longer than a meteor and sweep in Doppler, named from the bistatic Doppler curve of every satellite with known elements (Dijon to your location)."));
    ImGui::PopTextWrapPos();
    ImGui::Text("%llu %s, %llu %s", (unsigned long long)t.satEchoTotal, tr("echoes"), (unsigned long long)t.satEchoMatched, tr("named"));
    if (t.satCatalog == 0) ImGui::TextColored(pal::warnAmber(), "%s", tr("No orbital elements yet: open View > Passes once to download them."));
    if (!ImGui::BeginTable("##msats", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn(tr("UTC")); ImGui::TableSetupColumn(tr("Seconds")); ImGui::TableSetupColumn(tr("Doppler Hz")); ImGui::TableSetupColumn(tr("SNR")); ImGui::TableSetupColumn(tr("Satellite"));
    ImGui::TableHeadersRow();
    for (const auto& e : t.satEchoes) {
        ImGui::TableNextRow();
        char tb[32] = ""; time_t tt = (time_t)e.wallTime; struct tm tmv; 
#ifdef _WIN32
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        strftime(tb, sizeof tb, "%H:%M:%S", &tmv);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(tb);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", e.durationSec);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f > %+.0f", e.startHz, e.endHz);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", e.peakSnrDb);
        ImGui::TableNextColumn();
        if (e.name.empty()) ImGui::TextDisabled("%s", tr("unknown"));
        else if (e.ambiguous) ImGui::Text("%s (%s)", e.name.c_str(), tr("or a neighbour"));
        else ImGui::Text("%s  (%.0f Hz)", e.name.c_str(), e.rmsHz);
    }
    ImGui::EndTable();
}

void tab(App& a) {
    subNav("msc", S.view, {tr("Pings"), tr("Rates"), tr("Echoes"), tr("Satellites")});
    if (S.view == 0) pingsView(a);
    else if (S.view == 1) ratesView(a);
    else if (S.view == 2) echoView(a);
    else satView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    ImGui::Text("%llu %s", (unsigned long long)t.blocksOk, tr("pings"));
    ImGui::PushFont(a.mono, 0);
    int n = 0;
    for (const auto& p : t.pings) { if (n++ >= 10) break; ImGui::Text("%s %.1fs %.0fdB", clock(p.wallTime).c_str(), (double)p.durationSec, (double)p.peakSnrDb); }
    ImGui::PopFont();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::MsTelemetry& t = a.rx.mscatter;
    kvRow(a, tr("state"), "%s", dect2::msSummary(t).c_str());
    if (t.state == 0) kvRow(a, tr("beacon"), "%s", tr("not found"));
    else kvRow(a, tr("beacon"), "%+.1f Hz from the centre, %.1f dB over the noise in one bin, %.1f dBFS", t.cfoHz, (double)t.snrDb, (double)t.levelDb);
    kvRow(a, tr("noise"), "%.1f dBFS in one %.2f Hz bin", (double)t.noiseDb, t.binHz);
    kvRow(a, tr("detector"), "%.0f dB threshold, echoes 18 to 500 Hz from the line, %llu bursts rejected as interference", (double)t.thresholdDb, (unsigned long long)t.rejected);
    kvRow(a, tr("input"), "%.3f Msps, worked at %.0f Hz", t.inputRate / 1e6, t.workRate);
    kvRow(a, tr("log"), "%s", t.logging ? t.logPath.c_str() : tr("off"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The beacon is a fixed transmitter far beyond the horizon: it is heard only when a meteor trail reflects it, as a short echo a few Hz to a few hundred Hz away from the line. Echoes closer than 18 Hz to the line are masked."));
    ImGui::PopTextWrapPos();
    smart5MeteorCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Beacon"), !on ? 0 : a.rx.mscatter.state >= 1 ? 1 : 2); flowNext(10 * gUi);
    lamp(tr("Ping"), !on ? 0 : a.rx.mscatter.active ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::msSummary(a.rx.mscatter).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Meteor scatter";
    if (live(a)) l2 = dect2::msSummary(a.rx.mscatter);
}

void synth(App& a, bool& changed) {
    static const char* names[] = {"A ping every few seconds", "About one ping a minute", "Beacon only, no pings"};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 2) o = 0;
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##mssynth", names[o])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], i == o)) { o = i; changed = true; }
        ImGui::EndCombo();
    }
    bool b = a.tune.synth.modeOpt[1] == 1;
    if (ImGui::Checkbox(tr("with interference bursts"), &b)) { a.tune.synth.modeOpt[1] = b ? 1 : 0; changed = true; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal is a beacon 420 Hz off the centre with simulated meteor echoes: short underdense ones and a few long overdense ones."));
    ImGui::PopTextWrapPos();
}

} // namespace

extern const ModeUi kMscatterUi;
const ModeUi kMscatterUi = {
    .sideTitle = "PINGS",
    .tabName = "Meteors",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};
