// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME screens: the weather channels (WX1 - WX7), the alert history with every field in plain English (event, originator, the
// locations with the partial-county direction, purge and issue time, call sign, how many of the three repeats agreed), the 1050 Hz attention
// tone, a map of the states and provinces named (an approximation: a marker at the centre of the state, nothing more precise) and the raw
// messages. County names come only from a table the user loads (docs/same.md). Receive only: the text of an alert is a public broadcast.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/same/same_proto.h"
#include "dect2/same/same_rx.h"
#include "dect2/same/same_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using dect2::SameAlert;

struct State {
    bool loaded = false, wasRunning = false;
    int view = 0;                          // 0 alerts, 1 map, 2 raw messages
    int country = 0;
    int64_t selWall = 0; double selSec = -1;   // the selected alert (decode time and signal time); none: the newest
    adsbmap::View map;
    bool mapFollow = true;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; int pushedMute = -1, pushedCountry = -1;
    char countyPath[400] = "";
    std::string countyMsg;
    bool countyTried = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 77; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("sameZoom", 4);
    S.map.lat = 39.0; S.map.lon = -96.0;
    S.listen = plat::prefs().getB("sameListen", false);
    S.country = (int)plat::prefs().getI("sameCountry", 0);
    const std::string p = plat::prefs().getS("sameCounties", "");
    snprintf(S.countyPath, sizeof S.countyPath, "%s", p.c_str());
}

void loadCounties(bool announce) {
    if (!S.countyPath[0]) { S.countyMsg = announce ? "enter the path of a county CSV first" : ""; return; }
    std::string err;
    const size_t n = dect2::same::loadCountyFile(S.countyPath, &err);
    if (n) { S.countyMsg = std::to_string(n) + " counties loaded"; plat::prefs().setS("sameCounties", S.countyPath); }
    else S.countyMsg = err;
}

std::string clockText(int64_t wall) {
    return dect2::timefmt::clock((int64_t)wall);
}

std::string dateClock(int64_t wall) {
    char b[40];
    const time_t t = (time_t)wall;
    struct tm g {};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    strftime(b, sizeof b, "%b %d %H:%M:%S UTC", &g);
    return b;
}

// valid alerts are "current" for the purge time after they were received (the computer clock, not the sender's)
bool current(const SameAlert& a) { return a.valid && !a.eom && (int64_t)std::time(nullptr) - a.wall < (int64_t)a.msg.purgeMin * 60; }

ImVec4 kindColour(const std::string& k) {
    if (k == "warning") return ImVec4(0.95f, 0.40f, 0.36f, 1);
    if (k == "emergency") return ImVec4(0.92f, 0.45f, 0.82f, 1);
    if (k == "watch") return ImVec4(0.95f, 0.76f, 0.30f, 1);
    if (k == "statement") return ImVec4(0.55f, 0.76f, 0.95f, 1);
    if (k == "test") return ImVec4(0.62f, 0.68f, 0.72f, 1);
    return ImVec4(0.78f, 0.84f, 0.88f, 1);
}

std::string eventTitle(const SameAlert& a) {
    if (a.eom) return "End of message";
    if (!a.valid) return "Damaged message";
    return (a.msg.eventName.empty() ? "Unknown event" : a.msg.eventName) + " (" + a.msg.event + ")";
}

std::string areasText(const SameAlert& a, size_t maxLocs = 2) {
    if (!a.valid || a.eom) return "";
    std::string s;
    for (size_t i = 0; i < a.msg.locs.size() && i < maxLocs; i++) {
        if (i) s += "; ";
        s += dect2::same::locate(a.msg.locs[i].code, (dect2::same::Country)S.country).text;
    }
    if (a.msg.locs.size() > maxLocs) s += "; +" + std::to_string(a.msg.locs.size() - maxLocs) + " more";
    return s;
}

std::string fitText(const std::string& s, float w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "..").c_str()).x > w) t.pop_back();
    return t + "..";
}

const SameAlert* selected(const dect2::SameTelemetry& t) {
    for (const auto& a : t.alerts) if (a.wall == S.selWall && std::fabs(a.timeSec - S.selSec) < 1e-6) return &a;
    for (const auto& a : t.alerts) if (!a.eom) return &a;   // the newest alert
    return t.alerts.empty() ? nullptr : &t.alerts.front();
}

// ---------------------------------------------------------------- the alert list and its details

void details(App& a, const SameAlert& al) {
    const auto kv = [&](const char* k, const char* fmt, auto... v) {
        ImGui::TextDisabled("%s", k); kvColumn(120 * gUi);
        ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos();
    };
    ImGui::TextColored(al.valid ? kindColour(al.msg.eventKind) : pal::badRed(), "%s", eventTitle(al).c_str());
    if (al.valid && !al.eom) {
        const auto& m = al.msg;
        kv("from", "%s%s%s", m.org.c_str(), m.orgName.empty() ? "" : " - ", m.orgName.c_str());
        kv("sender", "%s", m.callsign.c_str());
        kv("received", "%s", dateClock(al.wall).c_str());
        const std::string it = dect2::same::issueText(al.year, m.issueDay, m.issueHour, m.issueMin);
        kv("issued", "day %03d, %02d:%02d UTC%s%s%s", m.issueDay, m.issueHour, m.issueMin, it.empty() ? "" : " (", it.c_str(), it.empty() ? "" : " this year)");
        kv("valid for", "%s", dect2::same::purgeText(m.purgeMin).c_str());
        kv("repeats", "%d of %d identical%s", al.agreeing, al.bursts, al.bitsFixed ? (std::string(", ") + std::to_string(al.bitsFixed) + " bit errors voted away").c_str() : "");
        kv("attention tone", "%s", al.toneSec > 0 ? (std::to_string((int)std::lround(al.toneSec * 10) / 10.0).substr(0, 4) + " s of 1050 Hz").c_str() : "none heard");
        ImGui::Spacing();
        ImGui::TextDisabled("Locations (%zu)", m.locs.size());
        if (ImGui::BeginTable("##same_loc", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Code"); ImGui::TableSetupColumn("Part"); ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& l0 : m.locs) {
                const dect2::same::Location l = dect2::same::locate(l0.code, (dect2::same::Country)S.country);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.code.c_str()); ImGui::PopFont();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("P = %d (partial county digit), SS = %02d (state), CCC = %03d (county)", l.pca, l.state, l.county);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.pca == 0 ? "whole" : l.pcaName.c_str());
                ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(l.text.c_str()); ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
    } else if (!al.valid) {
        kv("why", "%s", al.err.c_str());
        kv("received", "%s", dateClock(al.wall).c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("as sent");
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(al.raw.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    if (ImGui::SmallButton("copy##samecopy")) ImGui::SetClipboardText(al.raw.c_str());
}

void alertsView(App& a) {
    const dect2::SameTelemetry& t = a.rx.same;
    const float listH = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y * 0.45f);
    if (ImGui::BeginTable("##same_t", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("Areas", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Valid"); ImGui::TableSetupColumn("Repeats");
        ImGui::TableHeadersRow();
        const SameAlert* sel = selected(t);
        for (const SameAlert& al : t.alerts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)(al.wall * 31 + (int64_t)(al.timeSec * 10)));
            if (ImGui::Selectable(clockText(al.wall).c_str(), &al == sel, ImGuiSelectableFlags_SpanAllColumns)) { S.selWall = al.wall; S.selSec = al.timeSec; }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (al.eom) ImGui::TextDisabled("%s", eventTitle(al).c_str());
            else ImGui::TextColored(al.valid ? kindColour(al.msg.eventKind) : pal::badRed(), "%s", fitText(eventTitle(al), ImGui::GetContentRegionAvail().x).c_str());
            ImGui::TableNextColumn(); if (al.valid && !al.eom) ImGui::TextUnformatted(al.msg.org.c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fitText(areasText(al, 1), ImGui::GetContentRegionAvail().x).c_str());
            ImGui::TableNextColumn();
            if (al.valid && !al.eom) { if (current(al)) ImGui::TextColored(pal::okGreen(), "now"); else ImGui::TextDisabled("%s", dect2::same::purgeText(al.msg.purgeMin).c_str()); } else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", al.agreeing, al.bursts);
        }
        ImGui::EndTable();
    }
    if (t.alerts.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no alert received yet: the weather radio sends one only when something is issued, and a test on Wednesdays (the required weekly test)"); ImGui::PopTextWrapPos(); }
    ImGui::BeginChild("##same_d", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const SameAlert* s = selected(t)) details(a, *s);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the map (approximate)

struct Marker { double lat, lon; ImU32 col; bool cur; const SameAlert* al; std::string label; };

void mapView(App& a, ImVec2 size) {
    const dect2::SameTelemetry& t = a.rx.same;
    const bool on = live(a);
    std::vector<Marker> mk;
    if (on) {
        int n = 0;
        for (const SameAlert& al : t.alerts) {
            if (!al.valid || al.eom) continue;
            if (++n > 30) break;
            const ImVec4 c = kindColour(al.msg.eventKind);
            for (const auto& l0 : al.msg.locs) {
                const dect2::same::Location l = dect2::same::locate(l0.code, (dect2::same::Country)S.country);
                if (!l.hasPos) continue;
                // the partial county digit nudges the marker toward its direction (a hint of the side, not a position)
                static const int dx[10] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1}, dy[10] = {0, 1, 1, 1, 0, 0, 0, -1, -1, -1};
                Marker m;
                m.lat = l.lat + 0.6 * dy[l.pca]; m.lon = l.lon + 0.8 * dx[l.pca];
                m.col = ImGui::ColorConvertFloat4ToU32(c); m.cur = current(al); m.al = &al; m.label = al.msg.event;
                mk.push_back(m);
            }
        }
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && !mk.empty()) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        for (const auto& m : mk) { la0 = std::min(la0, m.lat); la1 = std::max(la1, m.lat); lo0 = std::min(lo0, m.lon); lo1 = std::max(lo1, m.lon); }
        S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
        const double span = std::max(6.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
        S.map.zoom = std::max(2, std::min(8, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon; const int zoom0 = S.map.zoom;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0 || S.map.zoom != zoom0) S.mapFollow = false;   // panned or zoomed (the wheel at the centre moves nothing)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    const SameAlert* hit = nullptr; float best = 18 * gUi;
    for (const auto& m : mk) {
        const ImVec2 q = adsbmap::project(m.lat, m.lon);
        if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
        const float r = 9.f * gUi;
        ImU32 fill = (m.col & 0x00FFFFFF) | ((m.cur ? 150u : 60u) << 24);
        dl->AddCircleFilled(q, r, fill);
        dl->AddCircle(q, r, m.col, 24, m.cur ? 2.f : 1.f);
        dl->AddText(ImVec2(q.x + r + 2 * gUi, q.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(235, 238, 240, 255), m.label.c_str());
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < best) { best = d; hit = m.al; }
    }
    if (hit) { S.selWall = hit->wall; S.selSec = hit->timeSec; }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("sameZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (mk.empty()) adsbmap::legend(size.x, "no alert with a state yet");
    else adsbmap::legend(size.x, "approximate: a marker is the middle of the state, moved toward the partial-county direction; not the area of the alert");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- the raw messages

void rawView(App& a) {
    const dect2::SameTelemetry& t = a.rx.same;
    if (ImGui::SmallButton("copy all##sameraw")) {
        std::string all;
        for (auto it = t.alerts.rbegin(); it != t.alerts.rend(); ++it) all += clockText(it->wall) + "  " + it->raw + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu messages, %llu bursts heard; each message is sent three times and voted bit by bit", t.alerts.size(), (unsigned long long)t.bursts);
    ImGui::BeginChild("##same_raw", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##same_rt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Repeats"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (const SameAlert& al : t.alerts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(al.wall).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d/%d", al.agreeing, al.bursts);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d bit%s changed by the vote, %.1f dB, carrier %+.0f Hz", al.bitsFixed, al.bitsFixed == 1 ? "" : "s", al.snrDb, al.cfoHz);
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0);
            if (al.valid) ImGui::TextUnformatted(al.raw.c_str()); else ImGui::TextColored(pal::badRed(), "%s  (%s)", al.raw.c_str(), al.err.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (t.alerts.empty()) ImGui::TextDisabled("nothing decoded yet");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- lamps and the hooks

void lamps(App& a) {
    const dect2::SameTelemetry& t = a.rx.same;
    const bool on = live(a);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("1050 Hz tone", !on ? 0 : t.toneNow ? 3 : t.timeSec - t.lastToneSec < 60 ? 2 : 0); flowNext(10 * gUi);
    bool cur = false;
    if (on) for (const auto& al : t.alerts) if (current(al)) cur = true;
    lamp("Alert", !on ? 0 : cur ? 3 : t.dataValid ? 1 : 0);
}

void channelButtons(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Channel"); flowNext(6 * gUi);
    int n = 0;
    const dect2::same::Channel* c = dect2::same::channels(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - c[i].mhz) < 0.0005;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[32]; snprintf(b, sizeof b, "%s##samech%d", c[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, c[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("NOAA Weather Radio channel %s, %.3f MHz (narrow FM)", c[i].name, c[i].mhz);
        flowNext(4 * gUi);
    }
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::SameReceiver& r = a.engine.same();
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    if (!S.wasRunning || S.country != S.pushedCountry) { r.setCountry(S.country); S.pushedCountry = S.country; }
    if (!S.wasRunning || a.volume != S.pushedVol || (int)a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    S.wasRunning = true;
}

void tuner(App& a, bool& retune) {
    load();
    int n = 0;
    const dect2::same::Channel* c = dect2::same::channels(n);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Channel");
    ImGui::SameLine(0, 5 * gUi);
    const char* cur = "other";
    for (int i = 0; i < n; i++) if (std::fabs(a.freqMhz - c[i].mhz) < 0.0005) cur = c[i].name;
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##samech", cur)) {
        for (int i = 0; i < n; i++) {
            char b[40]; snprintf(b, sizeof b, "%s  %.3f MHz", c[i].name, c[i].mhz);
            if (ImGui::Selectable(b, std::fabs(a.freqMhz - c[i].mhz) < 0.0005)) { tuneFreq(a, c[i].mhz); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The seven NOAA Weather Radio channels (US and Canada); each transmitter uses one of them");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Country");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    if (comboTr("##samecountry", &S.country, {N_("United States"), N_("Canada")})) { plat::prefs().setI("sameCountry", S.country); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the state part of a location code is named. The US FIPS state codes and Canada's province codes overlap (35 is New Mexico\n"
                                                  "or Ontario), and the message does not say which: choose the country of the transmitter you listen to.");
}

void tab(App& a) {
    load();
    lamps(a);
    flowNext(10 * gUi);
    channelButtons(a);
    flowNext(10 * gUi);
    listenControls(a, S.listen, "sameListen", "same", "Hear the channel: the voice, the 1050 Hz tone and the SAME data tones; decoding is the same either way");
    subNav("samev", S.view, {"Alerts", "Map", "Raw"});
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver on a weather radio channel (162.400 - 162.550 MHz)"); ImGui::PopTextWrapPos(); return; }
    if (S.view == 1) mapView(a, ImVec2(ImGui::GetContentRegionAvail().x, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 2 * ImGui::GetTextLineHeight())));
    else if (S.view == 2) rawView(a);
    else alertsView(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only. The text of an alert is a public broadcast; this is not an alerting system: do not rely on it for your safety.");
    ImGui::PopTextWrapPos();
}

void list(App& a) {
    load();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see alerts"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("same")) a.engine.same().clearResults();
    ImGui::TextDisabled("%zu messages", a.rx.same.alerts.size());
    if (!ImGui::BeginTable("##same_l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("UTC");
    ImGui::TableHeadersRow();
    const SameAlert* sel = selected(a.rx.same);
    for (const SameAlert& al : a.rx.same.alerts) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)(al.wall * 31 + (int64_t)(al.timeSec * 10)));
        if (ImGui::Selectable("##r", &al == sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { S.selWall = al.wall; S.selSec = al.timeSec; }
        ImGui::PopID();
        ImGui::SameLine(0, 0);
        if (al.eom) ImGui::TextDisabled("end");
        else ImGui::TextColored(al.valid ? kindColour(al.msg.eventKind) : pal::badRed(), "%s", al.valid ? al.msg.event.c_str() : "damaged");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", eventTitle(al).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(al.wall).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::SameTelemetry& t = a.rx.same;
    const auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 0 ? "no signal" : t.state == 1 ? "signal, no message lately" : "message decoded lately");
    kv("messages", "%llu decoded, %llu damaged", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("data bursts", "%llu heard (3 per message)", (unsigned long long)t.bursts);
    kv("alert tone", "%zu heard%s", t.tones.size(), t.toneNow ? ", on the air now" : "");
    kv("signal", "%.1f dB over the noise, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("country", "%s", S.country == 1 ? "Canada (province codes)" : "United States (state FIPS codes)");
    kv("county names", "%zu loaded%s", dect2::same::countyCount(), dect2::same::countyCount() ? "" : " (none built in: load a county CSV below)");
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("County CSV"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::max(120.f * gUi, ImGui::GetContentRegionAvail().x - 150 * gUi));
    ImGui::InputTextWithHint("##samecsv", "path of a county table (SSCCC,Name)", S.countyPath, sizeof S.countyPath);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines of 'SSCCC,Name', 'SS,CCC,Name' or the Census file national_county.txt. Counties are named only from this file:\nno county names are built in. See docs/same.md.");
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Load##samecsv")) loadCounties(true);
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Unload##samecsv")) { dect2::same::clearCounties(); S.countyMsg = "county names unloaded"; }
    if (!S.countyMsg.empty()) ImGui::TextDisabled("%s", S.countyMsg.c_str());
    if (!S.countyTried) { S.countyTried = true; if (S.countyPath[0]) loadCounties(false); }
    ImGui::Spacing();
    ImGui::TextDisabled("Attention tones");
    for (size_t i = 0; i < t.tones.size() && i < 8; i++) ImGui::Text("%s%s, %.1f s, %.0f dB", clockText(t.tones[i].wall).c_str(), dect2::timefmt::sfx(), t.tones[i].durSec, t.tones[i].levelDb);
    if (t.tones.empty()) ImGui::TextDisabled("none heard (the NOAA alert tone is 1050 Hz for 8 - 10 s after the header)");
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::SameTelemetry& t = a.rx.same;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("1050 Hz tone", !on ? 0 : t.toneNow ? 3 : t.timeSec - t.lastToneSec < 60 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Messages"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.blocksOk); else ImGui::TextUnformatted(run ? "starting" : "stopped");
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NOAA Weather Radio SAME";
    if (live(a)) l2 = dect2::sameSummary(a.rx.same);
}

} // namespace

extern const ModeUi kSameUi;
const ModeUi kSameUi = {
    .sideTitle = "ALERTS",
    .tabName = "Alerts",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
