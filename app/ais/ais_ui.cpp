// AIS screens: the map of vessels (the ADS-B map underneath), the vessel table, the detail panel, the message counters
// with the rates of the channels (AIS 1, AIS 2, and the long-range channels 75 and 76 when the capture holds them), the raw NMEA log, and the
// Satellite AIS panel (ais_sat_ui.cpp).
#include "../panels/photo_links_ui.h"
#include "ui/kit/session_list.h"
#include "ui/kit/mode_frame.h"
#include "app.h"
#include "state/results_export.h"
#include "../adsb/adsb_map.h"
#include "map/rangeplot_ui.h"
#include "dect2/ais/ais_tel.h"
#include "ais_sat_ui.h"
#include "ais_alerts_ui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace {

using dect2::AisVessel;

struct State {
    bool loaded = false;
    int view = 1;                    // 1 table (Live), 0 map, 2 messages, 3 NMEA, 4 range, 5 satellite
    uint32_t sel = 0;                // the selected station (MMSI), 0 = none
    adsbmap::View map;
    bool mapFollow = true;           // the map fits the stations until the user drags it
    int sortCol = 4, sortDir = 1;    // table: the column and the direction (1 ascending); starts on "heard"
    bool nmeaScroll = true;
    int labels = 0;                  // map labels: 0 from zoom 10 on (and the selected one), 1 off (only the selected and the hovered vessel), 2 always
    RangeView range;                 // the receiver-range plot (Range view)
    uint64_t rangeSeq = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 15; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("aisZoom", 9);
    S.labels = std::max(0, std::min(2, (int)d.getI("aisLabels", 0)));
}

std::string mmsiText(uint32_t m) { char b[16]; snprintf(b, sizeof b, "%09u", m); return b; }

std::string nameOf(const AisVessel& v) { return v.name.empty() ? mmsiText(v.mmsi) : v.name; }

const char* chLabel(char c) { return dect2::aisChannelLabel(dect2::aisChannelIndex(c)); }

std::string utcClock(double t) {
    if (t <= 0) return "-";
    const time_t s = (time_t)t;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[40]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%06.3f UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec + (t - std::floor(t)));
    return b;
}

std::string ageText(float s) {
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

// colour by what the station is: ship types in groups, the other classes by class
ImU32 vesselColour(const AisVessel& v, float alpha = 1.f) {
    int r, g, b;
    switch (v.cls) {
    case dect2::AIS_CLASS_BASE: r = 225; g = 228; b = 232; break;
    case dect2::AIS_CLASS_ATON: r = 240; g = 205; b = 70; break;
    case dect2::AIS_CLASS_SAR: r = 235; g = 70; b = 70; break;
    default: {
        const int t = v.shipType;
        if (t >= 70 && t < 80) { r = 110; g = 205; b = 120; }          // cargo
        else if (t >= 80 && t < 90) { r = 225; g = 100; b = 90; }      // tanker
        else if (t >= 60 && t < 70) { r = 90; g = 150; b = 240; }      // passenger
        else if (t == 30) { r = 240; g = 150; b = 60; }                // fishing
        else if (t == 36 || t == 37) { r = 185; g = 140; b = 240; }    // sailing, pleasure
        else if ((t >= 50 && t < 60) || (t >= 31 && t <= 35)) { r = 80; g = 205; b = 200; }   // tug, pilot, port and service craft
        else if (t >= 40 && t < 50) { r = 240; g = 190; b = 90; }      // high-speed
        else { r = 150; g = 154; b = 158; }                            // not known
    }
    }
    return IM_COL32(r, g, b, (int)(255 * alpha));
}

// a ship seen from above, bow to the heading; stations that sit still are a diamond
void drawShip(ImDrawList* dl, ImVec2 c, float deg, float r, ImU32 fill, bool outline) {
    static const float pts[5][2] = {{0, -1}, {0.55f, -0.35f}, {0.55f, 0.8f}, {-0.55f, 0.8f}, {-0.55f, -0.35f}};
    const float h = deg * (float)M_PI / 180.f, ch = std::cos(h), sh = std::sin(h);
    ImVec2 q[5];
    for (int i = 0; i < 5; i++) q[i] = ImVec2(c.x + (pts[i][0] * ch - pts[i][1] * sh) * r, c.y + (pts[i][0] * sh + pts[i][1] * ch) * r);
    dl->AddConvexPolyFilled(q, 5, fill);
    dl->AddPolyline(q, 5, outline ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, outline ? 1.6f : 1.f);
}

void drawMark(ImDrawList* dl, const AisVessel& v, ImVec2 q, bool sel) {
    const ImU32 col = vesselColour(v);
    const float r = (sel ? 8.f : 7.f) * gUi;
    if (v.cls == dect2::AIS_CLASS_ATON || v.cls == dect2::AIS_CLASS_BASE) {
        const ImVec2 d[4] = {{q.x, q.y - r}, {q.x + r * 0.8f, q.y}, {q.x, q.y + r}, {q.x - r * 0.8f, q.y}};
        dl->AddConvexPolyFilled(d, 4, col);
        dl->AddPolyline(d, 4, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, sel ? 1.6f : 1.f);
        return;
    }
    float dir = -1;
    if (v.heading >= 0 && v.heading < 360) dir = (float)v.heading;
    else if (v.cog >= 0 && v.cog < 360 && v.sog > 0.5f) dir = v.cog;
    if (dir < 0) {
        dl->AddCircleFilled(q, r * 0.6f, col);
        dl->AddCircle(q, r * 0.6f, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), 16, sel ? 1.6f : 1.f);
    } else {
        drawShip(dl, q, dir, r, col, sel);
    }
}

// the first six-bit characters of an !AIVDM sentence carry the type and the MMSI
bool sentenceInfo(const std::string& s, int& type, uint32_t& mmsi) {
    size_t p = 0;
    for (int i = 0; i < 5; i++) { p = s.find(',', p); if (p == std::string::npos) return false; p++; }
    if (s.size() < p + 7) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 7; i++) {
        int c = s[p + i] - 48;
        if (c > 40) c -= 8;
        bits = (bits << 6) | (uint64_t)(c & 63);
    }
    type = (int)(bits >> 36);
    mmsi = (uint32_t)((bits >> 4) & 0x3FFFFFFFu);
    return true;
}

const dect2::AisVessel* findVessel(const dect2::AisTelemetry& t, uint32_t mmsi) {
    if (!mmsi) return nullptr;
    for (const auto& v : t.vessels) if (v.mmsi == mmsi) return &v;
    return nullptr;
}

const char* msgName(int t) {
    static const char* const n[28] = {"Other types", "Position report A", "Position report A", "Position report A", "Base station report",
        "Static and voyage data", "Addressed binary", "Binary acknowledge", "Broadcast binary", "SAR aircraft position", "UTC inquiry",
        "UTC response", "Addressed safety text", "Safety acknowledge", "Safety broadcast", "Interrogation", "Assignment command",
        "DGNSS broadcast", "Position report B", "Position report B, extended", "Data link management", "Aid to navigation",
        "Channel management", "Group assignment", "Static data B", "Single slot binary", "Multi slot binary", "Long range position"};
    return t >= 0 && t < 28 ? n[t] : "";
}

// ---------------------------------------------------------------- the detail panel

void detail(App& a) {
    const dect2::AisTelemetry& t = a.rx.ais;
    const AisVessel* v = live(a) ? findVessel(t, S.sel) : nullptr;
    if (!v) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !live(a) ? "start the receiver" : t.vessels.empty() ? "no stations heard yet" : "click a station"); ImGui::PopTextWrapPos(); }
        return;
    }
    ImGui::TextColored(pal::heading(), "%s", nameOf(*v).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dect2::aisClassText(v->cls));
    char b[96];
    auto kv = [&](const char* k, const char* s) {
        ImGui::TextDisabled("%s", k); kvColumn(96 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(s); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    kv("MMSI", mmsiText(v->mmsi).c_str());
    if (!v->callsign.empty()) kv("call sign", v->callsign.c_str());
    if (v->imo) { snprintf(b, sizeof b, "%u", v->imo); kv("IMO", b); }
    if (v->shipType >= 0) { snprintf(b, sizeof b, "%s (%d)", dect2::aisShipTypeText(v->shipType), v->shipType); kv("type", b); }
    if (v->cls == dect2::AIS_CLASS_ATON && v->aidType >= 0) {
        snprintf(b, sizeof b, "type %d%s%s", v->aidType, v->virtualAid ? ", virtual" : "", v->offPosition ? ", off position" : "");
        kv("aid", b);
    }
    if (v->navStatus >= 0) kv("status", dect2::aisNavStatusText(v->navStatus));
    if (v->longRange) {
        snprintf(b, sizeof b, "message 27 x %u%s", v->longRangeMsgs, v->posLatency == 0 ? ", GNSS latency under 5 s" : v->posLatency == 1 ? ", GNSS latency 5 s or more" : "");
        kv("long range", b);
    }
    photolinks::shipButton(v->mmsi, v->imo, v->name);
    ImGui::Spacing();
    if (v->hasPos) { snprintf(b, sizeof b, v->lrPosition ? "%.4f, %.4f (1/10 min)" : "%.5f, %.5f", v->lat, v->lon); kv("position", b); } else kv("position", "-");
    snprintf(b, sizeof b, "%.1f kn", v->sog); kv("speed", v->sog >= 0 ? b : "-");
    snprintf(b, sizeof b, "%.1f deg", v->cog); kv("course", v->cog >= 0 ? b : "-");
    snprintf(b, sizeof b, "%d deg", v->heading); kv("heading", v->heading >= 0 && v->heading < 360 ? b : "-");
    if (v->hasRot) { snprintf(b, sizeof b, "%+.1f deg/min", v->rotDegMin); kv("turn", b); }
    if (v->altitudeM >= 0) { snprintf(b, sizeof b, "%d m", v->altitudeM); kv("altitude", b); }
    if (v->cls == dect2::AIS_CLASS_A || v->cls == dect2::AIS_CLASS_B || v->cls == dect2::AIS_CLASS_OTHER) {
        ImGui::Spacing();
        if (!v->destination.empty()) kv("destination", v->destination.c_str());
        if (v->etaMonth > 0 && v->etaDay > 0) {
            if (v->etaHour < 24) snprintf(b, sizeof b, "%02d-%02d %02d:%02d UTC", v->etaMonth, v->etaDay, v->etaHour, v->etaMin < 60 ? v->etaMin : 0);
            else snprintf(b, sizeof b, "%02d-%02d", v->etaMonth, v->etaDay);
            kv("ETA", b);
        }
        if (v->draughtM > 0) { snprintf(b, sizeof b, "%.1f m", v->draughtM); kv("draught", b); }
        if (v->dimA + v->dimB > 0 || v->dimC + v->dimD > 0) { snprintf(b, sizeof b, "%d x %d m", v->dimA + v->dimB, v->dimC + v->dimD); kv("length x beam", b); }
    }
    ImGui::Spacing();
    snprintf(b, sizeof b, "%u, last type %d, channel %s", v->messages, v->lastType, chLabel(v->channel)); kv("messages", b);
    kv("last heard", (ageText(v->ageSec) + " ago").c_str());
    if (v->lastUtc > 0) kv("received", utcClock(v->lastUtc).c_str());
    if (v->hasPos && ImGui::SmallButton("copy position")) { snprintf(b, sizeof b, "%.6f, %.6f", v->lat, v->lon); ImGui::SetClipboardText(b); }
    ImGui::Spacing();
    ImGui::TextDisabled("Last messages");
    int shown = 0;
    ImGui::PushFont(a.mono, 0);
    for (auto it = t.nmea.rbegin(); it != t.nmea.rend() && shown < 4; ++it) {
        int ty; uint32_t m;
        if (!sentenceInfo(*it, ty, m) || m != v->mmsi) continue;
        ImGui::TextWrapped("%d  %s", ty, it->c_str());
        shown++;
    }
    ImGui::PopFont();
    if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("none in the last 50"); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- the map

void mapView(App& a, ImVec2 size) {
    const dect2::AisTelemetry& t = a.rx.ais;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the stations that have a position, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& v : t.vessels) if (v.hasPos) { la0 = std::min(la0, v.lat); la1 = std::max(la1, v.lat); lo0 = std::min(lo0, v.lon); lo1 = std::max(lo1, v.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(0.005, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(12, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool mouseIn = ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + size.x, p0.y + size.y));
    const AisVessel* hit = nullptr; float best = 16 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    if (on) {
        for (const auto& v : t.vessels) {   // tracks first, so the marks sit on top
            if (!aisUiFilter().matches(v)) continue;
            if (!v.hasPos || v.track.size() < 2) continue;
            ImVec2 prev = adsbmap::project(v.track[0].lat, v.track[0].lon);
            for (size_t i = 1; i < v.track.size(); i++) {
                const ImVec2 q = adsbmap::project(v.track[i].lat, v.track[i].lon);
                dl->AddLine(prev, q, vesselColour(v, 0.45f), 1.5f);
                prev = q;
            }
            dl->AddLine(prev, adsbmap::project(v.lat, v.lon), vesselColour(v, 0.45f), 1.5f);
        }
        for (const auto& v : t.vessels) {
            if (!v.hasPos || !aisUiFilter().matches(v)) continue;
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
            const bool sel = v.mmsi == S.sel;
            drawMark(dl, v, q, sel);
            if (v.longRange) {   // long-range marker: a dashed ring (message 27 heard from this ship)
                const float rr = 11.f * gUi;
                for (int k = 0; k < 12; k += 2) { dl->PathArcTo(q, rr, k * (float)M_PI / 6, (k + 1) * (float)M_PI / 6, 4); dl->PathStroke(IM_COL32(245, 150, 60, 230), 0, 1.6f); }
            }
            const bool hovered = S.labels == 1 && mouseIn && std::hypot(q.x - mouse.x, q.y - mouse.y) < 14 * gUi;   // labels off: the vessel under the pointer still names itself
            if (sel || hovered || S.labels == 2 || (S.labels == 0 && S.map.zoom >= 10)) {
                const std::string s = nameOf(v);
                const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
                const ImVec2 lp(q.x + 12 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
                dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
                dl->AddRect(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), s.c_str());
            }
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &v; }
        }
    }
    if (clicked) S.sel = hit ? hit->mmsi : 0;
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::SameLine(0, 6 * gUi);
    {   // the name / MMSI boxes beside the vessels: crowded harbours want them off
        static const char* kLab[] = {"labels: zoomed in", "labels: off", "labels: all"};
        if (ImGui::Button(tr(kLab[S.labels]))) { S.labels = (S.labels + 1) % 3; plat::prefs().setI("aisLabels", S.labels); plat::prefs().flush(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Name or MMSI boxes on the map. Zoomed in: from zoom 10 on. Off: only the selected vessel and the one under the pointer. All: every vessel. Click to change."));
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("aisZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& v : t.vessels) if (v.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no positions yet");
    else if (t.longRangeVessels) adsbmap::legend(size.x, "%d of %u stations with a position; %u long range (dashed ring, message 27)", withPos, t.vesselCount, t.longRangeVessels);
    else adsbmap::legend(size.x, "%d of %u stations with a position", withPos, t.vesselCount);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- the table

void vesselTable(App& a, bool compact) {
    const dect2::AisTelemetry& t = a.rx.ais;
    aisFilterBar(a);
    if (aisUiFilter().active()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("some stations are hidden")); }
    const int cols = compact ? 3 : 7;
    if (!compact) {
        static std::string note;
        resexp::Table x; x.cols = {"time", "frequency_hz", "mode", "name", "mmsi", "type", "sog_kn", "lat", "lon", "heard_s_ago", "channel", "long_range", "received_unix"};
        const std::string now = resexp::stamp();
        for (const auto& v : t.vessels)
            x.add({now, resexp::num(dect2::aisChannelHz(dect2::aisChannelIndex(v.channel)), 0), "AIS", nameOf(v), std::to_string(v.mmsi), v.shipType >= 0 ? dect2::aisShipTypeText(v.shipType) : dect2::aisClassText(v.cls),
                   v.sog >= 0 ? resexp::num(v.sog, 1) : "", v.hasPos ? resexp::num(v.lat, 6) : "", v.hasPos ? resexp::num(v.lon, 6) : "", resexp::num(v.ageSec, 0), chLabel(v.channel),
                   v.longRange ? "1" : "0", v.lastUtc > 0 ? resexp::num(v.lastUtc, 3) : ""}, v.lastUtc);
        resexp::saveButton(x, "ais", note, "svais");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable;
    if (!ImGui::BeginTable(compact ? "##ais_s" : "##ais_f", cols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0, 0);
    if (!compact) { ImGui::TableSetupColumn("MMSI", 0, 0, 1); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0, 2); }
    ImGui::TableSetupColumn("SOG kn", 0, 0, 3);
    ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_DefaultSort, 0, 4);
    if (!compact) { ImGui::TableSetupColumn("Pos", 0, 0, 5); ImGui::TableSetupColumn("Ch", 0, 0, 6); }
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
        if (sp->SpecsCount > 0) { S.sortCol = (int)sp->Specs[0].ColumnUserID; S.sortDir = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending ? -1 : 1; }
    }
    std::vector<const AisVessel*> rows;
    const dect2::AisFilter& flt = aisUiFilter();
    for (const auto& v : t.vessels) if (flt.matches(v)) rows.push_back(&v);
    auto key = [&](const AisVessel* x, const AisVessel* y) -> int {
        switch (S.sortCol) {
        case 0: return strcasecmp(nameOf(*x).c_str(), nameOf(*y).c_str());
        case 1: return x->mmsi < y->mmsi ? -1 : x->mmsi > y->mmsi;
        case 2: return x->shipType < y->shipType ? -1 : x->shipType > y->shipType;
        case 3: return x->sog < y->sog ? -1 : x->sog > y->sog;
        case 4: return x->ageSec < y->ageSec ? -1 : x->ageSec > y->ageSec;
        case 5: return (int)x->hasPos - (int)y->hasPos;
        default: return x->channel < y->channel ? -1 : x->channel > y->channel;
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const AisVessel* x, const AisVessel* y) { return key(x, y) * S.sortDir < 0; });
    for (const AisVessel* v : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)v->mmsi);
        const std::string nm = nameOf(*v);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 4 * gUi, c.y + ImGui::GetTextLineHeight() * 0.5f), 3.f * gUi, vesselColour(*v));
        ImGui::SetCursorScreenPos(ImVec2(c.x + 12 * gUi, c.y));
        if (ImGui::Selectable(nm.c_str(), v->mmsi == S.sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = v->mmsi;
        if (ImGui::BeginPopupContextItem()) { photolinks::shipMenuItems(v->mmsi, v->imo, v->name); ImGui::EndPopup(); }
        ImGui::PopID();
        char b[32];
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mmsiText(v->mmsi).c_str());
            ImGui::TableNextColumn();
            if (v->shipType >= 0) ImGui::TextUnformatted(dect2::aisShipTypeText(v->shipType)); else ImGui::TextDisabled("%s", dect2::aisClassText(v->cls));
        }
        ImGui::TableNextColumn();
        if (v->sog >= 0) ImGui::Text("%.1f", v->sog); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(v->ageSec).c_str());
        if (!compact) {
            ImGui::TableNextColumn();
            if (v->hasPos && v->lrPosition) ImGui::TextColored(ImVec4(0.96f, 0.6f, 0.25f, 1), "LR"); else ImGui::TextUnformatted(v->hasPos ? "yes" : "-");
            if (v->longRange && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "sends message 27 (long-range AIS)");
            ImGui::TableNextColumn(); snprintf(b, sizeof b, "%s", chLabel(v->channel)); ImGui::TextUnformatted(b);
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- counters and the log

void counters(App& a) {
    const dect2::AisTelemetry& t = a.rx.ais;
    int chs[4], nc = 0;
    for (int c = 0; c < 4; c++) if ((t.channelMask >> c & 1) || t.channelOk[c]) chs[nc++] = c;
    if (!nc) { chs[0] = 0; chs[1] = 1; nc = 2; }
    if (ImGui::BeginTable("##ais_cnt", nc + 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        static const char* const hd[4] = {"AIS 1", "AIS 2", "Ch 75", "Ch 76"};
        for (int i = 0; i < nc; i++) ImGui::TableSetupColumn(hd[chs[i]]);
        ImGui::TableSetupColumn("Total");
        ImGui::TableHeadersRow();
        for (int k = 1; k <= 32; k++) {
            const int ty = k == 32 ? 0 : k;
            if (ty >= 28) continue;
            uint64_t tot = 0;
            for (int i = 0; i < nc; i++) tot += t.typeCount[chs[i]][ty];
            if (!tot) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ty) ImGui::Text("%d  %s", ty, msgName(ty)); else ImGui::TextUnformatted("28 and up");
            for (int i = 0; i < nc; i++) { ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)t.typeCount[chs[i]][ty]); }
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)tot);
        }
        ImGui::EndTable();
    }
}

void channelStats(App& a) {
    const dect2::AisTelemetry& t = a.rx.ais;
    if (ImGui::BeginTable("##ais_ch", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Good");
        ImGui::TableSetupColumn("Bad"); ImGui::TableSetupColumn("per min"); ImGui::TableSetupColumn("Level dBFS"); ImGui::TableSetupColumn("Noise dBFS");
        ImGui::TableHeadersRow();
        for (int c = 0; c < 4; c++) {
            if (!(t.channelMask >> c & 1) && !t.channelOk[c]) continue;   // a pair the capture does not hold
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(dect2::aisChannelName(c)); ImGui::PopTextWrapPos();   // the stretched column of a narrow table wraps
            ImGui::TableNextColumn(); ImGui::Text("%.3f", dect2::aisChannelHz(c) / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)t.channelOk[c]);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)t.channelBad[c]);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", t.burstsPerMin[c]);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", t.levelDbfs[c]);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", t.noiseDbfs[c]);
        }
        ImGui::EndTable();
    }
}

void nmeaView(App& a) {
    const dect2::AisTelemetry& t = a.rx.ais;
    if (ImGui::SmallButton("copy all")) {
        std::string all;
        for (const auto& s : t.nmea) { all += s; all += '\n'; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("follow", &S.nmeaScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("the last %zu good messages", t.nmea.size());
    ImGui::BeginChild("##ais_nmea", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : t.nmea) ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
    if (t.nmea.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing decoded yet"); ImGui::PopTextWrapPos(); }
    if (S.nmeaScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    kit::modeHeader({14, "AIS", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("aisv", S.view, {{tr("Live"), 1}, {tr("Tracks"), 6}, {tr("Map"), 0}, {tr("Log"), 2, true}, {tr("NMEA"), 3, true}, {tr("Range"), 4, true}, {tr("Satellite"), 5, true}, {tr("Alerts"), 7, true}});
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 2) {
        if (on) { channelStats(a); ImGui::Spacing(); }
        counters(a);
        return;
    }
    if (S.view == 3) { nmeaView(a); return; }
    if (S.view == 4) { rangeView(S.range, ImGui::GetContentRegionAvail(), 0.5399568, "nm", "ais", receiverLocationSet()); return; }
    if (S.view == 5) { aisSatPanel(a); return; }
    if (S.view == 6) { kit::sessionItems(14, 3, "ais"); return; }
    if (S.view == 7) { aisAlertsPanel(a); return; }
    const float cw = std::min(320.f * gUi, W * 0.34f);
    ImGui::BeginChild("##ais_l", ImVec2(W - cw - 8 * gUi, H));
    if (S.view == 0) {
        aisFilterBar(a);
        if (aisUiFilter().active()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("some stations are hidden")); }
        mapView(a, ImGui::GetContentRegionAvail());
    }
    else vesselTable(a, false);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ais_r", ImVec2(0, H));
    detail(a);
    ImGui::EndChild();
}

void list(App& a) {
    loadState();
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see vessels"); ImGui::PopTextWrapPos(); } return; }
    if (resultsClearButton("ais")) a.engine.ais().clearResults();
    ImGui::TextDisabled("%u stations", a.rx.ais.vesselCount);
    vesselTable(a, true);
}

void receiver(App& a) {
    const dect2::AisTelemetry& t = a.rx.ais;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 0 ? "searching" : t.state == 1 ? "signal, no good message" : "decoding");
    kv("messages", "%llu good, %llu failed the CRC", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("bursts", "%llu detected", (unsigned long long)t.bursts);
    kv("signal", "%.1f dB in 48 kHz, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    if (t.satellite) kv("satellite mode", "Doppler %+.0f Hz (%+.1f Hz/s); %llu recovered, %llu after cancelling", t.dopplerHz[0], t.dopplerRateHzS[0],
                        (unsigned long long)t.recovered, (unsigned long long)t.cancelled);
    if (t.longRangeVessels) kv("long range", "%u ships sent message 27", t.longRangeVessels);
    kv("stations", "%u in the table", t.vesselCount);
    kv("signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    channelStats(a);
    ImGui::Spacing();
    ImGui::TextDisabled("Messages by type");
    counters(a);
}

// every frame while AIS is selected: the positions heard, for the range plot (from the receiver position)
void tick(App& a) {
    aisSatTick(a);
    aisAlertsTick(a);
    if (!live(a) || a.rx.seq == S.rangeSeq) return;
    S.rangeSeq = a.rx.seq;
    double lat, lon;
    receiverLocation(lat, lon);
    rangeSetStation(S.range, lat, lon);
    for (const auto& v : a.rx.ais.vessels) if (v.hasPos) rangeFeed(S.range, v.lat, v.lon);
}

void tuner(App& a, bool& retune) { aisChannelControls(a, retune); }

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "AIS";
    if (live(a)) l2 = dect2::aisSummary(a.rx.ais);
}

} // namespace

extern const ModeUi kAisUi;
const ModeUi kAisUi = {
    .sideTitle = "VESSELS",
    .tabName = "Ships",
    .tabIcon = Ic::Compass,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
