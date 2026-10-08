// Iridium screens: the map of satellite sub-points with their recent track and the beam centres of the ring alerts, the frame
// counters and burst rates, the ring alert list, the pager messages with a text panel, the Iridium time, and a burst scatter
// (time against frequency) of the last 2 s.
#include "app.h"
#include "adsb_map.h"
#include "dect2/iridium_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>

namespace {

using dect2::IridiumTelemetry;

struct State {
    bool loaded = false;
    int view = 0;                    // 0 map, 1 messages, 2 ring alerts, 3 bursts
    int selSat = -1;                 // the selected satellite, -1 = none
    int selMsg = -1;                 // index into messages, newest = 0
    adsbmap::View map;
    bool mapFollow = true;           // the map fits the satellites until the user moves it
    float thrDb = 13.f;
    double pushedCenter = 0;
    float pushedThr = -1;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 20; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("iridiumZoom", 4);
    S.thrDb = (float)d.getD("iridiumThr", 13.0);
}

ImU32 satColour(int id, float alpha = 1.f) {
    static const int c[8][3] = {{90, 170, 245}, {240, 160, 70}, {120, 210, 120}, {230, 100, 110}, {190, 140, 240}, {80, 205, 200}, {240, 210, 90}, {200, 200, 205}};
    const int* k = c[(id < 0 ? 0 : id) % 8];
    return IM_COL32(k[0], k[1], k[2], (int)(255 * alpha));
}

std::string utcText(double unixSec, bool date) {
    const time_t s = (time_t)std::floor(unixSec);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[40];
    if (date) snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    else snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string ageText(double s) {
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", std::max(0.0, s));
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

const dect2::IridiumSatInfo* findSat(const IridiumTelemetry& t, int id) {
    for (const auto& s : t.sats) if (s.id == id) return &s;
    return nullptr;
}

void kvLine(App& a, const char* k, const char* v, float col = 110) {
    ImGui::TextDisabled("%s", k); kvColumn(col * gUi);   // a narrow pane: the column moves left and the value wraps
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// ---------------------------------------------------------------- the map

void mapView(App& a, ImVec2 size) {
    const IridiumTelemetry& t = a.rx.iridium;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the satellite sub-points and beam centres, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& p : t.positions) { la0 = std::min(la0, (double)p.lat); la1 = std::max(la1, (double)p.lat); lo0 = std::min(lo0, (double)p.lon); lo1 = std::max(lo1, (double)p.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(2.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(8, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    int hit = -1; float best = 16 * gUi;
    if (on) {
        // beam centres first (small dots), then the satellite tracks, then the satellites
        for (const auto& p : t.positions) {
            if (p.satellite) continue;
            const float age = (float)(t.timeSec - p.time);
            const float al = std::max(0.25f, 1.f - age / 120.f);
            dl->AddCircleFilled(adsbmap::project(p.lat, p.lon), 2.5f * gUi, satColour(p.sat, 0.6f * al));
        }
        std::map<int, std::vector<ImVec2>> tracks;
        for (const auto& p : t.positions) if (p.satellite) tracks[p.sat].push_back(adsbmap::project(p.lat, p.lon));
        for (const auto& [id, v] : tracks) {
            for (size_t i = 1; i < v.size(); i++) dl->AddLine(v[i - 1], v[i], satColour(id, 0.5f), 1.5f);
        }
        for (const auto& s : t.sats) {
            if (!s.hasPos) continue;
            const ImVec2 q = adsbmap::project(s.lat, s.lon);
            const bool sel = s.id == S.selSat;
            const float r = (sel ? 7.f : 6.f) * gUi;
            const ImVec2 d[4] = {{q.x, q.y - r}, {q.x + r, q.y}, {q.x, q.y + r}, {q.x - r, q.y}};
            dl->AddConvexPolyFilled(d, 4, satColour(s.id));
            dl->AddPolyline(d, 4, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, sel ? 1.6f : 1.f);
            char b[16]; snprintf(b, sizeof b, "SV%03d", s.id);
            const ImVec2 lp(q.x + 10 * gUi, q.y - ImGui::GetTextLineHeight() * 0.5f);
            const ImVec2 ts = ImGui::CalcTextSize(b);
            dl->AddRectFilled(ImVec2(lp.x - 3 * gUi, lp.y - 1), ImVec2(lp.x + ts.x + 3 * gUi, lp.y + ts.y + 1), IM_COL32(10, 12, 14, 200), 2.f);
            dl->AddText(lp, IM_COL32(235, 238, 240, 255), b);
            const float dd = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && dd < best) { best = dd; hit = s.id; }
        }
    }
    if (clicked) S.selSat = hit;
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never a position");
    plat::prefs().setI("iridiumZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& s : t.sats) if (s.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no satellite positions yet (they come in ring alerts)");
    else adsbmap::legend(size.x, "%d of %zu satellites with a position; dots: beam centres", withPos, t.sats.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

// ---------------------------------------------------------------- panels

void timeBox(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Iridium time (IBC)"); ImGui::PopTextWrapPos(); }
    ImGui::PushFont(a.mono, 0);
    if (t.hasTime) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "%s UTC", utcText(t.iridiumUtc, true).c_str()); ImGui::PopTextWrapPos(); }   // the time wraps in a narrow pane
    else ImGui::TextDisabled("not heard yet");
    ImGui::PopFont();
}

void satTable(App& a, bool compact) {
    const IridiumTelemetry& t = a.rx.iridium;
    const int cols = compact ? 3 : 6;
    if (!ImGui::BeginTable(compact ? "##iri_ss" : "##iri_sf", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0, compact ? ImGui::GetContentRegionAvail().y : std::min(ImGui::GetContentRegionAvail().y, 220 * gUi)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sat", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Frames");
    if (!compact) { ImGui::TableSetupColumn("Beams"); ImGui::TableSetupColumn("Offset kHz"); ImGui::TableSetupColumn("Position"); }
    ImGui::TableSetupColumn("Heard");
    ImGui::TableHeadersRow();
    std::vector<const dect2::IridiumSatInfo*> rows;
    for (const auto& s : t.sats) rows.push_back(&s);
    std::stable_sort(rows.begin(), rows.end(), [](auto* x, auto* y) { return x->lastHeard > y->lastHeard; });
    for (const auto* s : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(s->id);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 4 * gUi, c.y + ImGui::GetTextLineHeight() * 0.5f), 3.f * gUi, satColour(s->id));
        ImGui::SetCursorScreenPos(ImVec2(c.x + 12 * gUi, c.y));
        char b[48]; snprintf(b, sizeof b, "SV%03d", s->id);
        if (ImGui::Selectable(b, s->id == S.selSat, ImGuiSelectableFlags_SpanAllColumns)) S.selSat = s->id;
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s->frames);
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::Text("%zu", s->beams.size());
            ImGui::TableNextColumn(); ImGui::Text("%+.2f", s->freqOffsetHz / 1e3);
            ImGui::TableNextColumn();
            if (s->hasPos) ImGui::Text("%.2f, %.2f", s->lat, s->lon); else ImGui::TextDisabled("-");
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(t.timeSec - s->lastHeard).c_str());
    }
    if (rows.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("none heard"); }
    ImGui::EndTable();
}

void satDetail(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    const dect2::IridiumSatInfo* s = live(a) ? findSat(t, S.selSat) : nullptr;
    if (!s) { ImGui::TextDisabled("click a satellite"); return; }
    char b[160];
    ImGui::TextColored(pal::heading(), "Satellite %d", s->id);
    snprintf(b, sizeof b, "%llu", (unsigned long long)s->frames); kvLine(a, "frames", b);
    snprintf(b, sizeof b, "%+.0f Hz", s->freqOffsetHz); kvLine(a, "offset", b);
    if (s->hasPos) { snprintf(b, sizeof b, "%.3f, %.3f, %.0f km", s->lat, s->lon, s->altKm); kvLine(a, "sub-point", b); }
    std::string beams;
    for (int bm : s->beams) { if (!beams.empty()) beams += ' '; beams += std::to_string(bm); }
    ImGui::TextDisabled("beams"); kvColumn(110 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", beams.empty() ? "-" : beams.c_str()); ImGui::PopFont();
    kvLine(a, "last heard", (ageText(t.timeSec - s->lastHeard) + " ago").c_str());
}

void counters(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    char b[96];
    // short lines: this sits in a narrow column next to the map
    snprintf(b, sizeof b, "%.0f/s", t.burstsPerSec); kvLine(a, "bursts", b);
    snprintf(b, sizeof b, "%.0f/s", t.uwPerSec); kvLine(a, "unique word", b);
    snprintf(b, sizeof b, "%.0f/s, %.0f %% sure", t.framesPerSec, t.confidence); kvLine(a, "frames", b);
    snprintf(b, sizeof b, "%llu down, %llu up", (unsigned long long)t.downlink, (unsigned long long)t.uplink); kvLine(a, "totals", b);
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.dropped); kvLine(a, "dropped", b);
    // four label-count pairs a row, fewer when the column is too narrow for them (a count ran past its cell)
    float cell = 0;
    for (int k = 0; k < dect2::kIridiumTypeSlots; k++) cell = std::max(cell, ImGui::CalcTextSize(dect2::iridiumTypeLabel(k)).x);
    ImGui::PushFont(a.mono, 0); cell += ImGui::CalcTextSize("00000").x; ImGui::PopFont();
    cell += ImGui::GetStyle().ItemSpacing.x + 2 * ImGui::GetStyle().CellPadding.x;
    const int perRow = std::clamp((int)(ImGui::GetContentRegionAvail().x / std::max(1.f, cell)), 1, 4);
    if (ImGui::BeginTable("##iri_cnt", perRow, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        int col = 0;
        for (int i = 1; i <= dect2::kIridiumTypeSlots; i++) {
            const int k = i % dect2::kIridiumTypeSlots;   // "other" last
            if (col % perRow == 0) ImGui::TableNextRow();
            ImGui::TableNextColumn(); col++;
            ImGui::TextDisabled("%s", dect2::iridiumTypeLabel(k)); ImGui::SameLine();
            ImGui::PushFont(a.mono, 0); ImGui::Text("%llu", (unsigned long long)t.typeCount[k]); ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Voice frames are counted only, never decoded.");
}

// ---------------------------------------------------------------- burst scatter

void scatter(App& a, ImVec2 size, bool legend = true) {
    const IridiumTelemetry& t = a.rx.iridium;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 18, 255), 3.f);
    const float lm = 64 * gUi, bm = ImGui::GetTextLineHeight() + 6 * gUi;
    const ImVec2 q0(p0.x + lm, p0.y + 6 * gUi), q1(p1.x - 8 * gUi, p1.y - bm);
    const double half = (t.inputRate > 0 ? t.inputRate : 10e6) / 2e3;   // kHz
    const double c = t.centerMhz > 0 ? t.centerMhz : 1622.0;
    // frequency lines every 1 MHz (or 0.25 MHz on narrow captures), labelled in absolute MHz
    const double step = half > 1500 ? 1000 : 250;
    for (double f = std::ceil((c * 1e3 - half) / step) * step; f <= c * 1e3 + half; f += step) {
        const float y = (float)(q1.y - (f - (c * 1e3 - half)) / (2 * half) * (q1.y - q0.y));
        dl->AddLine(ImVec2(q0.x, y), ImVec2(q1.x, y), IM_COL32(50, 54, 58, 255));
        char b[24]; snprintf(b, sizeof b, "%.2f", f / 1e3);
        dl->AddText(ImVec2(p0.x + 4 * gUi, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(150, 154, 158, 255), b);
    }
    // the simplex band (ring alerts, pager messages)
    for (double f : {1626.0, 1626.5}) {
        const double k = (f * 1e3 - (c * 1e3 - half)) / (2 * half);
        if (k < 0 || k > 1) continue;
        const float y = (float)(q1.y - k * (q1.y - q0.y));
        dl->AddLine(ImVec2(q0.x, y), ImVec2(q1.x, y), IM_COL32(120, 100, 50, 255));
    }
    for (int s = 0; s <= 2; s++) {
        const float x = q1.x - s / 2.f * (q1.x - q0.x);
        char b[16]; snprintf(b, sizeof b, s ? "-%d s" : "now", s);
        dl->AddText(ImVec2(std::min(x, q1.x - ImGui::CalcTextSize(b).x), q1.y + 3 * gUi), IM_COL32(150, 154, 158, 255), b);
    }
    static const ImU32 kc[3] = {IM_COL32(110, 114, 120, 200), IM_COL32(240, 180, 70, 230), IM_COL32(100, 210, 120, 255)};
    if (live(a)) {
        for (const auto& d : t.scatter) {
            const float x = q1.x - std::min(2.f, std::max(0.f, d.age)) / 2.f * (q1.x - q0.x);
            const float y = (float)(q1.y - (d.freqKHz + half) / (2 * half) * (q1.y - q0.y));
            if (y < q0.y || y > q1.y) continue;
            dl->AddRectFilled(ImVec2(x - 1.5f * gUi, y - 1.5f * gUi), ImVec2(x + 1.5f * gUi, y + 1.5f * gUi), kc[d.kind > 2 ? 2 : d.kind]);
        }
    }
    if (!legend) { ImGui::SetCursorScreenPos(p0); ImGui::Dummy(size); return; }
    ImGui::SetCursorScreenPos(ImVec2(q0.x + 6 * gUi, q0.y + 2 * gUi));
    ImGui::TextColored(ImVec4(0.43f, 0.45f, 0.47f, 1), "found"); ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.94f, 0.7f, 0.27f, 1), "unique word"); ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.39f, 0.82f, 0.47f, 1), "frame decoded"); ImGui::SameLine();
    ImGui::TextDisabled(live(a) ? "  amber lines: simplex channels" : "  start the receiver");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(size.x, 0));
}

// ---------------------------------------------------------------- lists

void ringAlerts(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu ring alerts kept, %llu TMSIs paged in all (the identities are not kept)", t.ringAlerts.size(), (unsigned long long)t.pagedTotal); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##iri_ra", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Sat"); ImGui::TableSetupColumn("Beam"); ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Alt km"); ImGui::TableSetupColumn("Paged"); ImGui::TableSetupColumn("MHz");
    ImGui::TableHeadersRow();
    for (auto it = t.ringAlerts.rbegin(); it != t.ringAlerts.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (t.hasTime) ImGui::TextUnformatted(utcText(t.iridiumUtc - (t.timeSec - it->time), false).c_str()); else ImGui::Text("%.1f s", it->time);
        ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(satColour(it->sat)), "%d", it->sat);
        ImGui::TableNextColumn(); ImGui::Text("%d", it->beam);
        ImGui::TableNextColumn(); if (it->hasPos) ImGui::Text("%.3f, %.3f", it->lat, it->lon); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (it->hasPos) ImGui::Text("%.0f", it->altKm); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", it->paged);
        ImGui::TableNextColumn(); ImGui::Text("%.4f", it->freqHz / 1e6);
    }
    if (t.ringAlerts.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("none yet"); }
    ImGui::EndTable();
}

void messages(App& a, float W, float H) {
    const IridiumTelemetry& t = a.rx.iridium;
    const float lw = std::min(460.f * gUi, W * 0.5f);
    ImGui::BeginChild("##iri_ml", ImVec2(lw, H));
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Pager messages (MSG)"); ImGui::PopTextWrapPos(); }
    const float tableH = t.acars.empty() ? ImGui::GetContentRegionAvail().y : ImGui::GetContentRegionAvail().y * 0.6f;
    if (ImGui::BeginTable("##iri_msg", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("RIC"); ImGui::TableSetupColumn("Seq"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const int n = (int)t.messages.size();
        for (int i = 0; i < n; i++) {
            const auto& m = t.messages[n - 1 - i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const std::string tm = t.hasTime ? utcText(t.iridiumUtc - (t.timeSec - m.time), false) : std::to_string((int)m.time) + " s";
            if (ImGui::Selectable(tm.c_str(), S.selMsg == i, ImGuiSelectableFlags_SpanAllColumns)) S.selMsg = i;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%d", m.ric);
            ImGui::TableNextColumn(); ImGui::Text("%d", m.seq);
            ImGui::TableNextColumn();
            if (m.complete) ImGui::TextUnformatted(m.text.c_str()); else ImGui::TextDisabled("%s (incomplete)", m.text.c_str());
        }
        if (!n) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("none yet"); }
        ImGui::EndTable();
    }
    if (!t.acars.empty()) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("ACARS in short burst data (%llu packets)", (unsigned long long)t.sbdPackets); ImGui::PopTextWrapPos(); }
        if (ImGui::BeginTable("##iri_ac", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Label"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto it = t.acars.rbegin(); it != t.acars.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->downlink ? "down" : "up");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->reg.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->text.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##iri_mt", ImVec2(0, H), ImGuiChildFlags_Borders);
    const int n = (int)t.messages.size();
    if (S.selMsg < 0 || S.selMsg >= n) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(n ? "click a message" : live(a) ? "no pager messages yet" : "start the receiver"); ImGui::PopTextWrapPos(); }
    } else {
        const auto& m = t.messages[n - 1 - S.selMsg];
        ImGui::TextColored(pal::heading(), "Message to %d", m.ric);
        char b[64];
        snprintf(b, sizeof b, "%d", m.seq); kvLine(a, "sequence", b, 90);
        kvLine(a, "complete", m.complete ? "yes" : "no, parts missing", 90);
        if (t.hasTime) kvLine(a, "time", (utcText(t.iridiumUtc - (t.timeSec - m.time), true) + " UTC").c_str(), 90);
        ImGui::Separator();
        ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", m.text.c_str()); ImGui::PopFont();
        if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(m.text.c_str());
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the analysis row

// bursts against time and frequency, frames by type, the newest pager message
void panels(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(120.f, (W - 3 * gap) / 3.f), plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(colW, "Bursts, last 2 s (green: frame decoded)");   // the captions are never wider than their plots
    scatter(a, ImVec2(colW, plotH), false);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Frames by type");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##irtypes", ImVec2(colW, plotH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int nb = dect2::kIridiumTypeSlots;
        uint64_t mx = 1;
        if (on) for (int i = 0; i < nb; i++) mx = std::max(mx, t.typeCount[i]);
        const float bw = colW / nb, base = p.y + plotH - 14 * gUi;
        for (int i = 0; i < nb; i++) {
            const int k = (i + 1) % nb;   // "other" last
            const uint64_t n = on ? t.typeCount[k] : 0;
            const float h = (plotH - 18 * gUi) * (float)n / (float)mx;
            dl->AddRectFilled(ImVec2(p.x + i * bw + 2, base - h), ImVec2(p.x + (i + 1) * bw - 2, base), ImGui::ColorConvertFloat4ToU32(pal::accent(k == 12 ? 0.35f : 0.8f)));
            const char* lb = dect2::iridiumTypeLabel(k);
            char sh[4] = {lb[0], lb[1], lb[2], 0};   // three letters under a bar, fewer when the bars are narrow
            while (sh[0] && ImGui::CalcTextSize(sh).x > bw - 2) sh[strlen(sh) - 1] = 0;
            const ImVec2 ts = ImGui::CalcTextSize(sh);
            if (sh[0]) dl->AddText(ImVec2(p.x + i * bw + (bw - ts.x) * 0.5f, base + 1 * gUi), IM_COL32(150, 154, 158, 255), sh);
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::BeginChild("##irpan3", ImVec2(colW, H - 4));
    timeBox(a);
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Newest pager message"); ImGui::PopTextWrapPos(); }
    if (on && !t.messages.empty()) {
        const auto& m = t.messages.back();
        ImGui::TextDisabled("to %d%s", m.ric, m.complete ? "" : ", incomplete");
        ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", m.text.c_str()); ImGui::PopFont();
    } else {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(on ? "none yet" : "start the receiver"); ImGui::PopTextWrapPos(); }
    }
    ImGui::EndChild();
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    subNav("iriv", S.view, {"Map", "Messages", "Ring alerts", "Bursts"});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 1) { messages(a, W, H); return; }
    if (S.view == 2) { ringAlerts(a); return; }
    if (S.view == 3) {
        scatter(a, ImVec2(W, std::max(160 * gUi, H * 0.62f)));
        ImGui::Spacing();
        ImGui::BeginChild("##iri_bc", ImVec2(0, 0));
        counters(a);
        ImGui::EndChild();
        return;
    }
    const float cw = std::min(340.f * gUi, W * 0.36f);
    ImGui::BeginChild("##iri_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##iri_r", ImVec2(0, H));
    timeBox(a);
    ImGui::Spacing();
    satDetail(a);
    ImGui::Spacing();
    ImGui::TextDisabled("Frames");
    counters(a);
    ImGui::EndChild();
}

void list(App& a) {
    loadState();
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see satellites"); ImGui::PopTextWrapPos(); } return; }
    ImGui::TextDisabled("%zu satellites", a.rx.iridium.sats.size());
    satTable(a, true);
}

void receiver(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    char b[128];
    kvLine(a, "state", t.state == 0 ? "searching" : t.state == 1 ? "bursts, no good frame" : "decoding", 130);
    snprintf(b, sizeof b, "%llu good, %llu failed the codes", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kvLine(a, "frames", b, 130);
    snprintf(b, sizeof b, "%.1f dB Es/N0, offset %+.0f Hz", t.snrDb, t.cfoHz); kvLine(a, "last bursts", b, 130);
    snprintf(b, sizeof b, "%.3f MHz, %.2f Msps", t.centerMhz, t.inputRate / 1e6); kvLine(a, "centre", b, 130);
    snprintf(b, sizeof b, "%llu found, %llu demodulated, %llu repeats", (unsigned long long)t.bursts, (unsigned long long)t.demodulated, (unsigned long long)t.duplicates); kvLine(a, "bursts", b, 130);
    snprintf(b, sizeof b, "%.1f s", t.timeSec); kvLine(a, "signal time", b, 130);
    ImGui::Spacing();
    timeBox(a);
    ImGui::Spacing();
    counters(a);
    ImGui::Spacing();
    satTable(a, false);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const IridiumTelemetry& t = a.rx.iridium;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Bursts", !on ? 0 : t.uwPerSec > 0 ? 1 : t.burstsPerSec > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : t.state == 2 ? 1 : t.blocksOk > 0 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Bursts" : "Searching");
    snprintf(b, sizeof b, "%.0f/s", t.burstsPerSec); ro("Bursts", b);
    snprintf(b, sizeof b, "%zu", t.sats.size()); ro("Satellites", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / bad", b);
    if (t.hasTime) ro("UTC", utcText(t.iridiumUtc, false));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Iridium";
    if (live(a)) l2 = dect2::iridiumSummary(a.rx.iridium);
}

// The engine does not pass the tuned frequency on: the receiver needs it for absolute frequencies and the simplex rule.
void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedCenter = 0; S.pushedThr = -1; return; }
    dect2::IridiumReceiver& r = a.engine.iridium();
    if (S.pushedCenter != a.freqMhz) { r.setCenterMhz(a.freqMhz); S.pushedCenter = a.freqMhz; }
    if (S.pushedThr != S.thrDb) { r.setThresholdDb(S.thrDb); S.pushedThr = S.thrDb; }
}

void decoder(App& a, bool&) {
    loadState();
    (void)a;
    ImGui::TextDisabled("THRESHOLD"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::SliderFloat("##irthr", &S.thrDb, 8.f, 24.f, "%.0f dB")) plat::prefs().setD("iridiumThr", S.thrDb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Burst level over the noise floor needed to demodulate a burst. Lower finds weaker bursts but costs more CPU on noise.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("satellites");
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    static const char* nsat[] = {"1", "2", "3", "4"};
    int n = (sc.modeOpt[0] > 0 ? std::min(4, sc.modeOpt[0]) : 3) - 1;
    if (ImGui::Combo("##irn", &n, nsat, 4)) { sc.modeOpt[0] = n + 1; changed = true; }
    flowNext(); ImGui::TextDisabled("seed"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    int seed = sc.modeOpt[1];
    if (ImGui::InputInt("##irseed", &seed, 0)) { sc.modeOpt[1] = std::max(0, seed); changed = true; }
    flowNext();
    bool quiet = sc.modeOpt[2] != 0;
    if (ImGui::Checkbox("no traffic", &quiet)) { sc.modeOpt[2] = quiet ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave out the voice-like traffic bursts.");
    flowNext(); ImGui::TextDisabled("C/N0"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float cn0 = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 0.f;
    if (ImGui::SliderFloat("##ircn0", &cn0, 0.f, 80.f, cn0 > 0 ? "%.0f dB-Hz" : "from SNR")) { sc.modeVal[0] = cn0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Carrier to noise density of a satellite at the zenith; 0 uses the SNR setting (C/N0 = Es/N0 + 44 dB).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const IridiumTelemetry& t = a.rx.iridium;
    out.push_back({"BURSTS /s", "%.0f", t.burstsPerSec, 0, 400, t.uwPerSec > 0 ? 1 : 0});
    out.push_back({"Es/N0  dB", "%.0f", t.snrDb, 0, 30, t.snrDb >= 15 ? 1 : t.snrDb >= 10 ? 2 : 3});
    out.push_back({"SATELLITES", "%.0f", (double)t.sats.size(), 0, 6, t.sats.empty() ? 0 : 1});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"FRAMES OK  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 90 ? 1 : good >= 60 ? 2 : 3});
}

} // namespace

extern const ModeUi kIridiumUi;
const ModeUi kIridiumUi = {
    .sideTitle = "SATELLITES",
    .tabName = "Iridium",
    .tabIcon = Ic::Globe,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
