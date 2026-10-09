// ADS-B screens: the aircraft table, a radar plot around the receiver, the message monitor, statistics and the options of the test signal.
#include "app.h"
#include "adsb_map.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    double refLat = 25.25, refLon = 55.36;      // where the antenna is: ranges and local decoding; the default is only a placeholder
    double pushedLat = 1e9, pushedLon = 1e9;
    uint32_t sel = 0xFFFFFFFFu;                 // the selected aircraft (ICAO address)
    int correction = 1;
    std::deque<float> rate, snr;                // good messages per second and SNR, a point per report
    uint64_t lastSeq = 0;
    int viewMode = 0;                           // 0 map, 1 table, 2 radar
    adsbmap::View map;
    bool mapHome = true;                        // the map follows the antenna position until the user drags it
    std::string locMsg;
    int locBusy = 0;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 12; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("adsbLat")) S.refLat = d.getD("adsbLat", S.refLat);
    if (d.has("adsbLon")) S.refLon = d.getD("adsbLon", S.refLon);
    S.map.zoom = (int)d.getI("adsbZoom", 7);
    S.map.lat = S.refLat; S.map.lon = S.refLon;
}

const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1), kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1);

std::string icaoText(uint32_t i) { char b[16]; snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }

void setReference(App& a, double lat, double lon) {
    S.refLat = std::max(-90.0, std::min(90.0, lat)); S.refLon = std::max(-180.0, std::min(180.0, lon));
    plat::prefs().setD("adsbLat", S.refLat); plat::prefs().setD("adsbLon", S.refLon);
    S.map.lat = S.refLat; S.map.lon = S.refLon;
    savePrefs(a);
}

void tick(App& a) {
    loadState();
    if (S.locBusy) {   // the location request of the Locate button
        double la = 0, lo = 0, acc = 0; std::string msg;
        const int st = plat::locateState(la, lo, acc, msg);
        if (st == 2) { setReference(a, la, lo); char b[96]; snprintf(b, sizeof b, "position found (about %.0f m)", acc); S.locMsg = b; S.locBusy = 0; }
        else if (st == 3) { S.locMsg = msg; S.locBusy = 0; }
    }
    a.tune.synth.modeVal[2] = S.refLat; a.tune.synth.modeVal[3] = S.refLon;   // the simulated traffic flies around the reference point
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedLat != S.refLat || S.pushedLon != S.refLon)) {
        a.engine.adsb().setReference(S.refLat, S.refLon);
        a.engine.adsb().setCorrection(S.correction);
        S.pushedLat = S.refLat; S.pushedLon = S.refLon;
    }
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const AdsbTelemetry& t = a.rx.adsb;
        S.rate.push_back(t.msgsPerSec); if (S.rate.size() > 240) S.rate.pop_front();
        if (t.dataValid) { S.snr.push_back(t.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
    }
}

// one number or a dash
std::string num(bool has, const char* fmt, double v) { if (!has) return "-"; char b[32]; snprintf(b, sizeof b, fmt, v); return b; }

void aircraftTable(App& a, bool compact) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const int cols = compact ? 5 : 11;
    if (!ImGui::BeginTable(compact ? "##acs" : "##acf", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 62 * gUi); ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthFixed, 72 * gUi);
    ImGui::TableSetupColumn("Alt ft"); if (!compact) { ImGui::TableSetupColumn("Spd kt"); ImGui::TableSetupColumn("Hdg"); ImGui::TableSetupColumn("V/S"); }
    ImGui::TableSetupColumn("Dist nm");
    if (!compact) { ImGui::TableSetupColumn("Sqk"); ImGui::TableSetupColumn("Msgs"); ImGui::TableSetupColumn("Age s"); ImGui::TableSetupColumn("dBFS"); }
    ImGui::TableHeadersRow();
    std::vector<const AdsbAircraft*> rows;   // by address, not by last heard: the rows do not move about
    if (on) for (const auto& x : t.aircraft) rows.push_back(&x);
    std::sort(rows.begin(), rows.end(), [](const AdsbAircraft* x, const AdsbAircraft* y) { return x->icao < y->icao; });
    for (const AdsbAircraft* pac : rows) {
        const AdsbAircraft& ac = *pac;
        ImGui::TableNextRow();
        const bool emg = ac.emergency > 0;
        if (emg) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        ImGui::TableNextColumn();
        char id[24]; snprintf(id, sizeof id, "%s##%u", icaoText(ac.icao).c_str(), ac.icao);
        if (ImGui::Selectable(id, S.sel == ac.icao, ImGuiSelectableFlags_SpanAllColumns)) S.sel = ac.icao;
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "-" : ac.callsign.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.ground ? "ground" : num(ac.hasAlt, "%.0f", ac.altFt).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasSpeed, "%.0f", ac.speedKt).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasHeading, "%.0f", ac.headingDeg).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasVrate, "%+.0f", ac.vrateFpm).c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasRange, "%.1f", ac.distNm).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); if (ac.hasSquawk) ImGui::Text("%04d", ac.squawk); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", ac.messages);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.ageSec);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.levelDbfs);
        }
        if (emg) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

// the radar: the receiver in the middle, north up, a ring every few tens of nautical miles
void radar(App& a, ImVec2 size) {
    const AdsbTelemetry& t = a.rx.adsb;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##radar", size);
    const float side = std::min(size.x, size.y), R = side * 0.5f - 6 * gUi;
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    double maxNm = 20;
    const bool on = live(a);
    if (on) for (const auto& ac : t.aircraft) if (ac.hasRange) maxNm = std::max<double>(maxNm, ac.distNm * 1.1);
    static const double steps[] = {20, 50, 100, 150, 200, 300, 400, 600};
    double scale = steps[7];
    for (double s : steps) if (s >= maxNm) { scale = s; break; }
    const int rings = scale <= 20 ? 4 : scale <= 50 ? 5 : 4;
    const ImU32 grid = IM_COL32(60, 64, 66, 255);
    for (int i = 1; i <= rings; i++) {
        dl->AddCircle(c, R * i / rings, grid, 64);
        char b[24]; snprintf(b, sizeof b, "%.0f nm", scale * i / rings);
        dl->AddText(ImVec2(c.x + 3 * gUi, c.y - R * i / rings), IM_COL32(110, 114, 118, 255), b);
    }
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), grid); dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), grid);
    dl->AddText(ImVec2(c.x - 3 * gUi, c.y - R - ImGui::GetTextLineHeight() - 1), IM_COL32(130, 134, 138, 255), "N");
    dl->AddCircleFilled(c, 3 * gUi, ImGui::ColorConvertFloat4ToU32(pal::accent()));
    if (!on) return;
    const double k = std::cos(S.refLat * M_PI / 180.0);
    auto pt = [&](double lat, double lon) { return ImVec2(c.x + (float)((lon - S.refLon) * k * 60.0 / scale * R), c.y - (float)((lat - S.refLat) * 60.0 / scale * R)); };
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        const bool sel = ac.icao == S.sel;
        const ImU32 col = ac.emergency ? ImGui::ColorConvertFloat4ToU32(pal::badRed()) : sel ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(pal::accent());
        ImVec2 prev;
        bool first = true;
        for (const auto& tp : ac.track) { const ImVec2 q = pt(tp.lat, tp.lon); if (!first) dl->AddLine(prev, q, IM_COL32(70, 74, 78, 255)); prev = q; first = false; }
        const ImVec2 q = pt(ac.lat, ac.lon);
        if (std::fabs(q.x - c.x) > R * 1.1f || std::fabs(q.y - c.y) > R * 1.1f) continue;
        if (ac.hasHeading) { const float h = (float)(ac.headingDeg * M_PI / 180.0); dl->AddLine(q, ImVec2(q.x + std::sin(h) * 10 * gUi, q.y - std::cos(h) * 10 * gUi), col, 1.5f); }
        dl->AddCircleFilled(q, (sel ? 4.f : 3.f) * gUi, col);
        char b[48]; snprintf(b, sizeof b, "%s", ac.callsign.empty() ? icaoText(ac.icao).c_str() : ac.callsign.c_str());
        dl->AddText(ImVec2(q.x + 6 * gUi, q.y - 6 * gUi), IM_COL32(190, 194, 198, 255), b);
    }
    if (!t.refValid) dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + 4 * gUi), IM_COL32(150, 154, 158, 255), "no reference position");
}


// altitude colours, as on the flight-tracking sites: low and warm, high and cool
ImU32 altColour(const AdsbAircraft& ac, float alpha = 1.f) {
    static const float stops[5][3] = {{0.95f, 0.50f, 0.20f}, {0.55f, 0.85f, 0.40f}, {0.30f, 0.85f, 0.75f}, {0.35f, 0.60f, 0.95f}, {0.72f, 0.52f, 0.95f}};
    if (ac.emergency) return IM_COL32(235, 70, 70, (int)(255 * alpha));
    if (ac.ground) return IM_COL32(140, 144, 148, (int)(255 * alpha));
    float v = ac.hasAlt ? std::max(0.f, std::min(3.999f, ac.altFt / 10000.f)) : 1.5f;
    const int i = (int)v; const float f = v - i;
    auto c = [&](int k) { return (int)(255 * (stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f)); };
    return IM_COL32(c(0), c(1), c(2), (int)(255 * alpha));
}

// an airliner seen from above, nose to the heading
void drawPlane(ImDrawList* dl, ImVec2 c, float headingDeg, float r, ImU32 fill, bool outline) {
    static const float pts[][2] = {{0, -1}, {0.09f, -0.75f}, {0.10f, -0.15f}, {1.0f, 0.30f}, {1.0f, 0.46f}, {0.10f, 0.22f}, {0.09f, 0.68f}, {0.40f, 0.90f}, {0.40f, 1.0f}, {0, 0.92f},
                                   {-0.40f, 1.0f}, {-0.40f, 0.90f}, {-0.09f, 0.68f}, {-0.10f, 0.22f}, {-1.0f, 0.46f}, {-1.0f, 0.30f}, {-0.10f, -0.15f}, {-0.09f, -0.75f}};
    const int n = (int)(sizeof pts / sizeof *pts);
    const float h = headingDeg * (float)M_PI / 180.f, ch = std::cos(h), sh = std::sin(h);
    ImVec2 q[18];
    for (int i = 0; i < n; i++) q[i] = ImVec2(c.x + (pts[i][0] * ch - pts[i][1] * sh) * r, c.y + (pts[i][0] * sh + pts[i][1] * ch) * r);
    dl->AddConcavePolyFilled(q, n, fill);
    dl->AddPolyline(q, n, outline ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, outline ? 1.6f : 1.f);
}

// The antenna position is not where the receiver is (still the placeholder, or typed wrong): every aircraft with a position is further
// away than any 1090 MHz signal carries (500 nm; real ranges end near 250). Then lat/lon get the middle of the aircraft (median, so one
// bad fix does not pull it), which is within about 100 nm of the receiver: good enough for the map, not for exact ranges.
bool positionLooksWrong(const AdsbTelemetry& t, double& lat, double& lon, double& nearestNm) {
    std::vector<double> las, los;
    nearestNm = 1e9;
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        las.push_back(ac.lat); los.push_back(ac.lon);
        const double p1 = S.refLat * M_PI / 180, p2 = ac.lat * M_PI / 180, dl = (ac.lon - S.refLon) * M_PI / 180;
        const double c = std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl);
        nearestNm = std::min(nearestNm, std::acos(std::max(-1.0, std::min(1.0, c))) * 3440.065);   // great circle, Earth radius in nm
    }
    if (las.size() < 3 || nearestNm < 500) return false;
    std::nth_element(las.begin(), las.begin() + las.size() / 2, las.end());
    std::nth_element(los.begin(), los.begin() + los.size() / 2, los.end());
    lat = las[las.size() / 2]; lon = los[los.size() / 2];
    return true;
}

void mapView(App& a, ImVec2 size) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    double areaLat = 0, areaLon = 0, nearest = 0;
    const bool wrongRef = on && positionLooksWrong(t, areaLat, areaLon, nearest);
    if (wrongRef && S.mapHome) { S.map.lat = areaLat; S.map.lon = areaLon; }   // show the aircraft, not an empty map around the wrong antenna
    const double homeLat = S.map.lat, homeLon = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != homeLat || S.map.lon != homeLon) S.mapHome = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    // the antenna
    {
        const ImVec2 h = adsbmap::project(S.refLat, S.refLon);
        dl->AddCircle(h, 6 * gUi, IM_COL32(255, 255, 255, 220), 24, 1.5f);
        dl->AddCircleFilled(h, 2.5f * gUi, IM_COL32(255, 255, 255, 220));
    }
    const AdsbAircraft* hit = nullptr; float best = 16 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    if (on) {
        for (const auto& ac : t.aircraft) {   // trails first, so the icons sit on top
            if (!ac.hasPos || ac.track.size() < 2) continue;
            ImVec2 prev = adsbmap::project(ac.track[0].lat, ac.track[0].lon);
            for (size_t i = 1; i < ac.track.size(); i++) {
                const ImVec2 q = adsbmap::project(ac.track[i].lat, ac.track[i].lon);
                if (std::fabs(ac.track[i].lat - ac.track[i - 1].lat) < 2 && std::fabs(ac.track[i].lon - ac.track[i - 1].lon) < 2) dl->AddLine(prev, q, altColour(ac, 0.45f), 1.5f);   // a jump is a bad fix, not a flight
                prev = q;
            }
            if (std::fabs(ac.track.back().lat - ac.lat) < 2 && std::fabs(ac.track.back().lon - ac.lon) < 2) dl->AddLine(prev, adsbmap::project(ac.lat, ac.lon), altColour(ac, 0.45f), 1.5f);
        }
        for (const auto& ac : t.aircraft) {
            if (!ac.hasPos) continue;
            const ImVec2 q = adsbmap::project(ac.lat, ac.lon);
            if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
            const bool sel = ac.icao == S.sel;
            drawPlane(dl, q, ac.hasHeading ? (float)ac.headingDeg : 0.f, (sel ? 14.f : 12.f) * gUi, altColour(ac), sel);
            if (S.map.zoom >= 6 || sel) {
                char b[48]; snprintf(b, sizeof b, "%s", ac.callsign.empty() ? icaoText(ac.icao).c_str() : ac.callsign.c_str());
                char b2[24] = ""; if (ac.hasAlt && !ac.ground) snprintf(b2, sizeof b2, "FL%03d", (ac.altFt + 50) / 100);
                const ImVec2 ts = ImGui::CalcTextSize(b), ts2 = ImGui::CalcTextSize(b2);
                const float w = std::max(ts.x, ts2.x) + 8 * gUi, hh = ts.y + (b2[0] ? ts2.y : 0) + 4 * gUi;
                const ImVec2 lp(q.x + 12 * gUi, q.y - hh * 0.5f);
                dl->AddRectFilled(lp, ImVec2(lp.x + w, lp.y + hh), IM_COL32(10, 12, 14, 210), 2.f);
                dl->AddRect(lp, ImVec2(lp.x + w, lp.y + hh), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), b);
                if (b2[0]) dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi + ts.y), altColour(ac), b2);
            }
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &ac; }
        }
    }
    if (clicked) S.sel = hit ? hit->icao : 0xFFFFFFFFu;
    dl->PopClipRect();
    // controls in the corner
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("antenna")) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.mapHome = true; }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls(nullptr);
    if (wrongRef) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 38 * gUi));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.14f, 0.04f, 0.92f));
        ImGui::BeginChild("##wrongref", ImVec2(std::min(size.x - 16 * gUi, 520 * gUi), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "The antenna position (%.2f, %.2f) is %.0f nm from the nearest aircraft: it is probably not set, so ranges are wrong.", S.refLat, S.refLon, nearest);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Use the aircraft's area")) setReference(a, areaLat, areaLon);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the antenna in the middle of the aircraft received (within about 100 nm). For exact ranges type your position in the Tuner section.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    plat::prefs().setI("adsbZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void detail(const App& a) {
    // the readout of the selected aircraft is refreshed once a second and always has the same rows, so that it does not jump about
    static AdsbAircraft shown; static bool have = false; static double at = 0; static uint32_t of = 0;
    const AdsbTelemetry& t = a.rx.adsb;
    const AdsbAircraft* cur = nullptr;
    if (live(a)) for (const auto& x : t.aircraft) if (x.icao == S.sel) cur = &x;
    if (!cur) { have = false; ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click an aircraft"); ImGui::PopTextWrapPos(); return; }
    const double now = ImGui::GetTime();
    if (!have || of != cur->icao || now - at > 1.0) { shown = *cur; have = true; of = cur->icao; at = now; }
    const AdsbAircraft* ac = &shown;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    char b[96];
    kv("ICAO", icaoText(ac->icao) + (ac->icao >> 24 ? " (not ICAO)" : ""));
    kv("callsign", ac->callsign.empty() ? "-" : ac->callsign + (ac->category.empty() ? "" : "  (" + ac->category + ")"));
    if (ac->hasPos) snprintf(b, sizeof b, "%.4f  %.4f", ac->lat, ac->lon); else snprintf(b, sizeof b, "-");
    kv("position", b);
    kv("fix", ac->hasPos ? (ac->posKind == 2 ? "global" : ac->posKind == 3 ? "local, confirmed" : "local") : "-");
    if (ac->hasRange) snprintf(b, sizeof b, "%.1f nm at %.0f deg", ac->distNm, ac->bearingDeg); else snprintf(b, sizeof b, "-");
    kv("range", b);
    kv("altitude", ac->ground ? "on the ground" : ac->hasAlt ? num(true, "%.0f ft", ac->altFt) : "-");
    kv("GNSS height", ac->hasGeoAlt ? num(true, "%.0f ft", ac->geoAltFt) : "-");
    kv("speed", ac->hasSpeed ? num(true, "%.0f kt", ac->speedKt) + (ac->speedKind == 0 ? " ground" : ac->speedKind == 1 ? " ias" : " tas") : "-");
    kv(ac->headingIsTrack ? "track" : "heading", ac->hasHeading ? num(true, "%.0f deg", ac->headingDeg) : "-");
    kv("vertical", ac->hasVrate ? num(true, "%+.0f ft/min", ac->vrateFpm) : "-");
    if (ac->hasSquawk) snprintf(b, sizeof b, "%04d", ac->squawk); else snprintf(b, sizeof b, "-");
    kv("squawk", b);
    kv("selected alt", ac->hasSelAlt ? num(true, "%.0f ft", ac->selAltFt) : "-");
    kv("mach", ac->hasMach ? num(true, "%.2f", ac->mach) : "-");
    if (ac->emergency) snprintf(b, sizeof b, "state %d", ac->emergency); else snprintf(b, sizeof b, "none");
    kv("emergency", b);
    snprintf(b, sizeof b, "%u messages", ac->messages); kv("heard", b);
    snprintf(b, sizeof b, "%.0f dBFS", ac->levelDbfs); kv("level", b);
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    subNav("adsbv", S.viewMode, {"Map", "Table", "Radar"});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float dw = std::min(250.f * gUi, W * 0.26f);
    ImGui::BeginChild("##adsb_l", ImVec2(S.viewMode == 1 ? W : W - dw - 8 * gUi, H));
    if (S.viewMode == 0) mapView(a, ImGui::GetContentRegionAvail());
    else if (S.viewMode == 1) aircraftTable(a, false);
    else radar(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    if (S.viewMode != 1) {
        ImGui::SameLine();
        ImGui::BeginChild("##adsb_r", ImVec2(0, H));
        detail(a);
        ImGui::EndChild();
    }
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); ImGui::PopTextWrapPos(); } return; }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%u aircraft, %u with position", a.rx.adsb.aircraftCount, a.rx.adsb.withPosition); ImGui::PopTextWrapPos(); }
    aircraftTable(a, true);
}

void receiver(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(120 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 2 ? "receiving" : t.state == 1 ? "pulses, no good message" : "searching");
    kv("messages", "%.0f / s, %llu good, %llu failed the CRC", t.msgsPerSec, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("repaired", "%llu", (unsigned long long)t.corrected);
    kv("signal", "%.1f dBFS, noise %.1f dBFS, SNR %.1f dB", t.levelDbfs, t.noiseDbfs, t.snrDb);
    kv("aircraft", "%u (%u with position)", t.aircraftCount, t.withPosition);
    if (t.refValid) kv("farthest", "%.0f nm", t.maxRangeNm);
    ImGui::Spacing();
    ImGui::TextDisabled("Last messages");
    if (ImGui::BeginTable("##mon", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("DF"); ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("fix"); ImGui::TableSetupColumn("message"); ImGui::TableSetupColumn("what");
        ImGui::TableHeadersRow();
        for (size_t i = t.frames.size(); i-- > 0;) {
            const AdsbFrameInfo& f = t.frames[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", f.df);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(icaoText(f.icao).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", f.levelDbfs);
            ImGui::TableNextColumn(); ImGui::Text("%d", f.corrected);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.hex.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.what.c_str());
        }
        ImGui::EndTable();
    }
}

void panels(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(120.f, (W - 3 * gap) / 3.f), plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(colW, "Messages per second");   // the captions are never wider than their plots
    std::vector<float> v(S.rate.begin(), S.rate.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##rate", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Good messages by downlink format");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##df", ImVec2(colW, plotH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        static const int dfs[] = {0, 4, 5, 11, 16, 17, 18, 20, 21};
        uint64_t mx = 1;
        if (on) for (int d : dfs) mx = std::max(mx, t.dfCount[d]);
        const float bw = colW / 9.f;
        for (int i = 0; i < 9; i++) {
            const uint64_t n = on ? t.dfCount[dfs[i]] : 0;
            const float h = (plotH - 16 * gUi) * (float)n / (float)mx;
            dl->AddRectFilled(ImVec2(p.x + i * bw + 3, p.y + plotH - 14 * gUi - h), ImVec2(p.x + (i + 1) * bw - 3, p.y + plotH - 14 * gUi), ImGui::ColorConvertFloat4ToU32(pal::accent(0.8f)));
            char b[8]; snprintf(b, sizeof b, "%d", dfs[i]);
            const ImVec2 ts = ImGui::CalcTextSize(b);
            if (ts.x <= bw - 2) dl->AddText(ImVec2(p.x + i * bw + (bw - ts.x) * 0.5f, p.y + plotH - 13 * gUi), IM_COL32(150, 154, 158, 255), b);   // only where it fits under its bar
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR of good messages (dB)");
    std::vector<float> s(S.snr.begin(), S.snr.end());
    if (s.empty()) s.push_back(0);
    ImGui::PlotLines("##snr", s.data(), (int)s.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const AdsbTelemetry& t = a.rx.adsb;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Positions", !on ? 0 : t.withPosition > 0 ? 1 : t.aircraftCount > 0 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Receiving" : t.state == 1 ? "Pulses only" : "Searching");
    snprintf(b, sizeof b, "%.0f /s", t.msgsPerSec); ro("Messages", b);
    snprintf(b, sizeof b, "%u (%u)", t.aircraftCount, t.withPosition); ro("Aircraft", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%.1f dBFS", t.noiseDbfs); ro("Noise", b);
    if (t.refValid && t.maxRangeNm > 0) { snprintf(b, sizeof b, "%.0f nm", t.maxRangeNm); ro("Farthest", b); }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const AdsbTelemetry& t = a.rx.adsb;
    char b[96];
    l1 = "ADS-B";
    if (!live(a)) return;
    snprintf(b, sizeof b, "%s  %u aircraft  %.0f msg/s  SNR %.0f dB", t.state == 2 ? "receiving" : "searching", t.aircraftCount, t.msgsPerSec, t.snrDb);
    l2 = b;
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Band");
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    const bool std1090 = std::fabs(a.freqMhz - 1090.0) < 0.001;
    if (ImGui::BeginCombo("##band", std1090 ? "1090 MHz  ADS-B / Mode S" : "custom frequency")) {
        if (ImGui::Selectable("1090 MHz  ADS-B / Mode S", std1090)) { a.freqMhz = 1090.0; retune = true; }
        ImGui::BeginDisabled();
        ImGui::Selectable("978 MHz  UAT (US only)", false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("UAT is a separate system used by small aircraft in the US below 18,000 ft (FSK, not Mode S). OnAir does not decode it yet.");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Europe, the US and the rest of the world all send ADS-B on 1090 MHz (1090ES). The US has a second system, UAT on 978 MHz, which is not decoded yet.");
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Antenna position (for ranges and the map)"); ImGui::PopTextWrapPos(); }
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    double la = S.refLat, lo = S.refLon;
    bool ch = ImGui::InputDouble("##alat", &la, 0, 0, "%.4f N");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    ch |= ImGui::InputDouble("##alon", &lo, 0, 0, "%.4f E");
    if (ch) setReference(a, la, lo);
    if (ImGui::Button(S.locBusy ? "Locating..." : "Locate (Wi-Fi)") && !S.locBusy) { plat::locateStart(); S.locBusy = 1; S.locMsg = "asking the location service..."; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ask the system for this computer's position (Wi-Fi positioning). macOS asks for permission the first time.");
    if (!S.locMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.locMsg.c_str()); ImGui::PopTextWrapPos(); }
    double areaLat, areaLon, nearest;
    if (live(a) && positionLooksWrong(a.rx.adsb, areaLat, areaLon, nearest)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "Every aircraft is over %.0f nm away: this position is probably not yours.", nearest);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Use the aircraft's area##ref")) setReference(a, areaLat, areaLon);
    }
}

void decoder(App& a, bool&) {
    static const char* names[] = {"off", "1 bit", "2 bits"};
    ImGui::TextDisabled("FIX"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##fix", names[S.correction])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], S.correction == i)) { S.correction = i; if (a.engine.running()) a.engine.adsb().setCorrection(i); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repair of bit errors in DF11, DF17 and DF18 messages. 2 bits finds more messages but lets more false ones through.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated airspace"); ImGui::PopTextWrapPos(); }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] == 0 ? 12 : sc.modeOpt[0];
    if (ImGui::SliderInt("##sn", &n, 1, 100, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    float mul = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##sm", &mul, 0.2f, 10.f, "x%.1f rate", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = mul; changed = true; }
    flowNext();
    bool replies = sc.modeOpt[1] == 0;
    if (ImGui::Checkbox("radar replies", &replies)) { sc.modeOpt[1] = replies ? 0 : 1; changed = true; }
    flowNext();
    bool emg = sc.modeOpt[3] == 1;
    if (ImGui::Checkbox("emergency", &emg)) { sc.modeOpt[3] = emg ? 1 : 0; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const AdsbTelemetry& t = a.rx.adsb;
    out.push_back({"MESSAGES / s", "%.0f", t.msgsPerSec, 0, 100, t.msgsPerSec > 1 ? 1 : 2});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraftCount, 0, 60, 0});
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.snrDb >= 15 ? 1 : 2});
    out.push_back({"NOISE  dBFS", "%.1f", t.noiseDbfs, -80, -20, t.noiseDbfs > -35 ? 3 : 0});
}

} // namespace

extern const ModeUi kAdsbUi;
const ModeUi kAdsbUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Aircraft",
    .tabIcon = Ic::Compass,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
