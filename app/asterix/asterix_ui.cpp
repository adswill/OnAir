// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ASTERIX screens: the track table, the map (the shared slippy map of ADS-B), the data sources and the record viewer, and the input controls (UDP port,
// multicast group, a file, demo traffic, the radar's position). The data comes from a network or a file, not from the radio: the receiver (in the engine
// object) runs by itself. Receive only; the UDP port is closed until Listen is pressed and listens on this computer only unless the box is ticked.
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/asterix/asterix_engine.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    int view = 0;                          // 0 map, 1 tracks, 2 sources, 3 records
    int port = 8600;
    bool lan = false;
    char group[40] = "";
    double lat = 25.25, lon = 55.36;       // the radar / system centre
    bool haveRef = false;
    double pushedLat = 1e9, pushedLon = 1e9;
    bool pushedValid = false;
    char posText[64] = "";
    uint64_t sel = 0;
    bool showCat[3] = {true, true, true};  // 021, 048, 062
    bool trails = true;
    bool labels = true;
    adsbmap::View map;
    std::string msg;
    dect2::AsterixTelemetry tel;
    uint64_t seq = 0;
    bool demo = false;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.port = (int)d.getI("asterixPort", 8600);
    S.lan = d.getI("asterixLan", 0) != 0;
    snprintf(S.group, sizeof S.group, "%s", d.getS("asterixGroup", "").c_str());
    if (d.has("adsbLat") && d.has("adsbLon")) { S.lat = d.getD("adsbLat", S.lat); S.lon = d.getD("adsbLon", S.lon); S.haveRef = true; }
    S.map.zoom = (int)d.getI("asterixZoom", 7);
    S.map.lat = S.lat; S.map.lon = S.lon;
    snprintf(S.posText, sizeof S.posText, "%.5f, %.5f", S.lat, S.lon);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

bool catShown(int cat) { return cat == 21 ? S.showCat[0] : cat == 48 ? S.showCat[1] : S.showCat[2]; }
ImU32 catColour(int cat, double age) {
    const ImU32 c = cat == 48 ? IM_COL32(110, 170, 240, 255) : cat == 21 ? IM_COL32(236, 238, 240, 255) : IM_COL32(240, 190, 90, 255);
    return age > 30 ? adsbmap::acStale() : c;
}
std::string label(const dect2::AsterixTrack& t) {
    if (!t.callsign.empty()) return t.callsign;
    if (t.address >= 0) return dect2::asterix::addressText(t.address);
    char b[24]; snprintf(b, sizeof b, "trk %d", t.trackNo); return b;
}
std::string catText(int cat) { char b[12]; snprintf(b, sizeof b, "%03d", cat); return b; }
std::string posText(const dect2::AsterixTrack& t) {
    if (!t.hasPos) return "-";
    char b[48]; snprintf(b, sizeof b, "%.4f %.4f%s", t.lat, t.lon, t.posReported ? "" : " ~");
    return b;
}

void setPosition(App& a, double lat, double lon) {
    S.lat = std::max(-90.0, std::min(90.0, lat)); S.lon = std::max(-180.0, std::min(180.0, lon));
    S.haveRef = true;
    plat::prefs().setD("adsbLat", S.lat); plat::prefs().setD("adsbLon", S.lon);
    snprintf(S.posText, sizeof S.posText, "%.5f, %.5f", S.lat, S.lon);
    savePrefs(a);
}

void controls(App& a) {
    dect2::AsterixReceiver& rx = a.engine.asterix();
    const dect2::AsterixTelemetry& t = S.tel;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("UDP port"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputInt("##astport", &S.port, 0, 0);
    S.port = std::max(1, std::min(65535, S.port));
    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputTextWithHint("##astgrp", "multicast group", S.group, sizeof S.group);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional: a multicast group address (224.0.0.0 to 239.255.255.255) to join, as radars and ground stations often send to one.\nLeave empty for a unicast port.");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::Checkbox("let other computers send", &S.lan);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the port listens on this computer only (loopback). On: it listens on every network interface, so other computers can send data to it.\nA multicast group needs the second.");
    ImGui::SameLine(0, 8 * gUi);
    if (!t.listening) {
        if (ImGui::Button("Listen")) {
            std::string err;
            if (rx.startUdp(S.port, S.lan, S.group, err)) {
                S.msg = "listening";
                plat::Prefs& d = plat::prefs(); d.setI("asterixPort", S.port); d.setI("asterixLan", S.lan ? 1 : 0); d.setS("asterixGroup", S.group);
                savePrefs(a);
            } else S.msg = err;
        }
    } else if (ImGui::Button("Stop")) { rx.stopUdp(); S.msg = "stopped"; }
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Button("Open file...")) {
        const std::string p = openFileDialog();
        if (!p.empty()) { std::string err; S.msg = rx.loadFile(p, err) ? "reading " + p : err; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A recording: ASTERIX data blocks one after another, or a classic pcap capture (Ethernet, IPv4, UDP). pcapng: save as pcap first.");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox("demo traffic", &S.demo)) rx.setDemo(S.demo);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Made-up aircraft in all three categories (a radar, an ADS-B ground station and a system track server), sent through the same decoder.");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Button("Clear")) { rx.clear(); S.sel = 0; }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("radar / system centre"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(200 * gUi, ImGui::GetContentRegionAvail().x));
    const bool enter = ImGui::InputText("##astpos", S.posText, sizeof S.posText, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 5 * gUi);
    if (ImGui::Button("Set") || enter) {
        double la, lo;
        if (parseCoordinates(S.posText, la, lo)) { setPosition(a, la, lo); S.map.lat = la; S.map.lon = lo; S.msg = "position set"; }
        else if (fromMaidenhead(S.posText, la, lo)) { setPosition(a, la, lo); S.map.lat = la; S.map.lon = lo; S.msg = "position set from the locator"; }
        else S.msg = "not a position: try 54.687, 25.280 or a locator such as KO24";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the radar (CAT 048: range and azimuth) or the system's centre (Cartesian positions) is. Records that carry a WGS-84 position do not need it.\nThe same setting as the ADS-B screen's position. Positions worked out from it are approximate (marked ~).");
    if (!S.msg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.msg.c_str()); }
    if (!t.error.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%s", t.error.c_str()); }
}

void mapView(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    const float h = std::max(120.f, ImGui::GetContentRegionAvail().y - 4 * gUi);
    const ImVec2 size(ImGui::GetContentRegionAvail().x, h);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (S.haveRef) {
        const ImVec2 q = adsbmap::project(S.lat, S.lon);
        dl->AddCircleFilled(q, 5 * gUi, IM_COL32(240, 240, 240, 255));
        dl->AddCircle(q, 8 * gUi, IM_COL32(20, 20, 20, 255), 0, 1.5f);
    }
    ImVec2 click;
    const bool clicked = adsbmap::clickedAt(click);
    float bestD = 14 * gUi;
    int drawn = 0;
    for (const auto& k : t.tracks) {
        if (!k.hasPos || !catShown(k.cat)) continue;
        drawn++;
        const ImVec2 q = adsbmap::project(k.lat, k.lon);
        const bool sel = S.sel == k.key;
        const ImU32 col = catColour(k.cat, k.ageS);
        if (S.trails && k.trail.size() > 1) {
            ImVec2 prev = adsbmap::project(k.trail[0].first, k.trail[0].second);
            for (size_t i = 1; i < k.trail.size(); i++) {
                const ImVec2 p = adsbmap::project(k.trail[i].first, k.trail[i].second);
                dl->AddLine(prev, p, (col & 0x00FFFFFF) | 0x90000000, 1.5f);
                prev = p;
            }
        }
        if (k.hasTrack) {
            const float r = (sel ? 12.f : 10.f) * gUi, hd = (float)k.trackDeg * 3.14159265f / 180.f, ch = std::cos(hd), sh = std::sin(hd);
            auto gv = [&](float x, float y) { return ImVec2(q.x + (x * ch - y * sh) * r, q.y + (x * sh + y * ch) * r); };
            ImVec2 g[3] = {gv(0, -1), gv(0.7f, 0.8f), gv(-0.7f, 0.8f)};
            dl->AddTriangleFilled(g[0], g[1], g[2], col);
            dl->AddTriangle(g[0], g[1], g[2], sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), sel ? 1.6f : 1.f);
        } else {
            dl->AddCircleFilled(q, (sel ? 6.f : 4.5f) * gUi, col);
            dl->AddCircle(q, (sel ? 6.f : 4.5f) * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), 0, 1.2f);
        }
        if (S.labels && adsbmap::labelsAt(S.map.zoom, false)) {
            char b[96];
            if (k.hasLevel) snprintf(b, sizeof b, "%s  FL%03.0f", label(k).c_str(), k.flightLevel); else snprintf(b, sizeof b, "%s", label(k).c_str());
            dl->AddText(ImVec2(q.x + 9 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), b);
        }
        if (clicked) { const float dd = std::hypot(click.x - q.x, click.y - q.y); if (dd < bestD) { bestD = dd; S.sel = k.key; } }
    }
    dl->PopClipRect();
    adsbmap::legend(size.x, "%d of %zu tracks on the map   |   blue: radar plots (048)   white: ADS-B (021)   amber: system tracks (062)   ~ = from range and azimuth", drawn, t.tracks.size());
    (void)a;
}

void tableView(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    if (ImGui::BeginTable("##astt", 13, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Cat"); ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("Track"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Callsign");
        ImGui::TableSetupColumn("Squawk"); ImGui::TableSetupColumn("FL"); ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Course"); ImGui::TableSetupColumn("V/S");
        ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Age"); ImGui::TableSetupColumn("Reports");
        ImGui::TableHeadersRow();
        for (const auto& k : t.tracks) {
            if (!catShown(k.cat)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(catText(k.cat).c_str(), S.sel == k.key, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == k.key ? 0 : k.key;
            ImGui::TableNextColumn(); ImGui::Text("%d/%d", k.sac, k.sic);
            ImGui::TableNextColumn(); if (k.trackNo >= 0) ImGui::Text("%d", k.trackNo); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.address >= 0) ImGui::TextUnformatted(dect2::asterix::addressText(k.address).c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (!k.callsign.empty()) ImGui::TextUnformatted(k.callsign.c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (k.squawk >= 0) {
                const bool emerg = k.squawk == 7700 || k.squawk == 7600 || k.squawk == 7500;
                if (emerg) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.35f, 1));
                ImGui::TextUnformatted(dect2::asterix::squawkText(k.squawk).c_str());
                if (emerg) ImGui::PopStyleColor();
            } else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.hasLevel) ImGui::Text("%03.0f", k.flightLevel); else if (k.hasAltFt) ImGui::Text("%.0f ft", k.altFt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.hasGs) ImGui::Text("%.0f kt", k.gsKt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.hasTrack) ImGui::Text("%.0f", k.trackDeg); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.hasVrate) ImGui::Text("%+.0f", k.vrateFpm); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(posText(k).c_str());
            ImGui::TableNextColumn(); if (k.ageS > 0) ImGui::Text("%.0f s", k.ageS); else ImGui::TextDisabled("file");
            ImGui::TableNextColumn(); ImGui::Text("%u", k.reports);
        }
        ImGui::EndTable();
    }
    (void)a;
}

void sourcesView(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    if (ImGui::BeginTable("##asts", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("SAC/SIC"); ImGui::TableSetupColumn("CAT 021"); ImGui::TableSetupColumn("CAT 048"); ImGui::TableSetupColumn("CAT 062"); ImGui::TableSetupColumn("Records"); ImGui::TableSetupColumn("Last");
        ImGui::TableHeadersRow();
        for (const auto& s : t.sources) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d/%d", s.sac, s.sic);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.perCat[0]);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.perCat[1]);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.perCat[2]);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.reports);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s ago", s.ageS);
        }
        ImGui::EndTable();
    }
    if (t.sources.empty()) ImGui::TextDisabled("no data yet: press Listen and point a radar or a ground station at this computer, open a recording, or use the demo traffic");
    ImGui::Spacing();
    kvRow(a, "input", "%s", t.input.c_str());
    kvRow(a, "datagrams", "%llu, %llu bytes", (unsigned long long)t.datagrams, (unsigned long long)t.bytes);
    kvRow(a, "data blocks", "%u (CAT 021: %u, 048: %u, 062: %u, other categories: %u, bad length: %u)", t.stats.blocks, t.stats.perCat[21], t.stats.perCat[48], t.stats.perCat[62], t.stats.otherCategory, t.stats.badBlocks);
    kvRow(a, "records", "%u (%u cut at an item of unknown length)", t.stats.records, t.stats.partialRecords);
    kvRow(a, "without a track", "%llu records had neither a track number nor an address", (unsigned long long)t.noIdentity);
    if (t.fileFraction >= 0) kvRow(a, "file", "%.0f %% read", t.fileFraction * 100.0);
}

void recordsView(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    if (ImGui::BeginTable("##astr", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        for (auto it = t.recent.rbegin(); it != t.recent.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(it->c_str()); ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    if (t.recent.empty()) ImGui::TextDisabled("no record yet");
}

void tab(App& a) {
    loadState();
    subNav("astv", S.view, {"Map", "Tracks", "Sources", "Records"});
    ImGui::SameLine(0, 12 * gUi);
    ImGui::Checkbox("021", &S.showCat[0]); ImGui::SameLine(0, 4 * gUi);
    ImGui::Checkbox("048", &S.showCat[1]); ImGui::SameLine(0, 4 * gUi);
    ImGui::Checkbox("062", &S.showCat[2]);
    if (S.view == 0) {
        ImGui::SameLine(0, 10 * gUi); ImGui::Checkbox("trails", &S.trails);
        ImGui::SameLine(0, 4 * gUi); ImGui::Checkbox("labels", &S.labels);
        ImGui::SameLine(0, 12 * gUi); adsbmap::tileControls("never the position itself");
    }
    controls(a);
    switch (S.view) {
    case 0: mapView(a); break;
    case 1: tableView(a); break;
    case 2: sourcesView(a); break;
    default: recordsView(a); break;
    }
}

void list(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    if (ImGui::BeginTable("##astl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Cat"); ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("FL"); ImGui::TableSetupColumn("Age");
        for (const auto& k : t.tracks) {
            if (!catShown(k.cat)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(catText(k.cat).c_str(), S.sel == k.key, ImGuiSelectableFlags_SpanAllColumns)) S.sel = k.key;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(label(k).c_str());
            ImGui::TableNextColumn(); if (k.hasLevel) ImGui::Text("%03.0f", k.flightLevel); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.ageS > 0) ImGui::Text("%.0f", k.ageS); else ImGui::TextDisabled("f");
        }
        ImGui::EndTable();
    }
    if (t.tracks.empty()) ImGui::TextDisabled("no track yet");
    (void)a;
}

void receiver(App& a) {
    loadState();
    const dect2::AsterixTelemetry& t = S.tel;
    kvRow(a, "state", "%s", dect2::asterixSummary(t).c_str());
    kvRow(a, "input", "%s", t.input.c_str());
    kvRow(a, "records", "%u decoded, %u cut or bad", t.stats.records, t.stats.partialRecords + t.stats.badBlocks);
    const dect2::AsterixTrack* sel = nullptr;
    for (const auto& k : t.tracks) if (k.key == S.sel) sel = &k;
    if (sel) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Selected track");
        kvRow(a, "identity", "CAT %03d %d/%d, track %d, address %s, callsign %s", sel->cat, sel->sac, sel->sic, sel->trackNo, dect2::asterix::addressText(sel->address).c_str(), sel->callsign.c_str());
        kvRow(a, "squawk", "%s", dect2::asterix::squawkText(sel->squawk).c_str());
        kvRow(a, "position", "%s", posText(*sel).c_str());
        if (sel->hasLevel) kvRow(a, "level", "FL%03.0f", sel->flightLevel);
        if (sel->hasAltFt) kvRow(a, "altitude", "%.0f ft", sel->altFt);
        if (sel->hasGs) kvRow(a, "ground speed", "%.0f kt", sel->gsKt);
        if (sel->hasTrack) kvRow(a, "course", "%.1f deg", sel->trackDeg);
        if (sel->hasVrate) kvRow(a, "vertical rate", "%+.0f ft/min", sel->vrateFpm);
        if (!sel->kind.empty()) kvRow(a, "kind", "%s", sel->kind.c_str());
        if (!sel->dep.empty() || !sel->dst.empty()) kvRow(a, "flight plan", "%s to %s", sel->dep.c_str(), sel->dst.c_str());
        kvRow(a, "reports", "%u", sel->reports);
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("ASTERIX is EUROCONTROL's format for surveillance data. A radar's own pulses cannot be received by a software radio, so this mode reads the data a radar, "
                        "an ADS-B or multilateration ground station or a tracker sends on a network (CAT 048 monoradar target reports, CAT 021 ADS-B target reports, CAT 062 system tracks), "
                        "or a recording of it. Ask the operator before you listen to a live feed; the port is closed until you press Listen. Record layouts follow the public category descriptions "
                        "(CAT 021 edition 2.x, 048 and 062 edition 1.x); an item whose length is not known ends the decoding of that record. Nothing here was checked against a live radar.");
    ImGui::PopTextWrapPos();
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::SetNextItemWidth(80 * gUi);
    static float maxAge = 60.f;
    if (ImGui::InputFloat("drop live tracks after (s)", &maxAge, 0, 0, "%.0f")) a.engine.asterix().setMaxAge(std::max(0.f, maxAge));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A live track that has not been reported for this long is removed. 0 keeps them. Tracks read from a file are never removed.");
}

void synth(App& a, bool&) {
    (void)a;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("this mode needs no radio samples; the built-in source only keeps the engine running. Use the demo traffic switch for made-up data.");
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const dect2::AsterixTelemetry& t = S.tel;
    lamp("Input", t.listening || t.demo || t.fileFraction >= 0 ? 1 : 0); flowNext(10 * gUi);
    lamp("Tracks", t.tracks.empty() ? 0 : 1); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::asterixSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App&, std::string& l1, std::string& l2) {
    l1 = "ASTERIX";
    l2 = dect2::asterixSummary(S.tel);
}

void tick(App& a) {
    loadState();
    dect2::AsterixReceiver& rx = a.engine.asterix();
    dect2::AsterixTelemetry t;
    if (rx.telemetry(t, S.seq)) { S.seq = t.seq; S.tel = std::move(t); }
    const bool valid = S.haveRef || S.demo;
    if (S.pushedLat != S.lat || S.pushedLon != S.lon || S.pushedValid != valid) {
        S.pushedLat = S.lat; S.pushedLon = S.lon; S.pushedValid = valid;
        rx.setOrigin(S.lat, S.lon, valid);
    }
}

} // namespace

extern const ModeUi kAsterixUi;
const ModeUi kAsterixUi = {
    .sideTitle = "TRACKS",
    .tabName = "Surveillance",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
};
