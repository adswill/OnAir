// Inmarsat (all carriers) screens: Carriers (a strip of the whole capture and the table of every carrier found, click one to focus it), Messages
// (ACARS and ADS-C from all P channels, Inmarsat-C EGC), Aircraft (map and table from the position reports) and Voice (the C-channel calls, mute / solo,
// recording, the user-installed codec). One Auto button restarts the search; the CPU budget and the Inmarsat-C switch are under Advanced.
// What the mode does and does not decode is said on the Voice tab and in docs/INMARSAT.md. See docs/INMARSAT-REVAMP.md.
#include "../adsb/adsb_map.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/codec/codec_registry.h"
#include "../panels/picsave_ui.h"
#include "app.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/common/time_fmt.h"
#include "dect2/inmsat/inmsat_planes.h"
#include "dect2/inmsat/inmsat_rx.h"
#include "dect2/inmsat/inmsat_tel.h"
#include "dect2/inmsat/inmsat_voice.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    int view = 0;                        // 0 carriers, 1 messages, 2 aircraft, 3 voice
    int focusId = 0;
    int pushedFocus = -1;
    double budget = 10;
    double pushedBudget = -1;
    bool inmcOn = true;
    int pushedInmc = -1;
    char satChoice[48] = "";             // manual satellite, "" = automatic
    std::string pushedSat = "?";
    double pushedLat = -999, pushedLon = -999;
    bool record = false;
    int pushedRecord = -1;
    float pushedVol = -1; int pushedMute = -1;
    char filter[24] = "";
    adsbmap::View map;
    bool mapFollow = true;
    std::string sel;                     // planeKey of the selected aircraft
    char search[40] = "";
    bool autoJumped = false;             // the screen opened itself on the Aircraft view once
};
State S;

bool live(const App& a) { return a.rx.standard == 209; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.budget = (double)std::max(2L, std::min(24L, plat::prefs().getI("inmsatBudget", 10)));
    S.inmcOn = plat::prefs().getI("inmsatInmc", 1) != 0;
    S.record = plat::prefs().getI("inmsatRecord", 0) != 0;
    snprintf(S.satChoice, sizeof S.satChoice, "%s", plat::prefs().getS("inmsatSat", "").c_str());
}

const char* stateName(int s) { return s >= 3 ? "data" : s == 2 ? "frames in sync" : s == 1 ? "carrier locked" : "seen"; }

std::string idleText(double s) {
    if (s < 0) return "-";
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s); else snprintf(b, sizeof b, "%.0f min", s / 60);
    return b;
}

bool contains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

void header(App& a) {
    ImGui::AlignTextToFramePadding();
    if (ImGui::Button("Auto##inmsat")) {
        S.focusId = 0;
        a.engine.inmsat().reset();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Automatic is the normal mode: every carrier in the capture is found and decoded. This button forgets the carriers and searches again\n(after you moved the antenna or changed the gain). Messages stay.");
    flowNext(6 * gUi);
    if (ImGui::Button("Clear##inmsat")) a.engine.inmsat().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Empty the message, aircraft and log-on lists.");
    flowNext(10 * gUi);
    if (live(a)) {
        const dect2::InmsatTelemetry& t = a.rx.inmsat;
        ImGui::TextDisabled("%.2f Msps, %zu carriers, %zu windows, CPU %.1f of %.0f", t.rate / 1e6, t.carriers.size(), t.windows.size(), t.cpuCost, t.cpuBudget);
        flowNext(10 * gUi);
        // the aircraft with a position, one click from anywhere; before the first one, what is being waited for
        const int placed = dect2::inmsat::placedCount(t.aircraft);
        if (placed > 0) {
            char b[64]; snprintf(b, sizeof b, "%d aircraft with a position##inmchip", placed);
            ImGui::PushStyleColor(ImGuiCol_Button, S.view == 2 ? ImVec4(0.17f, 0.36f, 0.58f, 1) : ImVec4(0.12f, 0.30f, 0.52f, 0.9f));
            if (ImGui::Button(b)) S.view = 2;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Show these aircraft on the map (Aircraft view).");
        } else {
            ImGui::TextDisabled("waiting for position reports (%zu aircraft heard)", t.aircraft.size());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Aero channels carry only what the aircraft report: an ADS-C position every few minutes, or ACARS messages with a position.\nAircraft appear on the map as their reports arrive; that can take several minutes.");
        }
    }
    flowEnd();
}

// the whole capture as a strip: one bar per carrier, height by its level, green when decoding, amber locked, grey seen, red parked
void strip(App& a, const dect2::InmsatTelemetry& t) {
    const float h = 70 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(100.f, ImGui::GetContentRegionAvail().x);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(14, 16, 18, 255));
    const double half = 0.5 * t.rate;
    for (const auto& wi : t.windows) {
        const float x0 = p0.x + (float)((wi.centerHz - wi.bwHz / 2 + half) / t.rate) * w, x1 = p0.x + (float)((wi.centerHz + wi.bwHz / 2 + half) / t.rate) * w;
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + h), IM_COL32(60, 80, 110, 70));
    }
    for (const auto& c : t.carriers) {
        const float x = p0.x + (float)((c.offsetHz + half) / t.rate) * w;
        const float bh = std::min(h - 4, std::max(6.f, (c.snrDb / 25.f) * (h - 4)));
        const ImU32 col = c.parked ? IM_COL32(210, 80, 80, 255) : c.state >= 2 ? IM_COL32(90, 200, 120, 255) : c.state == 1 ? IM_COL32(230, 180, 70, 255) : IM_COL32(150, 154, 158, 255);
        dl->AddRectFilled(ImVec2(x - 1.5f * gUi, p0.y + h - bh), ImVec2(x + 1.5f * gUi, p0.y + h), col);
        if (c.id == S.focusId) dl->AddRect(ImVec2(x - 5 * gUi, p0.y + h - bh - 3), ImVec2(x + 5 * gUi, p0.y + h), IM_COL32(255, 255, 255, 220));
    }
    char lo[32], hi[32];
    const double c0 = t.centerHz != 0 ? t.centerHz / 1e6 : a.freqMhz;
    snprintf(lo, sizeof lo, "%.3f MHz", c0 - half / 1e6); snprintf(hi, sizeof hi, "%.3f MHz", c0 + half / 1e6);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(150, 154, 158, 255), lo);
    dl->AddText(ImVec2(p0.x + w - ImGui::CalcTextSize(hi).x - 4, p0.y + 2), IM_COL32(150, 154, 158, 255), hi);
    ImGui::Dummy(ImVec2(w, h));
}

// which satellite the capture is looking at, and why: the three kinds of evidence of the best candidates
void satellitePanel(const dect2::InmsatTelemetry& t) {
    const dect2::inmsat::SatResult& s = t.sat;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Satellite");
    ImGui::SameLine(0, 8 * gUi);
    if (s.valid) {
        ImGui::Text("%s", s.name.c_str());
        char lon[24]; snprintf(lon, sizeof lon, "%.1f%c", std::fabs(s.lonDeg), s.lonDeg < 0 ? 'W' : 'E');
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", lon);
        if (!s.beam.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", s.beam.c_str()); }
        ImGui::SameLine(0, 8 * gUi);
        if (s.manual) ImGui::TextDisabled("chosen by you");
        else ImGui::TextColored(s.confidence >= 0.7 ? ImVec4(0.45f, 0.85f, 0.55f, 1) : s.confidence >= 0.4 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.9f, 0.5f, 0.4f, 1), "%.0f%% sure%s", s.confidence * 100, s.ambiguous ? ", close call" : "");
    } else {
        ImGui::TextDisabled("%s", s.summary.c_str());
    }
    {
        if (!s.ranked.empty() && ImGui::TreeNode("Evidence##inmsatev")) {
            const size_t n = std::min<size_t>(3, s.ranked.size());
            for (size_t i = 0; i < n; i++) {
                const auto& c = s.ranked[i];
                const auto& sat = dect2::inmsat::satTable()[(size_t)c.index];
                ImGui::Text("%s  %.0f%%", sat.name, c.confidence * 100);
                for (const auto& e : c.evidence) ImGui::TextDisabled("    %s: %s%s", e.kind.c_str(), e.available ? "" : "(no data) ", e.text.c_str());
            }
            ImGui::TextDisabled("%s", "Tables are unverified: needs a user test with a real capture.");
            ImGui::TreePop();
        }
    }
}

void carriersView(App& a) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    satellitePanel(t);
    strip(a, t);
    ImGui::Spacing();
    if (t.carriers.empty()) { ImGui::TextDisabled("%s", "no carriers yet: the search needs two seconds. Tune the radio to the middle of the satellite's L-band channels (about 1545.6 MHz) and use a sample rate of 6 Msps or more."); return; }
    if (!ImGui::BeginTable("##inmc", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Satellite"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Rate"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Eb/N0");
    ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Last"); ImGui::TableSetupColumn("Width"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    std::vector<dect2::inmsat::CarrierRow> rows = t.carriers;
    std::sort(rows.begin(), rows.end(), [](const dect2::inmsat::CarrierRow& x, const dect2::inmsat::CarrierRow& y) { return x.offsetHz < y.offsetHz; });
    for (const auto& c : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char f[48];
        if (c.freqHz > 0) snprintf(f, sizeof f, "%.4f MHz##ic%d", c.freqHz / 1e6, c.id); else snprintf(f, sizeof f, "%+.1f kHz##ic%d", c.offsetHz / 1e3, c.id);
        if (ImGui::Selectable(f, c.id == S.focusId, ImGuiSelectableFlags_SpanAllColumns)) { S.focusId = c.id == S.focusId ? 0 : c.id; }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.satellite.empty() ? "-" : c.satellite.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::inmsat::carrierTypeName(c.type));
        ImGui::TableNextColumn(); if (c.bitRate) ImGui::Text("%d", c.bitRate); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.snrDb);
        ImGui::TableNextColumn(); if (c.state >= 2) ImGui::Text("%.1f dB", c.ebn0Db); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.parked ? "parked: CPU" : stateName(c.state));
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.frames);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(idleText(c.idleSec).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f kHz", c.widthHz / 1e3);
        ImGui::TableNextColumn();
        std::string note = c.region;
        if (!c.station.empty()) note += (note.empty() ? "" : ", ") + c.station;
        if (c.type == dect2::inmsat::CarrierType::AeroC) note += std::string(note.empty() ? "" : ", ") + "voice: " + std::to_string((unsigned long long)c.voiceFrames) + " coded frames";
        else if (c.voiceCandidate) note += std::string(note.empty() ? "" : ", ") + "wide, no decoder in sync (not a C channel in a call, or too weak)";
        if (c.type == dect2::inmsat::CarrierType::Unknown && !c.voiceCandidate && !c.parked && c.state < 1) note += std::string(note.empty() ? "" : ", ") + dect2::inmsat::carrierClassName(c.cls);
        ImGui::TextDisabled("%s", note.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void messagesView(App& a) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    ImGui::TextDisabled("Filter"); ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputText("##inmflt", S.filter, sizeof S.filter);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Shows messages whose registration, flight, label or text contains this.");
    ImGui::SameLine(); ImGui::TextDisabled("%zu ACARS / ADS-C, %zu Inmarsat-C", t.messages.size(), t.egc.size());
    if (!ImGui::BeginTable("##inmmsg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Aircraft / area"); ImGui::TableSetupColumn("Label"); ImGui::TableSetupColumn("Rate");
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (size_t i = t.messages.size(); i-- > 0;) {
        const auto& m = t.messages[i];
        const std::string hay = m.registration + " " + m.flight + " " + m.label + " " + m.text + " " + m.decoded;
        if (!contains(hay, S.filter)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(m.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(m.decoded.empty() ? "ACARS" : "ADS-C");
        ImGui::TableNextColumn(); ImGui::Text("%s %s", m.registration.c_str(), m.flight.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", m.label.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", m.bitRate);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", (m.decoded.empty() ? m.text : m.decoded).c_str());
    }
    for (const auto& e : t.egc) {
        if (!contains(e.serviceText + " " + e.text + " " + e.navarea + " " + e.subject, S.filter)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", e.rxTime.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.kind == 1 ? "SafetyNET" : e.kind == 2 ? "FleetNET" : "Inmarsat-C");
        ImGui::TableNextColumn(); ImGui::Text("%s", e.navarea.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", e.ident.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", e.complete ? "" : "part");
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", e.text.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

// the grid of the offline map (no tiles on screen): latitude and longitude of every graticule line, on the map's left and top edges
void gridLabels(ImDrawList* dl, ImVec2 p0, ImVec2 size) {
    const int step = S.map.zoom < 4 ? 30 : 10;
    const ImU32 col = IM_COL32(150, 160, 170, 200);
    char b[16];
    for (int la = -80; la <= 80; la += step) {
        const float y = adsbmap::project(la, S.map.lon).y;
        if (y < p0.y + 36 * gUi || y > p0.y + size.y - 22 * gUi) continue;
        snprintf(b, sizeof b, "%d%c", std::abs(la), la < 0 ? 'S' : la > 0 ? 'N' : ' ');
        dl->AddText(ImVec2(p0.x + 3, y - ImGui::GetTextLineHeight() - 1), col, b);
    }
    for (int lo = -180; lo <= 180; lo += step) {
        const float x = adsbmap::project(S.map.lat, lo).x;
        if (x < p0.x + 40 * gUi || x > p0.x + size.x - 30 * gUi) continue;
        snprintf(b, sizeof b, "%d%c", std::abs(lo), lo < 0 ? 'W' : lo > 0 ? 'E' : ' ');
        dl->AddText(ImVec2(x + 3, p0.y + size.y - 2 * ImGui::GetTextLineHeight() - 4), col, b);
    }
}

std::string planeLabel(const dect2::AeroAircraft& c, double ageSec) {
    std::string s = dect2::inmsat::planeName(c);
    char b[40];
    if (c.hasAlt) { snprintf(b, sizeof b, "  FL%03d", (c.altFt + 50) / 100); s += b; }
    if (c.hasSpeed) { snprintf(b, sizeof b, "  %.0f kt", c.speedKt); s += b; }
    if (c.hasTrack) { snprintf(b, sizeof b, "  %03.0f", c.trackDeg); s += b; }
    if (ageSec >= 0) s += "  " + dect2::inmsat::ageText(ageSec);
    return s;
}

std::string heardOn(const dect2::InmsatTelemetry& t, const dect2::AeroAircraft& c) {
    std::string s = t.sat.valid ? t.sat.name : "satellite ?";
    char b[48];
    if (c.heardHz > 0) { snprintf(b, sizeof b, ", %.4f MHz", c.heardHz / 1e6); s += b; }
    if (c.heardBitRate) { snprintf(b, sizeof b, ", %d bit/s", c.heardBitRate); s += b; }
    return s;
}

// the box about the selected aircraft, over the map's top right corner
void planeBox(ImDrawList* dl, ImVec2 p0, ImVec2 size, const dect2::InmsatTelemetry& t, const dect2::AeroAircraft& c, int64_t now) {
    std::vector<std::string> ln;
    char b[160];
    ln.push_back(dect2::inmsat::planeName(c) + (c.registration.empty() || c.registration == dect2::inmsat::planeName(c) ? "" : "  " + c.registration));
    const std::string icao = dect2::inmsat::planeIcao(c);
    if (!icao.empty()) ln.push_back("ICAO " + icao);
    if (c.hasPos) {
        snprintf(b, sizeof b, "%.4f %.4f", c.lat, c.lon); ln.push_back(b);
        std::string v;
        if (c.hasAlt) { snprintf(b, sizeof b, "%d ft", c.altFt); v += b; }
        if (c.hasSpeed) { snprintf(b, sizeof b, "%s%.0f kt", v.empty() ? "" : ", ", c.speedKt); v += b; }
        if (c.hasTrack) { snprintf(b, sizeof b, "%strack %03.0f", v.empty() ? "" : ", ", c.trackDeg); v += b; }
        if (!v.empty()) ln.push_back(v);
        ln.push_back("position " + dect2::inmsat::ageText(dect2::inmsat::posAgeSec(c, now)) + " ago, " + (c.posKind.empty() ? dect2::inmsat::planeSourceName(c.posSource) : c.posKind));
    } else ln.push_back("no position report yet");
    ln.push_back("heard on " + heardOn(t, c));
    snprintf(b, sizeof b, "%llu messages, %llu positions", (unsigned long long)c.messages, (unsigned long long)c.positions); ln.push_back(b);
    float w = 0;
    for (const auto& s : ln) w = std::max(w, ImGui::CalcTextSize(s.c_str()).x);
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 a(p0.x + size.x - w - 20 * gUi, p0.y + 8 * gUi);
    dl->AddRectFilled(a, ImVec2(a.x + w + 12 * gUi, a.y + lh * (float)ln.size() + 10 * gUi), IM_COL32(10, 12, 14, 225), 3.f);
    for (size_t i = 0; i < ln.size(); i++) dl->AddText(ImVec2(a.x + 6 * gUi, a.y + 5 * gUi + lh * (float)i), i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 210, 215, 255), ln[i].c_str());
}

void mapCanvas(App& a, ImVec2 size) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    const int64_t now = (int64_t)std::time(nullptr);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& c : t.aircraft) if (c.hasPos) { la0 = std::min(la0, c.lat); la1 = std::max(la1, c.lat); lo0 = std::min(lo0, c.lon); lo1 = std::max(lo1, c.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            // at least about 6 degrees across, so one aircraft is shown with its surroundings; offline the coastline is drawn to zoom 8 only
            const double span = std::max(6.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            const int zmax = adsbmap::online() ? 10 : 8;
            S.map.zoom = std::max(2, std::min(zmax, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (!adsbmap::tilesAvailable()) gridLabels(dl, p0, size);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    const dect2::AeroAircraft* hit = nullptr; float best = 18 * gUi;
    int placed = 0;
    // trails and the predicted route first, so the aircraft are drawn over them
    for (const auto& c : t.aircraft) {
        if (!c.hasPos) continue;
        placed++;
        const float fade = dect2::inmsat::posFade(dect2::inmsat::posAgeSec(c, now));
        for (size_t i = 1; i < c.track.size(); i++) {
            if (dect2::inmsat::segmentCrossesDateLine(c.track[i - 1].lon, c.track[i].lon)) continue;
            const float k = 0.35f + 0.65f * (float)i / (float)c.track.size();   // older parts of the trail are fainter
            dl->AddLine(adsbmap::project(c.track[i - 1].lat, c.track[i - 1].lon), adsbmap::project(c.track[i].lat, c.track[i].lon), adsbmap::acColour(adsbmap::AcSrc::Sat, 0, fade * k), 1.8f);
        }
        if (!c.route.empty() && !dect2::inmsat::segmentCrossesDateLine(c.lon, c.route[0].second)) {
            ImVec2 prev = adsbmap::project(c.lat, c.lon);
            for (const auto& w : c.route) {
                const ImVec2 q = adsbmap::project(w.first, w.second);
                dl->AddLine(prev, q, IM_COL32(120, 170, 230, (int)(110 * fade)), 1.f);
                prev = q;
            }
        }
    }
    for (const auto& c : t.aircraft) {
        if (!c.hasPos) continue;
        const ImVec2 q = adsbmap::project(c.lat, c.lon);
        if (q.x < p0.x - 60 || q.y < p0.y - 60 || q.x > p0.x + size.x + 60 || q.y > p0.y + size.y + 60) continue;
        const bool sel = !S.sel.empty() && dect2::inmsat::planeKey(c) == S.sel;
        const double age = dect2::inmsat::posAgeSec(c, now);
        const float fade = dect2::inmsat::posFade(age);
        const ImU32 fill = adsbmap::acColour(adsbmap::AcSrc::Sat, 0, fade), edge = sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, (int)(170 * fade));
        const float r = 6.5f * gUi;
        if (c.hasTrack) {   // an arrow pointing the way it flies
            const float th = (float)(c.trackDeg * 0.017453292519943295), sx = std::sin(th), sy = -std::cos(th);
            const ImVec2 tip(q.x + sx * r * 1.5f, q.y + sy * r * 1.5f), l(q.x - sx * r * 0.9f - sy * r * 0.9f, q.y - sy * r * 0.9f + sx * r * 0.9f), rr(q.x - sx * r * 0.9f + sy * r * 0.9f, q.y - sy * r * 0.9f - sx * r * 0.9f), nt(q.x - sx * r * 0.35f, q.y - sy * r * 0.35f);
            dl->AddQuadFilled(tip, l, nt, rr, fill);
            dl->AddQuad(tip, l, nt, rr, edge, sel ? 2.f : 1.f);
        } else {
            dl->AddCircleFilled(q, 5 * gUi, fill);
            dl->AddCircle(q, 5 * gUi, edge, 16, sel ? 2.f : 1.f);
        }
        if (S.map.zoom >= 5 || sel || placed <= 30) {
            const std::string s = planeLabel(c, age);
            const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddRectFilled(ImVec2(q.x + 11 * gUi, q.y - ts.y / 2 - 2), ImVec2(q.x + 19 * gUi + ts.x, q.y + ts.y / 2 + 2), IM_COL32(10, 12, 14, (int)(215 * std::max(fade, 0.55f))), 2.f);
            dl->AddText(ImVec2(q.x + 15 * gUi, q.y - ts.y / 2), IM_COL32(235, 238, 240, (int)(255 * std::max(fade, 0.55f))), s.c_str());
        }
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < best) { best = d; hit = &c; }
    }
    if (clicked) S.sel = hit ? dect2::inmsat::planeKey(*hit) : std::string();
    if (!S.sel.empty()) for (const auto& c : t.aircraft) if (dect2::inmsat::planeKey(c) == S.sel) { planeBox(dl, p0, size, t, c, now); break; }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##inmz", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##inmz", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit##inmz")) S.mapFollow = true;
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("?##inmhelp")) ImGui::OpenPopup("##inmplanehelp");
    ImGui::PopStyleColor();
    if (ImGui::BeginPopup("##inmplanehelp")) {
        ImGui::PushTextWrapPos(420 * gUi);
        ImGui::TextUnformatted("Where the aircraft are");
        ImGui::Separator();
        ImGui::TextWrapped("%s", "Inmarsat Aero channels carry only what an aircraft reports about itself: an ADS-C position report every few minutes, plus ACARS messages with a position in the text (POS and label 16 / 20 reports, weather reports). So aircraft appear one by one, minutes apart: a map with few planes after a short time is normal, not broken.");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", "An arrow points the way the aircraft was heading; a dot means no direction is known. The shade fades as the last position gets old (full for 5 minutes, faint after 40). The line behind it is its trail, the thin line ahead is the route in its last ADS-C report. Click an aircraft (or a row below) for its details and messages.");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", "Without network access the map shows coastlines and a latitude / longitude grid from the data built into OnAir. Log-on messages give an aircraft's ICAO address but no position. CPDLC position reports are not decoded yet. Unverified against a real capture: needs a user test.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    if (!placed) {   // never an empty, silent map: say what is happening
        const char* msg = "waiting for position reports";
        const char* sub = "ADS-C and ACARS position reports come every few minutes per aircraft";
        const ImVec2 a1 = ImGui::CalcTextSize(msg), a2 = ImGui::CalcTextSize(sub);
        const float bw = std::max(a1.x, a2.x) + 24 * gUi, bh = a1.y + a2.y + 18 * gUi;
        const ImVec2 b0(p0.x + (size.x - bw) / 2, p0.y + (size.y - bh) / 2);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), IM_COL32(10, 12, 14, 215), 4.f);
        dl->AddText(ImVec2(b0.x + (bw - a1.x) / 2, b0.y + 6 * gUi), IM_COL32(240, 200, 110, 255), msg);
        dl->AddText(ImVec2(b0.x + (bw - a2.x) / 2, b0.y + 12 * gUi + a1.y), IM_COL32(190, 195, 200, 255), sub);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    const char* offline = adsbmap::tilesAvailable() ? "" : "   offline map: coastlines and grid";
    if (!placed) adsbmap::legend(size.x, "no aircraft with a position yet (%zu heard)%s", t.aircraft.size(), offline);
    else adsbmap::legend(size.x, "%d of %zu aircraft with a position%s", placed, t.aircraft.size(), offline);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void aircraftTable(App& a) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    const int64_t now = (int64_t)std::time(nullptr);
    ImGui::TextDisabled("Search"); ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputText("##inmsearch", S.search, sizeof S.search);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Flight, registration, ICAO address (hex) or report type.");
    ImGui::SameLine(); ImGui::TextDisabled("%d with a position, %zu heard", dect2::inmsat::placedCount(t.aircraft), t.aircraft.size());
    if (!ImGui::BeginTable("##inmair", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Flight", 0, 0, 0);
    ImGui::TableSetupColumn("Registration", ImGuiTableColumnFlags_NoSort, 0, 1);
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_NoSort, 0, 2);
    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_NoSort, 0, 3);
    ImGui::TableSetupColumn("Altitude", ImGuiTableColumnFlags_PreferSortDescending, 0, 4);
    ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_PreferSortDescending, 0, 5);
    ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_NoSort, 0, 6);
    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_DefaultSort, 0, 7);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_NoSort, 0, 8);
    ImGui::TableSetupColumn("Heard on", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthStretch, 0, 9);
    ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_PreferSortDescending, 0, 10);
    ImGui::TableHeadersRow();
    dect2::inmsat::PlaneSort key = dect2::inmsat::PlaneSort::Age; bool desc = false;
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsCount > 0) {
        const int id = (int)sp->Specs[0].ColumnUserID;
        key = id == 0 ? dect2::inmsat::PlaneSort::Name : id == 4 ? dect2::inmsat::PlaneSort::Altitude : id == 5 ? dect2::inmsat::PlaneSort::Speed : id == 10 ? dect2::inmsat::PlaneSort::Messages : dect2::inmsat::PlaneSort::Age;
        desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
    }
    ImGui::PushFont(a.mono, 0);
    for (size_t i : dect2::inmsat::sortedPlanes(t.aircraft, key, desc, S.search, now)) {
        const dect2::AeroAircraft& c = t.aircraft[i];
        const double age = dect2::inmsat::posAgeSec(c, now);
        const std::string k = dect2::inmsat::planeKey(c);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char id[64]; snprintf(id, sizeof id, "%s##inma%s", dect2::inmsat::planeName(c).c_str(), k.c_str());
        if (ImGui::Selectable(id, !S.sel.empty() && k == S.sel, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = k; if (c.hasPos) { S.map.lat = c.lat; S.map.lon = c.lon; S.mapFollow = false; } }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.registration.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::inmsat::planeIcao(c).c_str());
        ImGui::TableNextColumn(); if (c.hasPos) ImGui::Text("%.3f %.3f", c.lat, c.lon); else ImGui::TextDisabled("waiting");
        ImGui::TableNextColumn(); if (c.hasPos && c.hasAlt) ImGui::Text("FL%03d", (c.altFt + 50) / 100); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (c.hasSpeed) ImGui::Text("%.0f kt", c.speedKt); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (c.hasTrack) ImGui::Text("%03.0f", c.trackDeg); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (age >= 0) ImGui::TextUnformatted(dect2::inmsat::ageText(age).c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c.hasPos ? (c.posKind.empty() ? dect2::inmsat::planeSourceName(c.posSource) : c.posKind.c_str()) : "-");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", heardOn(t, c).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.messages);
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

// the messages of the selected aircraft, newest first
void aircraftHistory(App& a, ImVec2 size) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    const dect2::AeroAircraft* sel = nullptr;
    for (const auto& c : t.aircraft) if (!S.sel.empty() && dect2::inmsat::planeKey(c) == S.sel) { sel = &c; break; }
    ImGui::BeginChild("##inmhist", size, true);
    if (!sel) {
        ImGui::TextDisabled("%s", "click an aircraft on the map or in the list to see its messages");
    } else {
        ImGui::TextUnformatted(dect2::inmsat::planeName(*sel).c_str());
        const auto hist = dect2::inmsat::planeHistory(t.messages, *sel, 40);
        ImGui::SameLine(); ImGui::TextDisabled("%zu messages kept", hist.size());
        ImGui::PushFont(a.mono, 0);
        ImGui::PushTextWrapPos(0);
        for (size_t i = hist.size(); i-- > 0;) {
            const dect2::AeroMessage& m = *hist[i];
            ImGui::TextDisabled("%s %s %s%s", dect2::timefmt::clock(m.wallTime).c_str(), m.label.c_str(), m.uplink ? "up" : "down", m.hasPos ? "  position" : "");
            ImGui::TextUnformatted((m.decoded.empty() ? m.text : m.decoded).c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void aircraftView(App& a) {
    const float h = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
    mapCanvas(a, ImVec2(w, std::max(120.f * gUi, h * 0.55f)));
    const float bh = ImGui::GetContentRegionAvail().y;
    if (w > 900 * gUi) {
        ImGui::BeginChild("##inmtblc", ImVec2(w * 0.64f, bh), false);
        aircraftTable(a);
        ImGui::EndChild();
        ImGui::SameLine();
        aircraftHistory(a, ImVec2(0, bh));
    } else {
        aircraftTable(a);
    }
}

void voiceView(App& a) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", "Aero C channels carry circuit-mode voice from the ground station to the aircraft, 8400 bit/s OQPSK in 0.5 s frames. Every wide carrier in a window gets a "
                              "C-channel decoder (the framing follows JAERO, github.com/jontio/JAERO); a carrier whose frames check shows as \"Aero C (voice)\" in the Carriers tab and its calls "
                              "appear below. Verified on real captures (France, Alphasat / 15.5W, October 2026). Voice needs the Aero AMBE codec (4800 bit/s), which you install yourself "
                              "(docs/CODECS.md); without it the calls are counted and their coded frames saved. Unencrypted traffic only.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    codecHelpButton(CodecHelp::AeroAmbe);
    ImGui::SameLine();
    appcodec::voiceCodecLine("aeroambe", "Aero AMBE");   // found / notice to accept / missing, with the prompt and the install help (? opens it too)
    if (t.codecReady) ImGui::TextDisabled("%s", "Codec loaded.");
    else if (!t.voiceMessage.empty() && !dect2::codec::pluginLoaded("aeroambe")) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.voiceMessage.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::Checkbox("Record calls##inmrec", &S.record)) plat::prefs().setI("inmsatRecord", S.record ? 1 : 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Writes each call to <Save pictures to>/Inmarsat as a WAV (with the codec) and always as the raw coded frames (.bits), so a capture is not wasted.");
    ImGui::Spacing();
    int cand = 0, cch = 0;
    for (const auto& c : t.carriers) { cand += c.voiceCandidate; cch += c.type == dect2::inmsat::CarrierType::AeroC; }
    ImGui::TextDisabled("%d C channel%s in sync; %d wide carrier%s with no decoder in sync.", cch, cch == 1 ? "" : "s", cand, cand == 1 ? "" : "s");
    if (t.voice.empty()) { ImGui::TextDisabled("%s", "no calls"); return; }
    if (!ImGui::BeginTable("##inmv", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Mute");
    ImGui::TableSetupColumn("Solo"); ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& v : t.voice) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.4f MHz", v.freqHz / 1e6);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(v.active ? "call" : "idle");
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)v.decoded, (unsigned long long)v.frames);
        ImGui::TableNextColumn(); ImGui::ProgressBar(v.level, ImVec2(60 * gUi, 8 * gUi), "");
        ImGui::TableNextColumn(); bool m = v.mute; char id[24]; snprintf(id, sizeof id, "##vm%d", v.id);
        if (ImGui::Checkbox(id, &m)) a.engine.inmsat().voice().setMute(v.id, m);
        ImGui::TableNextColumn(); bool s = v.solo; snprintf(id, sizeof id, "##vs%d", v.id);
        if (ImGui::Checkbox(id, &s)) a.engine.inmsat().voice().setSolo(v.id, s);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", v.recording.empty() ? v.rawFile.c_str() : v.recording.c_str());
    }
    ImGui::EndTable();
}

void tick(App& a) {
    load();
    if (live(a)) appcodec::voiceCodecTick("aeroambe");   // the user's Aero AMBE plug-in (docs/CODECS.md), loaded whichever tab is shown
    if (!a.engine.running()) { S.pushedFocus = -1; S.pushedBudget = -1; S.pushedInmc = -1; S.pushedRecord = -1; return; }
    dect2::InmsatReceiver& r = a.engine.inmsat();
    if (S.focusId != S.pushedFocus) { r.setFocus(S.focusId); S.pushedFocus = S.focusId; }
    if (a.volume != S.pushedVol) { r.setVolume(a.volume); S.pushedVol = a.volume; }          // the sound controls are shared with the other modes
    if ((int)a.muted != S.pushedMute) { r.setMuted(a.muted); S.pushedMute = (int)a.muted; }
    if (S.budget != S.pushedBudget) { r.setBudget(S.budget); S.pushedBudget = S.budget; }
    if ((int)S.inmcOn != S.pushedInmc) { r.setInmarsatC(S.inmcOn); S.pushedInmc = (int)S.inmcOn; }
    if (S.pushedSat != S.satChoice) { r.setSatelliteOverride(S.satChoice); S.pushedSat = S.satChoice; }
    { double la = 0, lo = 0; const bool known = receiverLocationSet(); if (known) receiverLocation(la, lo);
      if (la != S.pushedLat || lo != S.pushedLon) { r.setLocation(known, la, lo); S.pushedLat = la; S.pushedLon = lo; } }
    if ((int)S.record != S.pushedRecord) { r.voice().setRecording(S.record ? picui::folder() + "/Inmarsat" : std::string()); S.pushedRecord = (int)S.record; }
}

void tuner(App&, bool&) {
    load();
    if (ImGui::CollapsingHeader("Advanced##inmsat")) {
        ImGui::TextDisabled("CPU budget"); ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi);
        float b = (float)S.budget;
        if (ImGui::SliderFloat("##inmbud", &b, 2.f, 24.f, "%.0f")) { S.budget = std::round(b); plat::prefs().setI("inmsatBudget", (long)S.budget); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "How much decoding runs at once. One window of carriers (an Aero receiver) costs 1, an added Inmarsat-C receiver 0.6.\nCarriers that do not fit are listed as parked. The focused carrier is served first.");
        ImGui::TextDisabled("Satellite"); ImGui::SameLine(); ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("##inmsatsel", S.satChoice[0] ? S.satChoice : "Automatic")) {
            if (ImGui::Selectable("Automatic", !S.satChoice[0])) { S.satChoice[0] = 0; plat::prefs().setS("inmsatSat", ""); }
            for (const auto& s : dect2::inmsat::satTable())
                if (ImGui::Selectable(s.name, !strcmp(S.satChoice, s.name))) { snprintf(S.satChoice, sizeof S.satChoice, "%s", s.name); plat::prefs().setS("inmsatSat", s.name); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Normally found automatically from the decoded system table, the carrier frequencies and where the satellite stands in your sky (set your location under View > Map). Choose one here to override it.");
        if (ImGui::Checkbox("Inmarsat-C##inmc", &S.inmcOn)) plat::prefs().setI("inmsatInmc", S.inmcOn ? 1 : 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Also decode the 2 kHz Inmarsat-C channels (EGC safety and news broadcasts) found in the capture.");
    }
}

void tab(App& a) {
    load();
    if (!S.autoJumped) {   // open on the aircraft map the first time positions exist (once: a later choice of another view stays)
        const int dv = adsbmap::devView(-1);
        if (dv >= 0) { S.view = dv; S.autoJumped = true; }
        else if (live(a) && dect2::inmsat::placedCount(a.rx.inmsat.aircraft) > 0) { S.autoJumped = true; if (S.view == 0) S.view = 2; }
    }
    header(a);
    ImGui::Spacing();
    subNav("inmsat", S.view, {"Carriers", "Messages", "Aircraft", "Voice"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (a.rx.inmsat.seq == 0) { ImGui::TextDisabled("%s", "searching for carriers"); return; }
    switch (S.view) {
        case 0: carriersView(a); break;
        case 1: messagesView(a); break;
        case 2: aircraftView(a); break;
        default: voiceView(a); break;
    }
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    const int64_t now = (int64_t)std::time(nullptr);
    ImGui::TextDisabled("%d of %zu with a position", dect2::inmsat::placedCount(t.aircraft), t.aircraft.size());
    if (!ImGui::BeginTable("##inml", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Aircraft"); ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Msgs");
    for (size_t i : dect2::inmsat::sortedPlanes(t.aircraft, dect2::inmsat::PlaneSort::Age, false, "", now)) {
        const dect2::AeroAircraft& c = t.aircraft[i];
        const std::string k = dect2::inmsat::planeKey(c);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
        char id[64]; snprintf(id, sizeof id, "%s##inmal%s", dect2::inmsat::planeName(c).c_str(), k.c_str());
        if (ImGui::Selectable(id, !S.sel.empty() && k == S.sel)) { S.sel = k; S.view = 2; if (c.hasPos) { S.map.lat = c.lat; S.map.lon = c.lon; S.mapFollow = false; } }
        ImGui::PopFont();
        ImGui::TableNextColumn();
        if (c.hasPos) ImGui::TextDisabled("%s", dect2::inmsat::ageText(dect2::inmsat::posAgeSec(c, now)).c_str()); else ImGui::TextDisabled("waiting");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.messages);
    }
    ImGui::EndTable();
    if (t.aircraft.empty()) ImGui::TextDisabled("%s", "no aircraft yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int dec = 0, voice = 0, parked = 0;
    for (const auto& c : t.carriers) { dec += c.state >= 2; voice += c.voiceCandidate; parked += c.parked; }
    kv("capture", "%.2f Msps around %.4f MHz", t.rate / 1e6, t.centerHz / 1e6);
    kv("carriers", "%zu found, %d decoding, %d parked, %d maybe voice", t.carriers.size(), dec, parked, voice);
    kv("windows", "%zu of cost %.1f (budget %.0f)", t.windows.size(), t.cpuCost, t.cpuBudget);
    kv("signal units", "%llu good, %llu failed the check", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("messages", "%llu ACARS, %llu with a position, %zu Inmarsat-C", (unsigned long long)t.messagesTotal, (unsigned long long)t.positionsTotal, t.egc.size());
    kv("signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Decoded: Aero P channels (600, 1200, 10500 bit/s) and Inmarsat-C, all carriers of the capture at once. Not decoded: Aero C (voice) channels, R and T channels (aircraft to satellite, 1626 to 1660 MHz).");
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    int dec = 0;
    for (const auto& c : t.carriers) dec += c.state >= 2;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carriers", !on ? 0 : t.carriers.empty() ? 0 : 1); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : dec > 0 ? 1 : 0); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.messagesTotal > 0 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::inmsatSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Inmarsat";
    if (live(a)) l2 = dect2::inmsatSummary(a.rx.inmsat);
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::InmsatTelemetry& t = a.rx.inmsat;
    int dec = 0;
    for (const auto& c : t.carriers) dec += c.state >= 2;
    out.push_back({"CARRIERS", "%.0f", (double)t.carriers.size(), 0, 24, t.carriers.empty() ? 0 : 1});
    out.push_back({"DECODING", "%.0f", (double)dec, 0, 24, dec > 0 ? 1 : 0});
    out.push_back({"Eb/N0  dB", "%.0f", t.snrDb, 0, 30, t.snrDb >= 10 ? 1 : t.snrDb >= 6 ? 2 : 3});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraft.size(), 0, 60, t.aircraft.empty() ? 0 : 1});
}

} // namespace

extern const ModeUi kInmsatUi;
const ModeUi kInmsatUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Inmarsat",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
    .meters = meters,
};
