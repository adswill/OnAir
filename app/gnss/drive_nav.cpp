// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS > Navigate. The position is the fix of the GNSS decoder (nothing else is read); between fixes it is carried on along the last course
// (dead reckoning, at most 10 s), and "no fix" is shown as such. Routes are waypoints clicked on the map or a GPX file; no routing data is
// fetched, so the course is the straight line between the waypoints. Accuracy is whatever the fix gives; this is not a safety aid.
#include "drive_nav.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/nav.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/u8file.h"
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
using namespace dect2::geo;

struct State {
    bool loaded = false;
    adsbmap::View map;
    bool follow = true, night = false, chime = false, planning = false, logOn = true;
    Route route;
    int hintSeg = -1;
    NavProgress prog;
    DeadReckoner dr;
    TripLog trip;
    uint32_t lastFixCount = 0;
    uint64_t lastSeq = 0;
    bool wasOff = false, announcedArrival = false;
    int lastBand = -1;                    // the distance band of the last "approaching" chime
    std::string note;
    std::unique_ptr<dect2::AudioOut> out;
};
State S;

double utcOf(const GnssFix& f) {
    if (!f.timeValid) return 0;
    struct tm g = {};
    g.tm_year = f.year - 1900; g.tm_mon = f.month - 1; g.tm_mday = f.day; g.tm_hour = f.hour; g.tm_min = f.minute; g.tm_sec = (int)f.second;
#ifdef _WIN32
    return (double)_mkgmtime(&g);
#else
    return (double)timegm(&g);
#endif
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.night = p.getB("navNight", false);
    S.chime = p.getB("navChime", false);
    S.map.zoom = (int)p.getI("navZoom", 15);
    receiverLocation(S.map.lat, S.map.lon);
}

// a short two-tone chime (turn near, off route, arrival) through the audio output; off unless the user switches it on
void chime(int kind) {
    if (!S.chime) return;
    if (!S.out) { S.out = std::make_unique<dect2::AudioOut>(); S.out->setVolume(0.5f); }
    if (!S.out->running() && !S.out->start(48000)) return;
    const float f0 = kind == 1 ? 440.f : 880.f, f1 = kind == 1 ? 330.f : kind == 2 ? 1320.f : 1100.f;
    std::vector<float> b;
    for (int i = 0; i < 48000 * 0.36; i++) {
        const float t = (float)i / 48000.f, f = t < 0.18f ? f0 : f1, env = std::min(1.f, std::min(t < 0.18f ? t / 0.01f : (t - 0.18f) / 0.01f, (0.36f - t) / 0.02f + (t < 0.18f ? 1.f : 0.f)));
        const float v = 0.35f * std::max(0.f, env) * std::sin(6.2831853f * f * t);
        b.push_back(v); b.push_back(v);
    }
    S.out->write(b.data(), (int)b.size() / 2);
}

void setRoute(Route r) {
    S.route = std::move(r);
    S.hintSeg = -1; S.prog = NavProgress(); S.wasOff = false; S.announcedArrival = false; S.lastBand = -1;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = dect2::u8fopen(path, "rb");
    if (!f) return false;
    char buf[65536]; size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (64u << 20)) out.append(buf, n);
    fclose(f);
    return true;
}
bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = dect2::u8fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

} // namespace

void driveNavTick(App& a) {
    load();
    const double now = ImGui::GetTime();
    const bool live = a.rx.standard == 13 && a.engine.running();
    if (!live) { S.dr.clear(); S.lastSeq = 0; return; }
    const GnssTelemetry& t = a.rx.gnss;
    if (a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        if (t.fix.valid && t.fix.fixCount != S.lastFixCount) {
            S.lastFixCount = t.fix.fixCount;
            S.dr.onFix(now, {t.fix.latDeg, t.fix.lonDeg}, std::max(1.0, (double)t.fix.hErrM), t.fix.hasVelocity, t.fix.speedMps, t.fix.courseDeg);
            if (S.logOn) S.trip.add(t.fix.latDeg, t.fix.lonDeg, t.fix.heightM, utcOf(t.fix), t.fix.hasVelocity ? t.fix.speedMps : S.dr.speedMps);
        }
    }
    S.dr.tick(now);
    if (S.dr.state == DeadReckoner::NoFix || S.route.pts.size() < 2) { S.prog = NavProgress(); return; }
    S.prog = navProgress(S.route, S.dr.pos, S.dr.speedMps, S.dr.courseDeg, S.dr.courseKnown, S.dr.sigmaM, S.hintSeg);
    if (S.prog.valid) {
        S.hintSeg = S.prog.seg;
        if (S.prog.offRoute && !S.wasOff) chime(1);
        S.wasOff = S.prog.offRoute;
        if (S.prog.arrived && !S.announcedArrival) { S.announcedArrival = true; chime(2); }
        const int band = S.prog.toNextM < 100 ? 2 : S.prog.toNextM < 300 ? 1 : 0;   // "turn near" at 300 m and 100 m
        if (!S.prog.arrived && !S.prog.offRoute && band > 0 && band > S.lastBand && std::fabs(S.prog.turnDeg) > 30 && S.prog.next < (int)S.route.pts.size() - 1) chime(3);
        if (band != S.lastBand && (band > S.lastBand || S.prog.toNextM > 400)) S.lastBand = band > 0 || S.prog.toNextM > 400 ? band : S.lastBand;
    }
}

void driveNavView(App& a, ImVec2 size) {
    load();
    const GnssTelemetry& t = a.rx.gnss;
    const bool live = a.rx.standard == 13 && a.engine.running();
    const float panelW = std::min(250.f * gUi, size.x * 0.34f);
    const ImVec2 mapSize(size.x - panelW - 8 * gUi, size.y);

    ImGui::BeginChild("##nav_map", mapSize);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, mapSize);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.follow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + mapSize.x, p0.y + mapSize.y), true);
    if (S.night) dl->AddRectFilled(p0, ImVec2(p0.x + mapSize.x, p0.y + mapSize.y), IM_COL32(0, 8, 24, 150));
    const ImU32 routeCol = S.night ? IM_COL32(255, 170, 60, 255) : IM_COL32(30, 110, 230, 255);
    const ImU32 doneCol = S.night ? IM_COL32(120, 90, 50, 200) : IM_COL32(120, 140, 170, 200);
    // clicking adds a waypoint while planning (a click, not a drag)
    ImVec2 cp;
    if (S.planning && adsbmap::clickedAt(cp)) {
        LatLon w; adsbmap::unproject(cp, w.lat, w.lon);
        S.route.pts.push_back(w);
        S.hintSeg = -1;
    }
    const auto& pts = S.route.pts;
    for (size_t i = 1; i < pts.size(); i++) {
        const bool done = S.prog.valid && (int)i - 1 < S.prog.seg;
        dl->AddLine(adsbmap::project(pts[i - 1].lat, pts[i - 1].lon), adsbmap::project(pts[i].lat, pts[i].lon), done ? doneCol : routeCol, (done ? 3.f : 4.5f) * gUi);
    }
    for (size_t i = 0; i < pts.size(); i++) {
        const ImVec2 q = adsbmap::project(pts[i].lat, pts[i].lon);
        const bool nxt = S.prog.valid && (int)i == S.prog.next, last = i + 1 == pts.size();
        dl->AddCircleFilled(q, (nxt ? 7.f : 5.f) * gUi, last ? IM_COL32(220, 60, 60, 255) : nxt ? IM_COL32(255, 255, 255, 255) : routeCol);
        dl->AddCircle(q, (nxt ? 7.f : 5.f) * gUi, IM_COL32(0, 0, 0, 220), 20, 1.5f);
        char n[16]; snprintf(n, sizeof n, "%zu", i + 1);
        dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 8 * gUi), IM_COL32(235, 238, 240, 255), n);
    }
    if (S.trip.pts.size() > 1) {   // the trip so far
        ImVec2 prev = adsbmap::project(S.trip.pts[0].lat, S.trip.pts[0].lon);
        for (size_t i = 1; i < S.trip.pts.size(); i++) {
            const ImVec2 q = adsbmap::project(S.trip.pts[i].lat, S.trip.pts[i].lon);
            dl->AddLine(prev, q, IM_COL32(110, 220, 130, 200), 2.f * gUi);
            prev = q;
        }
    }
    if (S.dr.state != DeadReckoner::NoFix) {
        const ImVec2 q = adsbmap::project(S.dr.pos.lat, S.dr.pos.lon);
        const bool coast = S.dr.state == DeadReckoner::Coasting;
        const double dLat = S.dr.sigmaM / 111320.0;
        const float r = std::max(5.f * gUi, std::fabs(adsbmap::project(S.dr.pos.lat + dLat, S.dr.pos.lon).y - q.y));
        dl->AddCircleFilled(q, r, IM_COL32(80, 160, 255, 35), 40);
        if (S.dr.courseKnown) {   // an arrow along the course
            const double h = S.dr.courseDeg * 0.017453292519943295;
            const ImVec2 u((float)std::sin(h), (float)-std::cos(h)), v(-u.y, u.x);
            const float L = 13 * gUi;
            const ImVec2 tri[3] = {{q.x + u.x * L, q.y + u.y * L}, {q.x - u.x * L * 0.7f + v.x * L * 0.6f, q.y - u.y * L * 0.7f + v.y * L * 0.6f}, {q.x - u.x * L * 0.7f - v.x * L * 0.6f, q.y - u.y * L * 0.7f - v.y * L * 0.6f}};
            dl->AddConvexPolyFilled(tri, 3, coast ? IM_COL32(255, 200, 80, 255) : IM_COL32(255, 255, 255, 255));
            dl->AddPolyline(tri, 3, IM_COL32(20, 90, 200, 255), ImDrawFlags_Closed, 2.f);
        } else {
            dl->AddCircleFilled(q, 6 * gUi, coast ? IM_COL32(255, 200, 80, 255) : IM_COL32(255, 255, 255, 255));
            dl->AddCircle(q, 6 * gUi, IM_COL32(20, 90, 200, 255), 24, 2.f);
        }
        if (S.follow) { S.map.lat = S.dr.pos.lat; S.map.lon = S.dr.pos.lon; }
    }
    dl->PopClipRect();
    // the map's buttons
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("follow")) { S.follow = true; if (S.dr.state != DeadReckoner::NoFix) { S.map.lat = S.dr.pos.lat; S.map.lon = S.dr.pos.lon; } }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("navZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + mapSize.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(mapSize.x, "%s", S.planning ? "planning: click the map to add waypoints" : "straight line between waypoints, no road data");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + mapSize.y));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##nav_side", ImVec2(0, size.y));
    ImGui::PushTextWrapPos(0);
    // the state first: no fix is said plainly
    if (!live) ImGui::TextColored(pal::warnAmber(), "start the receiver for a position");
    else if (S.dr.state == DeadReckoner::NoFix) ImGui::TextColored(pal::badRed(), "NO FIX%s", t.nTracked > 0 ? " (tracking, not enough satellites yet)" : " (searching)");
    else if (S.dr.state == DeadReckoner::Coasting) ImGui::TextColored(pal::warnAmber(), "no new fix: carried on from the last one");
    else ImGui::TextColored(pal::okGreen(), "%s, %d satellites, error %.0f m", t.fix.type.c_str(), t.fix.nSats, t.fix.hErrM);
    ImGui::PushFont(a.mono, 0);
    if (S.dr.state != DeadReckoner::NoFix) {
        ImGui::Text("%.0f km/h   %s%s", S.dr.speedMps * 3.6, S.dr.courseKnown ? "" : "course ?", "");
        if (S.dr.courseKnown) { ImGui::SameLine(); ImGui::Text("%03.0f %s", S.dr.courseDeg, navCompass(S.dr.courseDeg)); }
    } else ImGui::TextUnformatted("- km/h");
    ImGui::PopFont();
    ImGui::Separator();
    if (S.route.pts.size() < 2) {
        ImGui::TextDisabled("No route. Plan waypoints on the map or open a GPX file.");
    } else if (S.prog.valid) {
        if (S.prog.arrived) ImGui::TextColored(pal::okGreen(), "ARRIVED");
        else if (S.prog.offRoute) ImGui::TextColored(pal::badRed(), "OFF ROUTE: %s from the line", navDistText(S.prog.crossM).c_str());
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("next %d of %zu", S.prog.next + 1, S.route.pts.size());
        ImGui::Text("%s  at %03.0f %s", navDistText(S.prog.toNextM).c_str(), S.prog.bearingToNext, navCompass(S.prog.bearingToNext));
        if (S.dr.courseKnown && std::fabs(S.prog.turnDeg) > 10) ImGui::Text("%s %.0f deg", S.prog.turnDeg > 0 ? "turn right" : "turn left", std::fabs(S.prog.turnDeg));
        else if (S.dr.courseKnown) ImGui::TextUnformatted("straight on");
        ImGui::Text("left  %s", navDistText(S.prog.leftM).c_str());
        ImGui::Text("ETA   %s", navEtaText(S.prog.etaS).c_str());
        ImGui::PopFont();
    } else ImGui::TextDisabled("%s", S.dr.state == DeadReckoner::NoFix ? "waiting for a fix" : "...");
    ImGui::Separator();
    if (ImGui::Checkbox("plan: click to add waypoints", &S.planning)) S.note.clear();
    if (ImGui::SmallButton("undo waypoint") && !S.route.pts.empty()) { S.route.pts.pop_back(); S.hintSeg = -1; }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear route")) setRoute(Route());
    if (ImGui::SmallButton("open GPX")) {
        const std::string path = openFileDialog();
        if (!path.empty()) {
            std::string xml; Route r;
            if (readFile(path, xml) && parseGpx(xml, r)) {
                setRoute(r); S.planning = false; S.follow = false;
                S.map.lat = r.pts[0].lat; S.map.lon = r.pts[0].lon;
                S.note = "opened " + std::to_string(r.pts.size()) + " points";
            } else S.note = "no route or points found in " + path;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("save route") && S.route.pts.size() >= 2) {
        const std::string path = saveFileDialog("route.gpx");
        if (!path.empty()) { Route r = S.route; if (r.name.empty()) r.name = "OnAir route"; S.note = writeFile(path, gpxRoute(r)) ? "saved " + path : "could not write " + path; }
    }
    ImGui::Separator();
    ImGui::Checkbox("trip log", &S.logOn);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu points, %s", S.trip.pts.size(), navDistText(S.trip.distM).c_str());
    if (ImGui::SmallButton("save trip (GPX)") && !S.trip.pts.empty()) {
        char nm[64]; const std::time_t now = std::time(nullptr);
        std::strftime(nm, sizeof nm, "trip_%Y%m%d_%H%M%S.gpx", std::localtime(&now));
        const std::string path = saveFileDialog(nm);
        if (!path.empty()) S.note = writeFile(path, gpxTrack(S.trip, "OnAir trip")) ? "saved " + path : "could not write " + path;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("new trip")) S.trip.clear();
    ImGui::Separator();
    if (ImGui::Checkbox("night colours", &S.night)) plat::prefs().setB("navNight", S.night);
    if (ImGui::Checkbox("chime for turns", &S.chime)) plat::prefs().setB("navChime", S.chime);
    ImGui::SameLine();
    ImGui::TextDisabled("(a tone, not speech)");
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("The accuracy is that of the decoded fix. Not for use as a safety aid.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}
