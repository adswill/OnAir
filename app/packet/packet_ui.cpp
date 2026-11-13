// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / Packet screens: the stations heard (table and map, the ADS-B map underneath, as for AIS), the log of raw packets, the state lamps.
#include "app.h"
#include "winlink/winlink_ui.h"
#include "adsb/records_ui.h"
#include "ui/kit/export.h"
#include "ui/kit/mode_panel.h"
#include "ui/kit/kit.h"
#include "ui/kit/session_list.h"
#include "ui/kit/mode_frame.h"
#include "../adsb/adsb_map.h"
#include "packet_board_ui.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/packet/packet_beacons.h"
#include "dect2/packet/packet_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using dect2::PacketStation;

struct State {
    bool loaded = false;
    int view = 0;                    // 0 stations, 1 map, 2 packets
    std::string sel;                 // the selected station (callsign), empty = none
    adsbmap::View map;
    bool mapFollow = true;           // the map fits the stations until the user drags it
    bool logScroll = true;
    int pushedNarrow = -1;
    int tableScale = 1, pushedScale = -1;   // list sizes times 1 / 5 / 20 (pref packetTableScale)
    int fixBits = 0, emphasis = 0; bool audioIn = false;   // saved as packetFixBits, packetEmphasis, packetAudioIn (docs/PACKET.md)
    int pushedFix = -1, pushedEmph = -1, pushedAudioIn = -1;
    bool vote = true, pushedVote = true, pushedVoteInit = false;   // multi-demodulator vote, saved as packetVote
    bool listen = false; int pushedListen = -1; float pushedVol = -1; int pushedMute = -1;
    std::string pushedCall = "\x01";   // the callsign for "to me" (packet_board_ui.h)
};
State S;

bool live(const App& a) { return a.rx.standard == 25; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("packetZoom", 9);
    S.listen = plat::prefs().getB("packetListen", false);
    S.tableScale = (int)plat::prefs().getI("packetTableScale", 1); if (S.tableScale != 5 && S.tableScale != 20) S.tableScale = 1;
    S.fixBits = std::max(0, std::min(2, (int)plat::prefs().getI("packetFixBits", 0)));
    S.emphasis = std::max(0, std::min(2, (int)plat::prefs().getI("packetEmphasis", 0)));
    S.audioIn = plat::prefs().getB("packetAudioIn", false);
    S.vote = plat::prefs().getB("packetVote", true);
}

std::string ageText(double s) {
    char b[24];
    if (s < 0) s = 0;
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

std::string clockText(double s) {
    char b[24];
    const int t = (int)s;
    snprintf(b, sizeof b, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

std::string posText(const PacketStation& s) {
    if (!s.hasPos) return "-";
    char b[48];
    snprintf(b, sizeof b, "%.4f%c %.4f%c", std::fabs(s.lat), s.lat < 0 ? 'S' : 'N', std::fabs(s.lon), s.lon < 0 ? 'W' : 'E');
    return b;
}

// the common APRS symbols of the primary table; anything else shows its two characters
const char* symbolName(char table, char code) {
    if (table == '\\') return code == '#' ? "digipeater" : code == '_' ? "weather" : nullptr;
    switch (code) {
    case '>': return "car";
    case '-': return "house";
    case '#': return "digipeater";
    case '_': return "weather";
    case 'k': return "truck";
    case '<': return "motorcycle";
    case 'b': return "bicycle";
    case '[': return "person";
    case 'O': return "balloon";
    case 'Y': return "yacht";
    case 's': return "ship";
    case '^': return "aircraft";
    case '\'': return "small aircraft";
    case 'I': return "TCP/IP";
    case 'r': return "antenna";
    case 'a': return "ambulance";
    case 'f': return "fire truck";
    case 'u': return "truck";
    case 'v': return "van";
    case '`': return "dish";
    case '!': return "police";
    case '$': return "phone";
    case '&': return "gateway";
    case '/': return "dot";
    default: return nullptr;
    }
}

std::string symText(const PacketStation& s) {
    if (!s.symCode) return "-";
    char b[40];
    const char* n = symbolName(s.symTable, s.symCode);
    if (n) snprintf(b, sizeof b, "%c%c  %s", s.symTable ? s.symTable : '/', s.symCode, n);
    else snprintf(b, sizeof b, "%c%c", s.symTable ? s.symTable : '/', s.symCode);
    return b;
}

// information fields can hold control characters (Mic-E): show them as dots
std::string printable(const std::string& s) {
    std::string o = s;
    for (char& c : o) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '.';
    return o;
}

// the text cut to w pixels with two dots at the end (the whole text is in a tooltip)
std::string fitText(const std::string& s, float w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "..").c_str()).x > w) t.pop_back();
    return t + "..";
}

ImU32 stationColour(const PacketStation& s, float alpha = 1.f) {
    int r, g, b;
    if (s.isObject) { r = 240; g = 190; b = 80; }
    else if (s.symCode == '_') { r = 80; g = 205; b = 200; }
    else if (s.symCode == '#' || s.symCode == '&') { r = 225; g = 228; b = 232; }
    else if (s.hasSpeed && s.speedKnots > 0.5) { r = 110; g = 205; b = 120; }
    else { r = 100; g = 160; b = 240; }
    return IM_COL32(r, g, b, (int)(255 * alpha));
}

// ---------------------------------------------------------------- the stations table

void stationTable(App& a, bool compact) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const bool wide = wideActive(a);
    const int cols = (compact ? 2 : 5) + (wide && !compact ? 1 : 0);
    {
        static std::string note;
        resexp::Table x; x.cols = {"callsign", "type", "symbol", "lat", "lon", "alt_m", "course_deg", "speed_kt", "via", "frames", "comment"};
        for (const PacketStation& s : t.stations)
            x.add({s.call, s.isObject ? "object" : "station", std::string(1, s.symTable ? s.symTable : ' ') + std::string(1, s.symCode ? s.symCode : ' '),
                   s.hasPos ? resexp::num(s.lat, 6) : "", s.hasPos ? resexp::num(s.lon, 6) : "", s.hasAlt ? resexp::num(s.altM, 0) : "",
                   s.hasCourse ? std::to_string(s.courseDeg) : "", s.hasSpeed ? resexp::num(s.speedKnots, 0) : "", s.via, std::to_string(s.count), s.comment});
        kit::exportBar(x, "aprs", note, compact ? "pkxs" : "pkxf");
    }
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(compact ? "##pk_s" : "##pk_f", cols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthStretch);
    if (!compact) ImGui::TableSetupColumn("Symbol");
    if (wide && !compact) ImGui::TableSetupColumn("Frequency MHz");
    ImGui::TableSetupColumn("Last heard");
    if (!compact) { ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch); }
    ImGui::TableHeadersRow();
    for (const PacketStation& s : t.stations) {      // newest first, as the receiver orders them
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(s.call.c_str());
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 4 * gUi, c.y + ImGui::GetTextLineHeight() * 0.5f), 3.f * gUi, stationColour(s));
        ImGui::SetCursorScreenPos(ImVec2(c.x + 12 * gUi, c.y));
        if (ImGui::Selectable(s.call.c_str(), s.call == S.sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = s.call;
        ImGui::PopID();
        if (!compact) { ImGui::TableNextColumn(); ImGui::TextUnformatted(symText(s).c_str()); }
        if (wide && !compact) { ImGui::TableNextColumn(); wideCarrierCell(a, s.carrierId, s.carrierHz); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(t.timeSec - s.lastHeardSec).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(posText(s).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fitText(printable(s.comment), ImGui::GetContentRegionAvail().x).c_str());
            if (!s.comment.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", printable(s.comment).c_str());
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- the map

// who repeated what: digipeaters and IGates heard in the paths (packet_path.h)
void digiTable(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    if (t.digis.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No digipeater heard yet. Frames show them in their path once they repeat a packet.")); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##pk_d", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Digipeater")); ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("Stations")); ImGui::TableSetupColumn(tr("Range km"));
    ImGui::TableSetupColumn(tr("Last")); ImGui::TableSetupColumn(tr("Repeats")); ImGui::TableSetupColumn(tr("Notes"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const dect2::PacketDigi& d : t.digis) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.call.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)d.heard);
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)d.stations);
        ImGui::TableNextColumn(); if (d.rangeKm > 0) ImGui::Text("%.0f", d.rangeKm); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(t.timeSec - d.lastSec).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)d.copies);
        ImGui::TableNextColumn();
        std::string n;
        if (d.igate) n += std::string(tr("IGate")) + "  ";
        if (d.looping) n += std::string(tr("looping")) + "  ";
        if (d.silent) n += std::string(tr("silent")) + "  ";
        ImGui::TextUnformatted(n.c_str());
    }
    ImGui::EndTable();
}

void mapView(App& a, ImVec2 size) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the stations that have a position, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& v : t.stations) if (v.hasPos) { la0 = std::min(la0, v.lat); la1 = std::max(la1, v.lat); lo0 = std::min(lo0, v.lon); lo1 = std::max(lo1, v.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(0.005, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(12, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon; const int zoom0 = S.map.zoom;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0 || S.map.zoom != zoom0) S.mapFollow = false;   // panned or zoomed (the wheel at the centre moves nothing)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const PacketStation* hit = nullptr; float best = 16 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    if (on) {
        for (const auto& v : t.stations) {
            if (!v.hasPos) continue;
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
            const bool sel = v.call == S.sel;
            const float r = (sel ? 8.f : 7.f) * gUi * 0.7f;
            dl->AddCircleFilled(q, r, stationColour(v));
            dl->AddCircle(q, r, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), 16, sel ? 1.6f : 1.f);
            if (adsbmap::labelsAt(S.map.zoom, sel)) {
            const std::string label = v.hasWx && v.wx.any() ? v.call + "  " + dect2::aprsWxLabel(v.wx) : v.call;   // weather stations: temperature and wind
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            const ImVec2 lp(q.x + 12 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddRect(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), label.c_str());
            }
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &v; }
        }
    }
    if (clicked) S.sel = hit ? hit->call : std::string();
    dl->PopClipRect();
    if (on && !S.sel.empty()) {   // the station card: everything heard about the selected station
        for (const auto& v : t.stations) {
            if (v.call != S.sel) continue;
            char b[10][160];
            int n = 0;
            snprintf(b[n++], 160, "%s%s", v.call.c_str(), v.isObject ? "  (object)" : "");
            if (v.symTable && v.symCode) snprintf(b[n++], 160, "symbol %c%c   %u frames   last heard %.0f s ago", v.symTable, v.symCode, (unsigned)v.count, std::max(0.0, t.timeSec - v.lastHeardSec));
            if (v.hasPos) {
                snprintf(b[n++], 160, "%.5f, %.5f  (%s)", v.lat, v.lon, dect2::toMaidenhead(v.lat, v.lon, 6).c_str());
                double rl, ro, km, brg; receiverLocation(rl, ro); dect2::greatCircle(rl, ro, v.lat, v.lon, km, brg);
                snprintf(b[n++], 160, "%.1f km at %.0f deg from the receiver", km, brg);
            }
            if (v.hasAlt || v.hasSpeed || v.hasCourse) {
                std::string m;
                if (v.hasAlt) m += "altitude " + std::to_string((int)v.altM) + " m   ";
                if (v.hasSpeed) m += "speed " + std::to_string((int)v.speedKnots) + " kn   ";
                if (v.hasCourse) m += "course " + std::to_string(v.courseDeg) + " deg";
                snprintf(b[n++], 160, "%s", m.c_str());
            }
            if (!v.via.empty()) snprintf(b[n++], 160, "sent by %s", v.via.c_str());
            if (v.carrierHz > 0) snprintf(b[n++], 160, "carrier %.4f MHz", v.carrierHz / 1e6);
            if (v.hasWx) {
                const dect2::AprsWx& x = v.wx;
                std::string m;
                char vb[48];
                if (!std::isnan(x.tempC)) { snprintf(vb, sizeof vb, "%.1f C   ", x.tempC); m += vb; }
                if (!std::isnan(x.windKmh)) { snprintf(vb, sizeof vb, "wind %s %.0f km/h   ", dect2::aprsCompass(x.windDirDeg), x.windKmh); m += vb; }
                if (!std::isnan(x.gustKmh)) { snprintf(vb, sizeof vb, "gust %.0f   ", x.gustKmh); m += vb; }
                if (!m.empty()) snprintf(b[n++], 160, "%s", m.c_str());
                m.clear();
                if (!std::isnan(x.humidityPct)) { snprintf(vb, sizeof vb, "humidity %.0f %%   ", x.humidityPct); m += vb; }
                if (!std::isnan(x.pressureHpa)) { snprintf(vb, sizeof vb, "%.1f hPa   ", x.pressureHpa); m += vb; }
                if (!std::isnan(x.rain24hMm)) { snprintf(vb, sizeof vb, "rain 24 h %.1f mm", x.rain24hMm); m += vb; }
                if (!m.empty()) snprintf(b[n++], 160, "%s", m.c_str());
            }
            if (!v.comment.empty() && n < 10) snprintf(b[n++], 160, "%s", v.comment.c_str());
            float w = 0;
            for (int i = 0; i < n; i++) w = std::max(w, ImGui::CalcTextSize(b[i]).x);
            const float lh = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 c0(p0.x + size.x - w - 24 * gUi, p0.y + 8 * gUi);
            if (c0.x > p0.x + 80 * gUi) {
                dl->AddRectFilled(c0, ImVec2(c0.x + w + 16 * gUi, c0.y + n * lh + 10 * gUi), IM_COL32(10, 12, 14, 225), 4.f);
                dl->AddRect(c0, ImVec2(c0.x + w + 16 * gUi, c0.y + n * lh + 10 * gUi), IM_COL32(90, 96, 104, 255), 4.f);
                for (int i = 0; i < n; i++) dl->AddText(ImVec2(c0.x + 8 * gUi, c0.y + 5 * gUi + i * lh), i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 210, 215, 255), b[i]);
            }
        }
    }
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
    plat::prefs().setI("packetZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& v : t.stations) if (v.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no positions yet");
    else adsbmap::legend(size.x, "%d of %zu stations with a position", withPos, t.stations.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- the packet log

std::string frameLine(const dect2::PacketFrameInfo& f) {
    std::string s = f.from + ">" + f.to;
    if (!f.path.empty()) s += "," + f.path;
    return s;
}

void packetLog(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    if (ImGui::SmallButton("copy all")) {
        std::string all;
        for (const auto& f : t.frames) { if (wideActive(a) && f.carrierId > 0) { char fb[24]; snprintf(fb, sizeof fb, "%.4f  ", radioCenterMhz(a) + f.carrierHz / 1e6); all += fb; } all += clockText(f.timeSec) + "  " + frameLine(f) + ":" + printable(f.info) + "\n"; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("follow", &S.logScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("the last %zu good packets", t.frames.size());
    ImGui::BeginChild("##pk_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const bool wide = wideActive(a);
    if (ImGui::BeginTable("##pk_t", 3 + (wide ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        if (wide) ImGui::TableSetupColumn("Frequency MHz");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("From > To,Path");
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (const auto& f : t.frames) {
            ImGui::TableNextRow();
            if (wide) { ImGui::TableNextColumn(); wideCarrierCell(a, f.carrierId, f.carrierHz); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(f.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(frameLine(f).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d baud, %s", f.baud, f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(printable(f.info).c_str()); ImGui::PopTextWrapPos();
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (t.frames.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing decoded yet"); ImGui::PopTextWrapPos(); }
    if (S.logScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- lamps and the hooks

int speedLamp(const dect2::PacketTelemetry& t, double last, uint64_t ok, uint64_t bad) {
    if (ok && t.timeSec - last < 60) return 1;       // frames in the last minute
    if (ok || bad) return 2;                         // heard before, or only damaged frames
    return 0;
}

void lamps(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const bool on = live(a);
    lamp("Signal", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(10 * gUi);
    lamp("1200", !on ? 0 : speedLamp(t, t.last1200Sec, t.ok1200, t.bad1200)); flowNext(10 * gUi);
    lamp("9600", !on ? 0 : speedLamp(t, t.last9600Sec, t.ok9600, t.bad9600)); flowNext(10 * gUi);
    lamp("APRS", !on ? 0 : t.aprsFrames && t.timeSec - t.lastAprsSec < 60 ? 1 : t.aprsFrames ? 2 : 0);
}

void tick(App& a) {
    loadState();
    winlinkTick(a);   // Winlink email (app/winlink): its session runs while the screen is open
    if (!a.engine.running()) return;
    if (live(a)) {   // the records board: the farthest APRS station with a position (direct or through a digipeater alike; objects left out)
        static double last = 0;
        if (ImGui::GetTime() - last > 5) { last = ImGui::GetTime(); for (const auto& s : a.rx.packet.stations) if (s.hasPos && s.via.empty()) recordsOffer("APRS", "", recordsDistKm(s.lat, s.lon), s.call, ""); }   // stations, not objects
    }
    dect2::PacketReceiver& r = a.engine.packet();
    if ((int)a.packetNarrow != S.pushedNarrow) { r.setNarrowChannel(a.packetNarrow); S.pushedNarrow = a.packetNarrow; }
    if (S.tableScale != S.pushedScale) { r.setTableScale(S.tableScale); S.pushedScale = S.tableScale; }
    if ((int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    { const std::string c = aprsboard::myCall(a); if (c != S.pushedCall) { r.setMyCall(c); S.pushedCall = c; } }
    if (!S.pushedVoteInit || S.vote != S.pushedVote) { r.setVote(S.vote); S.pushedVote = S.vote; S.pushedVoteInit = true; }
    if (S.fixBits != S.pushedFix) { r.setFixBits(S.fixBits); S.pushedFix = S.fixBits; }
    if (S.emphasis != S.pushedEmph) { r.setEmphasis(S.emphasis); S.pushedEmph = S.emphasis; }
    if ((int)S.audioIn != S.pushedAudioIn) { r.setAudioInput(S.audioIn); S.pushedAudioIn = S.audioIn; }
    if (a.volume != S.pushedVol || (int)a.muted != S.pushedMute) {
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
}

void controls(App& a) {
    listenControls(a, S.listen, "packetListen", "pkt", "Hear the channel: the demodulated audio, the AFSK tones of 1200 baud packet (1200 / 2200 Hz); decoding is the same either way");
    ImGui::TextDisabled("%s", tr("Channel")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::BeginCombo("##pktbw", a.packetNarrow ? tr("12.5 kHz") : tr("25 kHz"))) {
        if (ImGui::Selectable(tr("25 kHz"), !a.packetNarrow)) { a.packetNarrow = false; savePrefs(a); }
        if (ImGui::Selectable(tr("12.5 kHz"), a.packetNarrow)) { a.packetNarrow = true; savePrefs(a); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Receive filter width. 12.5 kHz cuts a neighbouring channel better (often best for APRS) but needs the signal on the channel centre; 25 kHz is the wider, more forgiving filter."));
    flowNext(10 * gUi);
    {
        static const char* kSz[] = {"standard", "5 times", "20 times"};
        const int cur = S.tableScale == 20 ? 2 : S.tableScale == 5 ? 1 : 0;
        ImGui::TextDisabled("%s", tr("Lists")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(95 * gUi);
        if (ImGui::BeginCombo("##pktsz", kSz[cur])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(kSz[i], cur == i)) { S.tableScale = i == 2 ? 20 : i == 1 ? 5 : 1; plat::prefs().setI("packetTableScale", S.tableScale); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many frames (200) and stations (300) are kept before the oldest leave. Larger lists suit a full day of monitoring; they use more memory."));
    }
    flowNext(10 * gUi);
    {
        static const char* kEmph[] = {"Auto", "De-emphasis", "Pre-emphasis"};
        ImGui::TextDisabled("%s", tr("Emphasis")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::BeginCombo("##pktemph", tr(kEmph[S.emphasis]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kEmph[i]), S.emphasis == i)) { S.emphasis = i; plat::prefs().setI("packetEmphasis", i); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            const dect2::PacketTelemetry& t = a.rx.packet;
            std::string tip = tr("1200 baud tone balance. Auto (best for most): the receiver tries several mark / space weights at once and covers +-6 dB. De-emphasis: for stations sending through a microphone input (pre-emphasised; the 2200 Hz tone louder). Pre-emphasis: for a sound card on a radio's speaker output (de-emphasised; the 1200 Hz tone louder).");
            if (t.twistFrames) {
                char b[160];
                snprintf(b, sizeof b, tr("\nMeasured on the last frames: 1200 Hz %+.1f dB against 2200 Hz."), t.twistDb);
                tip += b;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        flowNext(10 * gUi);
        static const char* kFix[] = {"Off", "1 bit", "2 bits"};
        ImGui::TextDisabled("%s", tr("Fix bits")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::BeginCombo("##pktfix", tr(kFix[S.fixBits]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kFix[i]), S.fixBits == i)) { S.fixBits = i; plat::prefs().setI("packetFixBits", i); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A frame that fails its check is tried again with one wrong bit (or two in a row) corrected. More frames from weak stations; a repaired frame is marked [1 bit fixed]. Off by default: a few repaired frames could be wrong."));
        flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("Vote"), &S.vote)) plat::prefs().setB("packetVote", S.vote);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Several demodulators run side by side. When none of them gets a frame right, their copies are voted bit by bit and the few bits they disagree on are tried until the check fits. More frames at the edge of reception; frames made this way are marked [repaired by vote]. On by default."));
        flowNext(10 * gUi);
        ImGui::TextDisabled("%s", tr("Input")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::BeginCombo("##pktin", S.audioIn ? tr("FM audio") : tr("Radio IQ"))) {
            if (ImGui::Selectable(tr("Radio IQ"), !S.audioIn)) { S.audioIn = false; plat::prefs().setB("packetAudioIn", false); }
            if (ImGui::Selectable(tr("FM audio"), S.audioIn)) { S.audioIn = true; plat::prefs().setB("packetAudioIn", true); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Radio IQ: an SDR (or a sound card carrying IQ). FM audio: a sound card on a radio's discriminator (9600 baud too) or speaker output (1200 baud), with the sound card chosen as the radio."));
        flowNext(10 * gUi);
    }
}

// Channel use (4.0): who fills the channel and how often each station beacons, from the frames heard in the last 10 minutes.
void channelUse(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const auto u = dect2::analyseChannel(t.frames, t.timeSec, 600);
    ImGui::TextDisabled(tr("Channel busy %.0f %% of the last 10 minutes by %d stations"), u.occupancyPct, (int)u.stations.size());
    if (u.stations.empty() || !ImGui::BeginTable("##pk_cu", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Frames")); ImGui::TableSetupColumn(tr("Median gap s")); ImGui::TableSetupColumn(tr("Shortest s"));
    ImGui::TableSetupColumn(tr("Airtime %")); ImGui::TableSetupColumn(tr("Same text")); ImGui::TableSetupColumn(tr("Notes"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < u.stations.size() && i < 12; i++) {
        const auto& s = u.stations[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.call.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", s.frames);
        ImGui::TableNextColumn(); if (s.frames > 1) ImGui::Text("%.0f", s.medianGapS); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (s.frames > 1) ImGui::Text("%.0f", s.minGapS); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.1f", s.sharePct);
        ImGui::TableNextColumn(); ImGui::Text("%d", s.repeats);
        ImGui::TableNextColumn();
        std::string n;
        if (s.fast) n += std::string(tr("beacons fast")) + "  ";
        if (s.hog) n += std::string(tr("big share")) + "  ";
        if (s.repeat) n += std::string(tr("same text again and again")) + "  ";
        ImGui::TextUnformatted(n.c_str());
    }
    ImGui::EndTable();
}

void tab(App& a) {
    loadState();
    lamps(a);
    flowNext(10 * gUi);
    controls(a);
    kit::modeHeader({24, "APRS / Packet", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    {
        const int unread = aprsboard::unreadToMe(a);
        char ml[48]; snprintf(ml, sizeof ml, unread ? "%s (%d)" : "%s", tr("Messages"), unread);
        static std::string msgLabel; msgLabel = ml;
        kit::modeNav("pktv", S.view, {{tr("Live"), 0}, {tr("Tracks"), 3}, {tr("Map"), 1}, {tr("Weather"), 4}, {msgLabel.c_str(), 5}, {tr("Log"), 2, true}, {tr("Winlink"), 6}});
    }
    if (S.view == 6) { winlinkPanel(a, 0); return; }
    if (S.view == 2) { packetLog(a); return; }
    if (S.view == 4) { aprsboard::weatherView(a, S.sel); return; }
    if (S.view == 5) { aprsboard::messagesView(a); return; }
    if (S.view == 3) { kit::sessionItems(24, 3, "aprs"); return; }
    if (S.view == 1) { mapView(a, ImGui::GetContentRegionAvail()); return; }
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver"); ImGui::PopTextWrapPos(); return; }
    if (ImGui::TreeNode(trw("Channel use"))) { channelUse(a); ImGui::TreePop(); }
    stationTable(a, false);
}

void list(App& a) {
    loadState();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see stations"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("packet")) a.engine.clearResults(26);   // also the whole-span receivers
    ImGui::TextDisabled("%zu stations", a.rx.packet.stations.size());
    stationTable(a, true);
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::PacketTelemetry& t = a.rx.packet;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 0 ? "no signal" : t.state == 1 ? "signal, no good frame" : "decoding");
    kv("1200 baud", "%llu good, %llu failed the check", (unsigned long long)t.ok1200, (unsigned long long)t.bad1200);
    kv("9600 baud", "%llu good, %llu failed the check", (unsigned long long)t.ok9600, (unsigned long long)t.bad9600);
    kv("APRS packets", "%llu of %llu good frames", (unsigned long long)t.aprsFrames, (unsigned long long)t.blocksOk);
    kv("signal", "%.1f dB over the noise, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("stations", "%zu in the table", t.stations.size());
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::PacketTelemetry& t = a.rx.packet;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(10 * gUi);
    lamp("1200", !on ? 0 : speedLamp(t, t.last1200Sec, t.ok1200, t.bad1200)); flowNext(10 * gUi);
    lamp("9600", !on ? 0 : speedLamp(t, t.last9600Sec, t.ok9600, t.bad9600)); flowNext(10 * gUi);
    lamp("APRS", !on ? 0 : t.aprsFrames && t.timeSec - t.lastAprsSec < 60 ? 1 : t.aprsFrames ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Frames"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.blocksOk); else ImGui::TextUnformatted(run ? "starting" : "stopped");
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "APRS / Packet";
    if (live(a)) l2 = dect2::packetSummary(a.rx.packet);
}

} // namespace

// Simple view: the stations, the map and the frame log side by side; the channel options in a closed section above them
static void command(App& a) {
    loadState();
    lamps(a);
    flowNext(10 * gUi);
    kit::modeHeader({24, "APRS / Packet", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    if (kit::collapsingSection(tr("Channel options"), false)) controls(a);
    modepanel::panes({{"##pktcst", tr("Stations"), [&a] { if (live(a)) stationTable(a, false); else ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver"); }, 1.2f, 240.f},
                      {"##pktcmap", tr("Map"), [&a] { mapView(a, ImGui::GetContentRegionAvail()); }, 1.2f, 260.f},
                      {"##pktcdg", tr("Digipeaters"), [&a] { if (live(a)) digiTable(a); else ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver"); }, 1.f, 220.f},
                      {"##pktclog", tr("Log"), [&a] { packetLog(a); }, 1.f, 220.f}}, 320.f);
}

extern const ModeUi kPacketUi;
const ModeUi kPacketUi = {
    .sideTitle = "STATIONS",
    .tabName = "Packets",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
    .command = command,
};
