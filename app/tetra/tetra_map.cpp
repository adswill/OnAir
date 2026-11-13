// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA Map sub-tab, see tetra_map.h. The positions come from the clear SDS messages the receiver decoded (tetra_sds.cpp); nothing is decrypted.
#include "tetra_map.h"
#include "map/my_location.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/maplayers.h"
#include "dect2/tetra/tetra_tel.h"
#include "state/results_export.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr double kStaleSec = 600.0;   // a terminal not heard for this long is drawn grey (LIP reports come every 10 s to several minutes)

struct State {
    bool loaded = false;
    adsbmap::View map;
    char issiBuf[24] = "";   // filter: ISSI (empty = all)
    int ageMin = 0;          // 0 = all, else the last N minutes
    bool metric = true;
    bool tracks = true;
    int sel = -2;            // the selected ISSI (-2 none, -1 the unknown terminal)
    bool fitted = false;
    std::string note;
};
State S;

bool live(const App& a) { return a.rx.standard == 47; }

const char* rumb(int deg) {
    static const char* k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return deg < 0 ? "-" : k[((deg + 22) / 45) & 7];
}

std::string clockText(int64_t t) {
    char b[24];
    const time_t tt = (time_t)t;
    if (const std::tm* tm = std::localtime(&tt)) std::strftime(b, sizeof b, "%H:%M:%S", tm); else snprintf(b, sizeof b, "-");
    return b;
}

ImU32 colourOf(int issi) {
    static const ImU32 c[] = {IM_COL32(0x22, 0xD3, 0xEE, 255), IM_COL32(0xF5, 0x9E, 0x0B, 255), IM_COL32(0x34, 0xD3, 0x99, 255), IM_COL32(0xA7, 0x8B, 0xFA, 255),
                              IM_COL32(0xFB, 0x71, 0x85, 255), IM_COL32(0x60, 0xA5, 0xFA, 255), IM_COL32(0xFA, 0xCC, 0x15, 255), IM_COL32(0xE8, 0x79, 0xF9, 255),
                              IM_COL32(0xF9, 0x73, 0x16, 255), IM_COL32(0x2D, 0xD4, 0xBF, 255)};
    return c[(unsigned)(issi < 0 ? 0 : issi) % (sizeof c / sizeof c[0])];
}

std::string idText(int issi) { return issi >= 0 ? std::to_string(issi) : std::string("?"); }

bool pass(const dect2::TetraPos& p, double now) {
    if (S.issiBuf[0]) { char* e = nullptr; const long f = std::strtol(S.issiBuf, &e, 10); if (e && !*e && p.issi != (int)f) return false; }
    if (S.ageMin > 0 && now - (double)p.wallTime > S.ageMin * 60.0) return false;
    return true;
}

struct Unit { int issi; const dect2::TetraPos* last; std::vector<ImVec2> track; };   // track in lat, lon

// "<what the report was> from <issi> to <address>": the one line a click shows as the source message
std::string sourceLine(const dect2::TetraPos& p) {
    char b[200];
    snprintf(b, sizeof b, "D-SDS-DATA from %s to %s on %.4f MHz", idText(p.issi).c_str(), p.toAddr >= 0 ? std::to_string(p.toAddr).c_str() : "?", p.freqHz / 1e6);
    return std::string(p.source.empty() ? "LIP report" : p.source) + "; " + b;
}

void cardRow(const char* k, const std::string& v) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
}

std::string num(double v, const char* f) { char b[48]; snprintf(b, sizeof b, f, v); return b; }

} // namespace

void tetraMap(App& a) {
    if (!S.loaded) { S.loaded = true; receiverLocation(S.map.lat, S.map.lon); S.map.zoom = 8; }
    const auto& t = a.rx.tetra;
    const double now = (double)std::time(nullptr);
    // the filtered units: last report and track of each ISSI
    std::map<int, Unit> units;
    size_t shown = 0;
    for (const auto& p : t.positions) {
        if (!pass(p, now)) continue;
        shown++;
        Unit& u = units[p.issi];
        u.issi = p.issi; u.last = &p;
        u.track.push_back(ImVec2((float)p.lat, (float)p.lon));
    }
    if (!S.fitted && !units.empty()) { const Unit& u = units.begin()->second; S.map.lat = u.last->lat; S.map.lon = u.last->lon; S.map.zoom = 12; S.fitted = true; }

    // the controls: ISSI filter, age, units, tracks, export
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("ISSI"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputTextWithHint("##tmissi", tr("all"), S.issiBuf, sizeof S.issiBuf);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%s", tr("last")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    static const char* ages[] = {"all", "5 min", "15 min", "1 h", "6 h"};
    static const int mins[] = {0, 5, 15, 60, 360};
    int cur = 0;
    for (int i = 0; i < 5; i++) if (S.ageMin == mins[i]) cur = i;
    if (ImGui::BeginCombo("##tmage", ages[cur])) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(ages[i], cur == i)) S.ageMin = mins[i];
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::Checkbox(tr("tracks"), &S.tracks);
    ImGui::SameLine(0, 10 * gUi);
    resexp::Table x;
    x.cols = {"time_unix", "id", "lat", "lon", "alt_m", "uncertainty_m", "speed_kmh", "heading_deg", "mode", "to_address", "frequency_hz", "source"};
    for (const auto& p : t.positions) {
        if (!pass(p, now)) continue;
        x.add({std::to_string(p.wallTime), idText(p.issi), resexp::num(p.lat, 6), resexp::num(p.lon, 6), p.hasAlt ? resexp::num(p.altM, 0) : "",
               p.uncM >= 0 ? resexp::num(p.uncM, 0) : "", p.speedKmh >= 0 ? resexp::num(p.speedKmh, 1) : "", p.headingDeg >= 0 ? std::to_string(p.headingDeg) : "",
               std::string("ISSI ") + idText(p.issi) + (p.pduType == 1 ? ", LIP long" : ", LIP short"), p.toAddr >= 0 ? std::to_string(p.toAddr) : "",
               resexp::num(p.freqHz, 0), p.source}, (double)p.wallTime);
    }
    resexp::saveButton(x, "tetra_map", S.note, "svtetramap");   // CSV, KML or GPX: one point per report, the ISSI as the name, the time of reception
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }

    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float panelW = std::min(250.f * gUi, W * 0.3f), gap = 8.f * gUi;
    ImGui::BeginChild("##tetramap", ImVec2(W - panelW - gap, H), false);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120 * gUi, ImGui::GetContentRegionAvail().y));
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    struct Hit { ImVec2 p; int issi; };
    std::vector<Hit> hits;
    const double mpp = 156543.03392 * std::cos(S.map.lat * M_PI / 180.0) / std::pow(2.0, S.map.zoom);   // metres per pixel at the centre
    for (const auto& kv : units) {
        const Unit& u = kv.second;
        const bool sel = S.sel == u.issi;
        const bool stale = now - (double)u.last->wallTime > kStaleSec;
        const ImU32 col = stale ? IM_COL32(150, 154, 158, 255) : colourOf(u.issi);
        if (S.tracks && u.track.size() > 1) {
            ImVec2 prev = adsbmap::project(u.track[0].x, u.track[0].y);
            for (size_t k = 1; k < u.track.size(); k++) {
                const ImVec2 q = adsbmap::project(u.track[k].x, u.track[k].y);
                if (std::fabs(u.track[k].y - u.track[k - 1].y) < 180) dl->AddLine(prev, q, (col & 0x00FFFFFFu) | 0x99000000u, 2.0f * gUi);
                prev = q;
            }
        }
        const ImVec2 q = adsbmap::project(u.last->lat, u.last->lon);
        if (sel && u.last->uncM > 0 && mpp > 0) {   // the position error circle
            const float r = (float)(u.last->uncM / mpp);
            if (r > 4.f * gUi && r < 4000.f) dl->AddCircle(q, r, (col & 0x00FFFFFFu) | 0xAA000000u, 48, 1.5f * gUi);
        }
        if (u.last->headingDeg >= 0 && !stale) {
            const float h = (float)(u.last->headingDeg * M_PI / 180.0), sx = std::sin(h), sy = -std::cos(h), r = 7.f * gUi;
            const ImVec2 tip(q.x + sx * (r + 9.f * gUi), q.y + sy * (r + 9.f * gUi)), base(q.x + sx * (r + 2.f * gUi), q.y + sy * (r + 2.f * gUi));
            dl->AddTriangleFilled(tip, ImVec2(base.x - sy * 4.f * gUi, base.y + sx * 4.f * gUi), ImVec2(base.x + sy * 4.f * gUi, base.y - sx * 4.f * gUi), col);
        }
        dl->AddCircleFilled(q, 6.f * gUi, col, 20);
        dl->AddCircle(q, 6.f * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), 20, sel ? 2.2f : 1.2f);
        if (adsbmap::labelsAt(S.map.zoom, sel)) dl->AddText(ImVec2(q.x + 10 * gUi, q.y - 7 * gUi), IM_COL32(235, 238, 240, 255), idText(u.issi).c_str());
        hits.push_back({q, u.issi});
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("me")) { double rl, ro; receiverLocation(rl, ro); S.map.lat = rl; S.map.lon = ro; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(S.metric ? "km/h" : "mph", ImVec2(46 * gUi, 0))) S.metric = !S.metric;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the terminal positions");
    ImVec2 cp;
    if (adsbmap::clickedAt(cp) && cp.y > p0.y + 34.0f * gUi) {
        float bd = 14.f * gUi;
        int best = -2;
        for (const Hit& h : hits) { const float d = std::hypotf(cp.x - h.p.x, cp.y - h.p.y); if (d < bd) { bd = d; best = h.issi; } }
        S.sel = best;
    }
    if (S.sel != -2 && !ImGui::GetIO().WantTextInput && !adsbmap::fullActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) S.sel = -2;
    ImGui::EndChild();

    // the click card: time, position, and the message it came from
    if (S.sel != -2) {
        const auto it = units.find(S.sel);
        if (it != units.end()) {
            const dect2::TetraPos& p = *it->second.last;
            const ImVec2 q = adsbmap::project(p.lat, p.lon);
            static ImVec2 cardSize(260 * gUi, 200 * gUi);
            const float margin = 4 * gUi;
            float px = q.x + 20 * gUi, py = q.y + 12 * gUi;
            if (px + cardSize.x > p0.x + size.x - margin) px = q.x - 20 * gUi - cardSize.x;
            if (py + cardSize.y > p0.y + size.y - margin) py = q.y - 12 * gUi - cardSize.y;
            px = std::clamp(px, p0.x + margin, std::max(p0.x + margin, p0.x + size.x - margin - cardSize.x));
            py = std::clamp(py, p0.y + margin, std::max(p0.y + margin, p0.y + size.y - margin - cardSize.y));
            ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::min(300.f * gUi, std::max(1.f, size.x - 2 * margin)), std::max(1.f, size.y - 2 * margin)));
            ImGui::SetNextWindowBgAlpha(0.94f);
            if (ImGui::Begin("##tetracard", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextColored(ImVec4(1, 1, 1, 1), "ISSI %s", idText(p.issi).c_str());
                ImGui::Separator();
                if (ImGui::BeginTable("##tetracardg", 2, ImGuiTableFlags_SizingFixedFit)) {
                    cardRow(tr("Received"), clockText(p.wallTime) + "  (" + dect2::ageText(std::max(0.0, now - (double)p.wallTime)) + " " + tr("ago") + ")");
                    if (p.timeElapsed >= 0) { static const char* te[4] = {"< 5 s", "< 5 min", "< 30 min", "?"}; cardRow(tr("Fix age"), te[p.timeElapsed & 3]); }
                    cardRow(tr("Latitude"), num(p.lat, "%.5f"));
                    cardRow(tr("Longitude"), num(p.lon, "%.5f"));
                    if (p.hasAlt) cardRow(tr("Altitude"), num(p.altM, "%.0f m"));
                    if (p.uncM >= 0) cardRow(tr("Error"), num(p.uncM, "< %.0f m"));
                    if (p.speedKmh >= 0) cardRow(tr("Speed"), num(S.metric ? p.speedKmh : p.speedKmh * 0.621371, S.metric ? "%.1f km/h" : "%.1f mph"));
                    if (p.headingDeg >= 0) cardRow(tr("Course"), std::to_string(p.headingDeg) + " deg  " + rumb(p.headingDeg));
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("%s", tr("Message"));
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 280 * gUi);
                ImGui::TextUnformatted(sourceLine(p).c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::SmallButton(tr("copy coords"))) { char b[64]; snprintf(b, sizeof b, "%.6f, %.6f", p.lat, p.lon); ImGui::SetClipboardText(b); }
                cardSize = ImGui::GetWindowSize();
                ImGui::End();
            }
        }
    }

    ImGui::SameLine(0, gap);
    ImGui::BeginChild("##tetramapside", ImVec2(panelW, H), false);
    ImGui::TextDisabled("%s %zu, %s %zu", tr("terminals"), units.size(), tr("reports"), shown);
    ImGui::Separator();
    if (units.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", live(a) ? tr("No LIP position heard yet. Terminals send their GPS position in a clear SDS (short or long location report); an encrypted one is not read.")
                                          : tr("Start the receiver to see TETRA terminals on the map."));
        ImGui::PopTextWrapPos();
    }
    std::vector<const Unit*> rows;
    for (const auto& kv : units) rows.push_back(&kv.second);
    std::sort(rows.begin(), rows.end(), [](const Unit* l, const Unit* r) { return l->last->wallTime > r->last->wallTime; });   // the freshest first
    for (const Unit* u : rows) {
        const dect2::TetraPos& p = *u->last;
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rp.x + 6 * gUi, rp.y + ImGui::GetTextLineHeight() * 0.5f), 4 * gUi, colourOf(u->issi));
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 14 * gUi, rp.y));
        char b[96];
        snprintf(b, sizeof b, "ISSI %s##tm%d", idText(u->issi).c_str(), u->issi);
        if (ImGui::Selectable(b, S.sel == u->issi)) { S.sel = u->issi; S.map.lat = p.lat; S.map.lon = p.lon; S.map.zoom = std::max(S.map.zoom, 13); }
        ImGui::TextDisabled("%.5f, %.5f", p.lat, p.lon);
        mylocmap::infoLine(p.lat, p.lon);
        ImGui::TextDisabled("%s %s %s, %zu %s", tr("last heard"), dect2::ageText(std::max(0.0, now - (double)p.wallTime)).c_str(), tr("ago"), u->track.size(), tr("fixes"));
        ImGui::Separator();
    }
    ImGui::EndChild();
}
