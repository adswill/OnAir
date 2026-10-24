// ADS-B screens: the radar plot around the receiver and the map of aircraft (tiles, trails, range rings), with the full-window map.
// Split from adsb_ui.cpp; the tile map itself (projection, tiles, controls) is in adsb_map.cpp.
// The shared state and helpers are declared in adsb_internal.h.
#include "../map/bar_flow.h"
#include "adsb_internal.h"
#include "map/pos_predict.h"
#include "adsb_acars.h"
#include "trail_draw.h"
#include "gfx/render_caps.h"

using namespace adsbui;

namespace adsbui {

static pospred::Layer gPred;   // position estimates: smooth interpolation between ADS-B updates (on by default), uncertainty ellipses (off)

static double pathDistanceM(double lat1, double lon1, double lat2, double lon2) {
    const double p = M_PI / 180.0, dLat = (lat2 - lat1) * p, dLon = (lon2 - lon1) * p;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
        + std::cos(lat1 * p) * std::cos(lat2 * p) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 6371000.0 * 2.0 * std::asin(std::min(1.0, std::sqrt(a)));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p = M_PI / 180.0, dLon = (lon2 - lon1) * p;
    const double y = std::sin(dLon) * std::cos(lat2 * p);
    const double x = std::cos(lat1 * p) * std::sin(lat2 * p) - std::sin(lat1 * p) * std::cos(lat2 * p) * std::cos(dLon);
    double b = std::atan2(y, x) / p;
    if (b < 0) b += 360.0;
    return b;
}

static bool planeHeadingDeg(const AdsbAircraft& ac, float& out) {
    if (ac.hasHeading) { out = (float)ac.headingDeg; return true; }
    constexpr double kMinM = 15.0;
    if (ac.hasPos && !ac.track.empty()) {
        const double lat2 = ac.lat, lon2 = ac.lon;
        for (size_t i = ac.track.size(); i > 0; i--) {
            const double lat1 = ac.track[i - 1].lat, lon1 = ac.track[i - 1].lon;
            if (pathDistanceM(lat1, lon1, lat2, lon2) >= kMinM) { out = (float)bearingDeg(lat1, lon1, lat2, lon2); return true; }
        }
    }
    return false;
}

// the radar: the receiver in the middle, north up, a ring every few tens of nautical miles
void radar(App& a, ImVec2 size) {
    const AdsbTelemetry& t = a.rx.adsb;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();   // full-screen control sits on top (same as adsb_map.cpp)
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
        char b[24]; snprintf(b, sizeof b, "%.0f %s", au::distOut(S.units, scale * i / rings), au::distUnit(S.units));
        dl->AddText(ImVec2(c.x + 3 * gUi, c.y - R * i / rings), IM_COL32(110, 114, 118, 255), b);
    }
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), grid); dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), grid);
    dl->AddText(ImVec2(c.x - 3 * gUi, c.y - R - ImGui::GetTextLineHeight() - 1), IM_COL32(130, 134, 138, 255), "N");
    dl->AddCircleFilled(c, 3 * gUi, ImGui::ColorConvertFloat4ToU32(pal::accent()));
    if (on) {
        const double k = std::cos(S.refLat * M_PI / 180.0);
        auto pt = [&](double lat, double lon) { return ImVec2(c.x + (float)((lon - S.refLon) * k * 60.0 / scale * R), c.y - (float)((lat - S.refLat) * 60.0 / scale * R)); };
        for (const auto& ac : t.aircraft) {
            if (!ac.hasPos) continue;
            const bool sel = ac.icao == S.sel;
            const ImU32 col = ac.emergency ? ImGui::ColorConvertFloat4ToU32(pal::badRed()) : sel ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(pal::accent());
            if (!rcaps::lowPowerDraw() && ac.track.size() >= 2) {   // the trail, thinned and clipped as on the map (trail_draw.h)
                static std::vector<trails::Pt> pts;
                pts.resize(ac.track.size());
                for (size_t i = 0; i < ac.track.size(); i++) { const ImVec2 q = pt(ac.track[i].lat, ac.track[i].lon); pts[i] = {q.x, q.y}; }
                trails::forEachSegment(pts.data(), pts.size(), 1.5f * gUi, {p0.x, p0.y, p0.x + size.x, p0.y + size.y},
                                       [&](size_t a, size_t b) { dl->AddLine(ImVec2(pts[a].x, pts[a].y), ImVec2(pts[b].x, pts[b].y), IM_COL32(70, 74, 78, 255)); });
            }
            const ImVec2 q = pt(ac.lat, ac.lon);
            if (std::fabs(q.x - c.x) > R * 1.1f || std::fabs(q.y - c.y) > R * 1.1f) continue;
            { float hdg = 0.f; if (planeHeadingDeg(ac, hdg)) { const float h = hdg * (float)M_PI / 180.f; dl->AddLine(q, ImVec2(q.x + std::sin(h) * 10 * gUi, q.y - std::cos(h) * 10 * gUi), col, 1.5f); } }
            dl->AddCircleFilled(q, (sel ? 4.f : 3.f) * gUi, col);
            char b[48]; snprintf(b, sizeof b, "%s", ac.callsign.empty() ? icaoText(ac.icao).c_str() : ac.callsign.c_str());
            dl->AddText(ImVec2(q.x + 6 * gUi, q.y - 6 * gUi), IM_COL32(190, 194, 198, 255), b);
        }
    }
    if (!t.refValid) dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + 36 * gUi), IM_COL32(150, 154, 158, 255), "no reference position");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (adsbmap::fullButton(S.full, "radar")) S.full = !S.full;
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// altitude colours, as on the flight-tracking sites: low and warm, high and cool
ImU32 altColour(const AdsbAircraft& ac, float alpha = 1.f) {
    if (ac.emergency) return IM_COL32(235, 70, 70, (int)(255 * alpha));
    const dect2::adsbd::Rgb c = dect2::adsbd::altRgb(ac.hasAlt, ac.altFt, ac.ground);
    return IM_COL32(c.r, c.g, c.b, (int)(255 * alpha));
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

// the aircraft trails first (so the icons sit on top), then the path a couple of minutes ahead at the last speed
// Points closer than 1.5 px to the last one drawn are skipped and segments wholly off one side of the view are not drawn (trail_draw.h).
static void drawTrails(ImDrawList* dl, const AdsbTelemetry& t, ImVec2 p0, ImVec2 size) {
    const trails::Rect clip{p0.x, p0.y, p0.x + size.x, p0.y + size.y};
    static std::vector<trails::Pt> pts;   // the projected track, reused (UI thread only)
    for (const auto& ac : t.aircraft) {   // trails first, so the icons sit on top
        if (!ac.hasPos || ac.track.size() < 2) continue;
        const ImU32 trail = adsbmap::acColour(adsbmap::AcSrc::Adsb, ac.posAgeSec, 0.55f);
        const bool stale = ac.posAgeSec > adsbmap::acStaleSec(adsbmap::AcSrc::Adsb);
        const bool alt = S.disp.altColour && !stale;   // a silent aircraft greys, as its icon does
        const float w = (float)S.disp.trackWidth * gUi;
        auto seg = [&](const AdsbTrackPoint& tp) {
            if (!alt) return trail;
            const dect2::adsbd::Rgb c = dect2::adsbd::altRgb(tp.hasAlt, tp.altFt(), tp.ground);
            return IM_COL32(c.r, c.g, c.b, 140);
        };
        pts.resize(ac.track.size());
        for (size_t i = 0; i < ac.track.size(); i++) { const ImVec2 q = adsbmap::project(ac.track[i].lat, ac.track[i].lon); pts[i] = {q.x, q.y}; }
        trails::forEachSegment(pts.data(), pts.size(), 1.5f * gUi, clip, [&](size_t a, size_t b) {
            if (std::fabs(ac.track[b].lat - ac.track[a].lat) < 2 && std::fabs(ac.track[b].lon - ac.track[a].lon) < 2)   // a jump is a bad fix, not a flight
                dl->AddLine(ImVec2(pts[a].x, pts[a].y), ImVec2(pts[b].x, pts[b].y), seg(ac.track[b]), w);
        });
        const ImVec2 now = adsbmap::project(ac.lat, ac.lon);
        if (std::fabs(ac.track.back().lat - ac.lat) < 2 && std::fabs(ac.track.back().lon - ac.lon) < 2 && !trails::sameSideOutside(pts.back(), {now.x, now.y}, clip))
            dl->AddLine(ImVec2(pts.back().x, pts.back().y), now, alt ? altColour(ac, 0.55f) : trail, w);
    }
    for (const auto& ac : t.aircraft) {   // a couple of minutes ahead at the last speed, like the path on the flight-tracking sites
        if (!ac.hasPos || !ac.hasHeading || !ac.hasSpeed || ac.speedKt < 30 || ac.ground) continue;
        double la = 0, lo = 0;
        adsbmap::aheadNm(ac.lat, ac.lon, ac.headingDeg, ac.speedKt * (2.0 / 60.0), la, lo);
        const ImVec2 a = adsbmap::project(ac.lat, ac.lon), b = adsbmap::project(la, lo);
        if (!trails::sameSideOutside({a.x, a.y}, {b.x, b.y}, clip)) dl->AddLine(a, b, adsbmap::acColour(adsbmap::AcSrc::Adsb, ac.posAgeSec, 0.35f), 1.2f);
    }
}

// GPS interference: the 0.5 degree cells where many of the aircraft heard in the last hour reported a poor GPS position (NACp / NIC low, or a position
// jump). An indicator, not proof of jamming: old transponders and shaded antennas look the same.
static void drawJam(ImDrawList* dl, ImVec2 p0, ImVec2 size) {
    static const ImU32 col[4] = {IM_COL32(63, 174, 90, 70), IM_COL32(230, 194, 41, 95), IM_COL32(240, 138, 36, 120), IM_COL32(224, 68, 58, 150)};
    const auto cells = S.jam.cells(ImGui::GetTime());
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const dect2::GpsJamCell* tip = nullptr;
    for (const auto& c : cells) {
        const int lv = c.level();
        if (lv < 0) continue;
        const ImVec2 a = adsbmap::project(c.south(), c.west()), b = adsbmap::project(c.south() + dect2::kGpsJamCellDeg, c.west() + dect2::kGpsJamCellDeg);
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        if (hi.x < p0.x || hi.y < p0.y || lo.x > p0.x + size.x || lo.y > p0.y + size.y) continue;
        dl->AddRectFilled(lo, hi, col[lv]);
        if (mouse.x >= lo.x && mouse.x < hi.x && mouse.y >= lo.y && mouse.y < hi.y && ImGui::IsWindowHovered()) tip = &c;
    }
    if (tip) ImGui::SetTooltip("%d of %d aircraft reported a poor GPS position in the last hour (%d with a position jump).\nAn indicator, not proof of jamming.", tip->degraded, tip->aircraft, tip->jumps);
}

// winds aloft: an arrow downwind from each aircraft, 1 px per kt, a tick per 50 kt
static void drawWinds(ImDrawList* dl, const AdsbTelemetry& t) {
    for (const auto& ac : t.aircraft) {   // winds aloft: an arrow downwind from the aircraft, 1 px per kt, a tick per 50 kt
        if (!ac.hasPos || ac.ground || !(ac.hasWindDer || ac.hasWindRep)) continue;
        const float kt = ac.hasWindDer ? ac.windDerKt : ac.windRepKt, from = ac.hasWindDer ? ac.windDerDir : ac.windRepDir;
        const ImVec2 q = adsbmap::project(ac.lat, ac.lon);
        const float rad = (from + 180.f) * (float)M_PI / 180.f, dx = std::sin(rad), dy = -std::cos(rad);
        const float len = (18.f + std::min(kt, 200.f) * 0.35f) * gUi;
        const ImVec2 e(q.x + dx * len, q.y + dy * len);
        const ImU32 col = altColour(ac, 0.9f);
        dl->AddLine(q, e, IM_COL32(0, 0, 0, 160), 3.5f * gUi);
        dl->AddLine(q, e, col, 2.f * gUi);
        const float hw = 5 * gUi;
        dl->AddTriangleFilled(ImVec2(e.x + dx * hw, e.y + dy * hw), ImVec2(e.x - dy * hw * 0.7f, e.y + dx * hw * 0.7f), ImVec2(e.x + dy * hw * 0.7f, e.y - dx * hw * 0.7f), col);
        for (int k = 1; k * 50 <= (int)kt && k <= 4; k++) {   // ticks at the tail
            const ImVec2 m(q.x + dx * (len * 0.12f * k + 4 * gUi), q.y + dy * (len * 0.12f * k + 4 * gUi));
            dl->AddLine(ImVec2(m.x - dy * 4 * gUi, m.y + dx * 4 * gUi), ImVec2(m.x + dy * 4 * gUi, m.y - dx * 4 * gUi), col, 1.5f * gUi);
        }
        if (S.map.zoom >= 7) { char b[16]; snprintf(b, sizeof b, "%s", uSpd(kt).c_str()); dl->AddText(ImVec2(e.x + 4 * gUi, e.y - 6 * gUi), col, b); }
    }
}

// the aircraft icons with their label boxes; the one nearest a click (within 16 px) becomes `hit`
static void drawAircraft(ImDrawList* dl, const AdsbTelemetry& t, ImVec2 p0, ImVec2 size, ImVec2 click, bool clicked, float& best, const AdsbAircraft*& hit) {
    const double nowT = ImGui::GetTime();
    const bool lite = rcaps::lowPowerDraw();   // low-power drawing: a triangle per aircraft, the label box only for the selected one
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        dect2::track::PosEstimate pe;
        const bool est = gPred.get(dect2::track::keyFromIcao(ac.icao), nowT, pe, ac.posAgeSec) && pe.valid;
        const ImVec2 q = est ? adsbmap::project(pe.lat, pe.lon) : adsbmap::project(ac.lat, ac.lon);
        if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
        const bool sel = ac.icao == S.sel;
        const bool stale = ac.posAgeSec > adsbmap::acStaleSec(adsbmap::AcSrc::Adsb);
        const ImU32 fill = ac.emergency ? ImGui::ColorConvertFloat4ToU32(pal::badRed()) : (S.disp.altColour && !stale) ? altColour(ac) : adsbmap::acColour(adsbmap::AcSrc::Adsb, ac.posAgeSec);
        {
            float hdg = 0.f;
            const float hd = planeHeadingDeg(ac, hdg) ? hdg : 0.f, r = (sel ? 14.f : 12.f) * gUi;
            if (lite && !sel) {
                const float h = hd * (float)M_PI / 180.f, s = std::sin(h), c = std::cos(h);
                auto rot = [&](float x, float y) { return ImVec2(q.x + (x * c - y * s) * r, q.y + (x * s + y * c) * r); };
                dl->AddTriangleFilled(rot(0, -1), rot(0.7f, 0.8f), rot(-0.7f, 0.8f), fill);
            } else drawPlane(dl, q, hd, r, fill, sel);
        }
        if (est && !lite) { pospred::drawPredictionMarks(dl, pe, q, fill, gPred.ellipses); pospred::hoverTooltip(pe, q); }
        if (!lite && acarslog::count(ac.icao) > 0) dl->AddCircleFilled(ImVec2(q.x + 8 * gUi, q.y - 8 * gUi), 3.f * gUi, IM_COL32(245, 160, 40, 255));   // it has sent ACARS messages
        if (lite || !S.disp.labels ? sel : (S.map.zoom >= 6 || sel)) {   // labels off (#85): only the selected aircraft has its box
            char l1[48]; snprintf(l1, sizeof l1, "%s", ac.callsign.empty() ? icaoText(ac.icao).c_str() : ac.callsign.c_str());
            char l2[48] = "", l3[48] = "";
            if (ac.ground) snprintf(l2, sizeof l2, "GND");
            else if (ac.hasAlt) {
                if (S.units.metric()) snprintf(l2, sizeof l2, "%s m", uAlt(ac.altFt).c_str());
                else snprintf(l2, sizeof l2, "FL%03d", (ac.altFt + 50) / 100);
            }
            if (ac.hasSpeed) {
                const size_t n = strlen(l2);
                snprintf(l2 + n, sizeof l2 - n, "%s%s %s", n ? "  " : "", uSpd(ac.speedKt).c_str(), au::speedUnit(S.units));
            }
            if (ac.hasSquawk) snprintf(l3, sizeof l3, "sqk %04d", ac.squawk);
            if (!ac.category.empty()) {
                const size_t n = strlen(l3);
                snprintf(l3 + n, sizeof l3 - n, "%s%s", n ? "  " : "", ac.category.c_str());
            }
            char l4[96] = "";   // registration, country code and operator: for the selected aircraft and when zoomed in
            if (sel || S.map.zoom >= 9) {
                const std::string rg = registrationOf(ac.icao), cc = countryCode(ac.icao), op = operatorOf(ac.icao);
                snprintf(l4, sizeof l4, "%s%s%s%s%s", rg.c_str(), rg.empty() || cc.empty() ? "" : " ", cc.c_str(), op.empty() ? "" : "  ", op.c_str());
            }
            const ImVec2 t1 = ImGui::CalcTextSize(l1), t2 = ImGui::CalcTextSize(l2), t3 = ImGui::CalcTextSize(l3), t4 = ImGui::CalcTextSize(l4);
            const float w = std::max(std::max(t1.x, t2.x), std::max(t3.x, t4.x)) + 8 * gUi;
            const float hh = t1.y + (l2[0] ? t2.y : 0) + (l3[0] ? t3.y : 0) + (l4[0] ? t4.y : 0) + 4 * gUi;
            const ImVec2 lp(q.x + 12 * gUi, q.y - hh * 0.5f);
            dl->AddRectFilled(lp, ImVec2(lp.x + w, lp.y + hh), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddRect(lp, ImVec2(lp.x + w, lp.y + hh), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
            float y = lp.y + 2 * gUi;
            dl->AddText(ImVec2(lp.x + 4 * gUi, y), IM_COL32(235, 238, 240, 255), l1); y += t1.y;
            if (l2[0]) { dl->AddText(ImVec2(lp.x + 4 * gUi, y), fill, l2); y += t2.y; }
            if (l3[0]) { dl->AddText(ImVec2(lp.x + 4 * gUi, y), IM_COL32(190, 194, 198, 255), l3); y += t3.y; }
            if (l4[0]) dl->AddText(ImVec2(lp.x + 4 * gUi, y), IM_COL32(170, 176, 182, 255), l4);
        }
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < best) { best = d; hit = &ac; }
    }
}

// the buttons in the map corner, the "antenna position looks wrong" note and the position / right-click popups
static void mapCorner(App& a, ImVec2 p0, ImVec2 size, bool wrongRef, double areaLat, double areaLon, double nearest) {
    // controls in the corner: they wrap onto further lines when the map is narrow, so nothing runs off its edge
    const float left = p0.x + 8 * gUi, right = p0.x + size.x - 8 * gUi;
    const ImGuiStyle& st = ImGui::GetStyle();
    float lastRight = left - 6 * gUi, rowTop = p0.y + 8 * gUi, bottom = rowTop;
    auto place = [&](float w, float gap) {   // put the cursor where an item w wide goes
        const barflow::Spot sp = barflow::next(lastRight, w, gap, left, right);
        if (sp.newLine) rowTop = bottom + 4 * gUi;
        ImGui::SetCursorScreenPos(ImVec2(sp.x, rowTop));
    };
    auto done = [&]() { lastRight = ImGui::GetItemRectMax().x; bottom = std::max(bottom, ImGui::GetItemRectMax().y); };
    auto btnW = [&](const char* l) { return ImGui::CalcTextSize(l).x + 2 * st.FramePadding.x; };
    auto chkW = [&](const char* l) { return ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize(l).x; };
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    place(26 * gUi, 0);
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    done(); place(26 * gUi, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    done(); place(btnW("antenna"), 6 * gUi);
    if (ImGui::Button("antenna")) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.mapHome = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centre the map on the antenna");
    done(); place(btnW("position"), 2 * gUi);
    if (ImGui::Button("position")) ImGui::OpenPopup("##adsbpos");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set where the antenna is: for ranges, the radar rings and decoding positions locally. You can also right-click the map.");
    done(); place(btnW(tr("Exit full screen")), 6 * gUi);
    if (adsbmap::fullButton(S.full, "map")) S.full = !S.full;
    ImGui::PopStyleColor();
    done();
    {   // units: the label and the drop-down(s) stay together
        const float w = ImGui::CalcTextSize("Units").x + st.ItemSpacing.x + 130 * gUi + (S.units.metric() ? st.ItemSpacing.x + 70 * gUi : 0.f);
        place(w, 6 * gUi);
        unitsControls();
        done();
    }
    place(btnW(tr("Display")), 6 * gUi);
    displayControls();
    done();
    place(chkW("winds aloft"), 6 * gUi);
    if (ImGui::Checkbox("winds aloft", &S.winds)) { plat::prefs().setB("adsbWinds", S.winds); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wind arrows (downwind) at the aircraft, coloured by altitude: derived from Comm-B 5,0 + 6,0 replies, or reported in 4,4.");
    done(); place(chkW("GPS interference"), 6 * gUi);
    if (ImGui::Checkbox("GPS interference", &S.jamLayer)) { plat::prefs().setB("adsbGpsJam", S.jamLayer); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shades 0.5 degree squares by the share of aircraft that reported a poor GPS position (NACp / NIC low, or a position jump) in the last hour; at least 3 aircraft per square. An indicator, not proof of jamming.");
    done();
    gPred.load("adsb", true, false);
    place(chkW(tr("Predict positions")) + (gPred.predict ? 6 * gUi + chkW(tr("uncertainty")) : 0.f), 6 * gUi);
    gPred.controls();
    done();
    place(0, 6 * gUi);   // tiles / online map wrap by themselves (adsbmap::tileControls)
    adsbmap::tileControls(nullptr, false);
    bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    if (wrongRef) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, bottom + 6 * gUi));
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
    if (ImGui::BeginPopup("##adsbpos")) {
        ImGui::TextDisabled("Antenna position");
        positionFields(a);
        ImGui::TextDisabled("or right-click the map to put it there");
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##adsbmapctx")) {
        ImGui::TextDisabled("%.4f  %.4f", S.ctxLat, S.ctxLon);
        if (ImGui::MenuItem("Put the antenna here")) { setReference(a, S.ctxLat, S.ctxLon); S.mapHome = true; S.locMsg.clear(); }
        if (ImGui::MenuItem("Centre the map here")) { S.map.lat = S.ctxLat; S.map.lon = S.ctxLon; S.mapHome = false; }
        ImGui::EndPopup();
    }
}

void mapView(App& a, ImVec2 size) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    double areaLat = 0, areaLon = 0, nearest = 0;
    const bool wrongRef = on && positionLooksWrong(t, areaLat, areaLon, nearest);
    if (wrongRef && S.mapHome) { S.map.lat = areaLat; S.map.lon = areaLon; }   // show the aircraft, not an empty map around the wrong antenna
    const double homeLat = S.map.lat, homeLon = S.map.lon;
    const bool hov = adsbmap::draw(S.map, size);
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { adsbmap::unproject(ImGui::GetIO().MousePos, S.ctxLat, S.ctxLon); ImGui::OpenPopup("##adsbmapctx"); }
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
    gPred.load("adsb", true, false);
    if (on && gPred.predict) gPred.feedAdsb(t.aircraft, ImGui::GetTime());
    if (on && S.jamLayer) drawJam(dl, p0, size);
    if (on) {
        const bool lite = rcaps::lowPowerDraw();   // low-power drawing: no trails, paths ahead or wind arrows (gfx/render_caps.h)
        if (!lite) drawTrails(dl, t, p0, size);
        if (S.winds && !lite) drawWinds(dl, t);
        drawAircraft(dl, t, p0, size, click, clicked, best, hit);
    }
    if (clicked) S.sel = hit ? hit->icao : 0xFFFFFFFFu;
    dl->PopClipRect();
    mapCorner(a, p0, size, wrongRef, areaLat, areaLon, nearest);
    plat::prefs().setI("adsbZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& ac : t.aircraft) if (ac.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no lat/lon yet: ident, altitude and speed do not need a position. Set the antenna (Tuner, Locate) or wait for both even and odd CPR frames");
    else adsbmap::legend(size.x, "%d of %zu placed · white ADS-B, grey after 30 s", withPos, t.aircraft.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

}  // namespace adsbui

// main.cpp: the map or radar over the whole window while it is popped out (the "full screen" button); false when it is not
bool adsbFullMap(App& a, ImVec2 disp) {
    if (!S.full) return false;
    if (modeUi(a.family) != &kAdsbUi && modeUi(a.family) != &kModeAcUi) { S.full = false; return false; }   // Mode A/C draws this map too (#235: its full screen button did nothing)
    if (S.viewMode != 0 && S.viewMode != 2) { S.full = false; return false; }
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { S.full = false; return false; }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // NoBringToFrontOnFocus: a click on the map must not raise it over the aircraft panel (#235: the second aircraft clicked showed no panel)
    ImGui::Begin("##adsbfull", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    if (S.viewMode == 2) radar(a, ImGui::GetContentRegionAvail());
    else mapView(a, ImGui::GetContentRegionAvail());
    ImGui::Dummy(ImVec2(0, 0));   // the view leaves the cursor at the bottom edge: an item there so ImGui does not take it as growing the window
    ImGui::End();
    if (S.sel != 0xFFFFFFFFu) {   // the selected aircraft's readout (same as the side pane), as a panel on the right (#235)
        const float w = std::min(disp.x * 0.45f, 330.f * gUi);
        ImGui::SetNextWindowPos(ImVec2(disp.x - w - 8 * gUi, 8 * gUi));
        ImGui::SetNextWindowSize(ImVec2(w, disp.y - 16 * gUi));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("##adsbfulldet", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        detail(a);
        ImGui::End();
    }
    return true;
}
