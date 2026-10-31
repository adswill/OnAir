// View > Map: a world map (the ADS-B map's tiles) to find a position by coordinates (decimal or D°M'S") or by Maidenhead locator,
// and to read a clicked position's coordinates, locator, and distance and bearing from the receiver location.
#include "../app.h"
#include "../adsb/adsb_map.h"
#include "wx_overlay.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    adsbmap::View map;
    bool marker = false;
    double mLat = 0, mLon = 0;
    char coordBuf[96] = "", locBuf[16] = "";
    std::string msg;              // why the typed text was not understood
    // layers: shown or not (saved), the data they read (refreshed once a second; kept when the mode is not running), what was clicked
    bool layerOn[dect2::LayerCount + 1] = {true, true, true, true, true, true, true, true};   // the last one: the receiver
    double refreshedAt = -1e9;
    dect2::AdsbTelemetry adsb; dect2::AisTelemetry ais; dect2::PacketTelemetry packet; dect2::SondeTelemetry sonde; dect2::CellTelemetry cell; dect2::DmrTelemetry dmr; dect2::VdesTelemetry vdes;
    std::vector<dect2::MapMarker> markers;
    std::vector<dect2::SatMapPoint> sats;
    std::vector<MapCellTower> towers;
    int selLayer = -1; std::string selId;
};
State S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    receiverLocation(S.map.lat, S.map.lon);
    S.map.zoom = (int)plat::prefs().getI("mapZoom", 7);
    for (int i = 0; i <= dect2::LayerCount; i++) { char k[24]; snprintf(k, sizeof k, "mapLayer%d", i); S.layerOn[i] = plat::prefs().getB(k, true); }
}

const char* const kLayerName[] = {"ADS-B aircraft", "AIS ships", "APRS stations", "Radiosondes", "Satellites", "Cell towers", "DMR radios", "Receiver"};
ImU32 layerColour(int l, int alpha = 255) {
    static const ImU32 c[] = {IM_COL32(255, 200, 60, 255), IM_COL32(80, 170, 255, 255), IM_COL32(120, 230, 120, 255), IM_COL32(255, 120, 200, 255), IM_COL32(200, 160, 255, 255), IM_COL32(240, 240, 240, 255), IM_COL32(255, 160, 70, 255), IM_COL32(110, 210, 230, 255)};
    const ImU32 v = c[std::max(0, std::min(7, l))];
    return (v & 0x00FFFFFF) | ((ImU32)alpha << 24);
}

// the results of every receiver, once a second (they stay after the mode stops: the telemetry of a receiver that is not running is its kept list)
void refresh(App& a) {
    const double now = ImGui::GetTime();
    if (now - S.refreshedAt < 1.0) return;
    S.refreshedAt = now;
    a.engine.adsb().telemetry(S.adsb, S.adsb.seq);
    a.engine.ais().telemetry(S.ais, S.ais.seq);
    a.engine.packet().telemetry(S.packet, S.packet.seq);
    a.engine.sonde().telemetry(S.sonde, S.sonde.seq);
    a.engine.cell().telemetry(S.cell, S.cell.seq);
    a.engine.dmr().telemetry(S.dmr, S.dmr.seq);
    a.engine.vdes().telemetry(S.vdes, S.vdes.seq);
    S.markers.clear();
    auto add = [](std::vector<dect2::MapMarker> v) { for (auto& m : v) S.markers.push_back(std::move(m)); };
    add(dect2::markersFromAdsb(S.adsb)); add(dect2::markersFromAis(S.ais)); add(dect2::markersFromVdes(S.vdes)); add(dect2::markersFromAprs(S.packet)); add(dect2::markersFromSonde(S.sonde)); add(dect2::markersFromDmr(S.dmr));
    S.towers.clear();
    cellMapTowers(S.cell, S.towers);
    S.sats.clear();
    std::vector<MapSatRef> refs;
    passesMapSatellites(refs);
    const double t = (double)std::time(nullptr);
    for (const MapSatRef& r : refs) {
        dect2::SatMapPoint p;
        if (dect2::satelliteMapPoint(*r.sgp, t, 45, 45, 1.5, p)) { p.name = r.name; S.sats.push_back(std::move(p)); }
    }
}

int countOf(int layer) {
    if (layer == dect2::LayerSat) return (int)S.sats.size();
    if (layer == dect2::LayerCell) return (int)S.towers.size();
    int n = 0; for (const auto& m : S.markers) if (m.layer == layer) n++;
    return n;
}

void polyline(ImDrawList* dl, const std::vector<std::pair<double, double>>& pts, ImU32 col, float w, size_t from = 0, size_t to = (size_t)-1) {
    ImVec2 prev; bool have = false; double plon = 0;
    for (size_t i = from; i < pts.size() && i <= to; i++) {
        const ImVec2 q = adsbmap::project(pts[i].first, pts[i].second);
        if (have && std::fabs(pts[i].second - plon) < 180) dl->AddLine(prev, q, col, w);
        prev = q; plon = pts[i].second; have = true;
    }
}

void drawLayers(ImDrawList* dl, const ImVec2& p0, const ImVec2& size) {
    const bool* on = S.layerOn;
    if (on[dect2::LayerCell])
        for (const MapCellTower& c : S.towers) {
            const ImVec2 q = adsbmap::project(c.lat, c.lon);
            const ImU32 col = (ImU32)c.colour;
            if (c.rangeM > 0) {
                const float rpx = std::fabs(adsbmap::project(c.lat + c.rangeM / 111320.0, c.lon).y - q.y);
                if (rpx > 2 && rpx < 4000) dl->AddCircle(q, rpx, (col & 0x00FFFFFF) | (70u << 24), 48);
            }
            dl->AddRectFilled(ImVec2(q.x - 4 * gUi, q.y - 4 * gUi), ImVec2(q.x + 4 * gUi, q.y + 4 * gUi), col);
            dl->AddRect(ImVec2(q.x - 4 * gUi, q.y - 4 * gUi), ImVec2(q.x + 4 * gUi, q.y + 4 * gUi), IM_COL32(0, 0, 0, 200));
            { const unsigned t = adsbmap::overlayText(); dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), IM_COL32((t >> 16) & 255, (t >> 8) & 255, t & 255, 230), c.label.c_str()); }
        }
    if (on[dect2::LayerSat])
        for (const dect2::SatMapPoint& sp : S.sats) {
            const ImU32 col = layerColour(dect2::LayerSat);
            if (sp.footprintKm > 0) { auto c = dect2::circlePoints(sp.lat, sp.lon, sp.footprintKm, 90); c.push_back(c[0]); polyline(dl, c, layerColour(dect2::LayerSat, 90), 1.f * gUi); }
            polyline(dl, sp.track, layerColour(dect2::LayerSat, 90), 1.f * gUi, 0, sp.nowIndex);        // where it has been: faint
            polyline(dl, sp.track, layerColour(dect2::LayerSat, 200), 1.5f * gUi, sp.nowIndex);         // where it goes
            const ImVec2 q = adsbmap::project(sp.lat, sp.lon);
            dl->AddCircleFilled(q, 4.5f * gUi, col);
            dl->AddCircle(q, 4.5f * gUi, IM_COL32(0, 0, 0, 220));
            dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), col, sp.name.c_str());
        }
    for (const dect2::MapMarker& m : S.markers) {
        if (!on[m.layer]) continue;
        const ImU32 col = layerColour(m.layer);
        if (m.track.size() > 1) {
            ImVec2 prev = adsbmap::project(m.track[0].first, m.track[0].second);
            for (size_t i = 1; i < m.track.size(); i++) {
                const ImVec2 q = adsbmap::project(m.track[i].first, m.track[i].second);
                if (std::fabs(m.track[i].second - m.track[i - 1].second) < 180) dl->AddLine(prev, q, layerColour(m.layer, 110), 1.f * gUi);
                prev = q;
            }
        }
        const ImVec2 q = adsbmap::project(m.lat, m.lon);
        if (q.x < p0.x - 20 || q.x > p0.x + size.x + 20 || q.y < p0.y - 20 || q.y > p0.y + size.y + 20) continue;
        const bool sel = m.layer == S.selLayer && m.id == S.selId;
        if (m.headingDeg >= 0) {   // a triangle pointing the way it goes
            const float h = (float)(m.headingDeg * M_PI / 180.0), sx = std::sin(h), sy = -std::cos(h), r = 6 * gUi;
            dl->AddTriangleFilled(ImVec2(q.x + sx * r, q.y + sy * r), ImVec2(q.x - sx * r * 0.7f - sy * r * 0.6f, q.y - sy * r * 0.7f + sx * r * 0.6f),
                                  ImVec2(q.x - sx * r * 0.7f + sy * r * 0.6f, q.y - sy * r * 0.7f - sx * r * 0.6f), col);
        } else dl->AddCircleFilled(q, 4 * gUi, col);
        if (sel) dl->AddCircle(q, 10 * gUi, IM_COL32(255, 255, 255, 230), 24, 1.5f * gUi);
        if (size.x > 300 * gUi) dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32((adsbmap::overlayText() >> 16) & 255, (adsbmap::overlayText() >> 8) & 255, adsbmap::overlayText() & 255, 220), m.label.c_str());
    }
}

// the marker (or satellite) under a click, if any
bool hitTest(ImVec2 click) {
    float best = 12 * gUi; bool found = false;
    auto consider = [&](int layer, const std::string& id, double la, double lo) {
        const ImVec2 q = adsbmap::project(la, lo);
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (d < best) { best = d; S.selLayer = layer; S.selId = id; found = true; }
    };
    for (const auto& m : S.markers) if (S.layerOn[m.layer]) consider(m.layer, m.id, m.lat, m.lon);
    if (S.layerOn[dect2::LayerSat]) for (const auto& sp : S.sats) consider(dect2::LayerSat, std::to_string(sp.norad) + sp.name, sp.lat, sp.lon);
    if (S.layerOn[dect2::LayerCell]) for (const auto& c : S.towers) consider(dect2::LayerCell, c.label, c.lat, c.lon);
    return found;
}

void infoPopup() {
    if (!ImGui::BeginPopup("##mapinfo")) return;
    const dect2::MapMarker* mk = nullptr;
    for (const auto& m : S.markers) if (m.layer == S.selLayer && m.id == S.selId) mk = &m;
    if (mk) {
        const char* kind = S.selLayer == dect2::LayerAdsb ? "ICAO" : S.selLayer == dect2::LayerAis ? "MMSI" : S.selLayer == dect2::LayerAprs ? "callsign" : S.selLayer == dect2::LayerDmr ? "Src" : "serial";
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(layerColour(mk->layer)), "%s", mk->label.c_str());
        ImGui::TextDisabled("%s %s, %s", kind, mk->id.c_str(), kLayerName[mk->layer]);
        ImGui::Text("%.5f, %.5f  (%s)", mk->lat, mk->lon, dect2::toMaidenhead(mk->lat, mk->lon, 6).c_str());
        if (mk->hasAlt) ImGui::Text("altitude %.0f m (%.0f ft)", mk->altM, mk->altM / 0.3048);
        if (mk->hasSpeed) ImGui::Text("speed %.0f kn (%.0f km/h)", mk->speedKt, mk->speedKt * 1.852);
        if (mk->headingDeg >= 0) ImGui::Text("heading %.0fÂ°", mk->headingDeg);
        double rl, ro, km, brg; receiverLocation(rl, ro); dect2::greatCircle(rl, ro, mk->lat, mk->lon, km, brg);
        ImGui::Text("%.1f km at %.0fÂ° from the receiver", km, brg);
        if (ImGui::Button(tr("Point antenna rotator"))) rotatorPointBearing(brg, 0);
        ImGui::Text("last heard %s ago", dect2::ageText(mk->ageSec).c_str());
        if (!mk->detail.empty()) ImGui::TextDisabled("%s", mk->detail.c_str());
    } else if (S.selLayer == dect2::LayerSat) {
        for (const auto& sp : S.sats)
            if (std::to_string(sp.norad) + sp.name == S.selId) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(layerColour(dect2::LayerSat)), "%s", sp.name.c_str());
                ImGui::TextDisabled("NORAD %d, satellite", sp.norad);
                ImGui::Text("under it: %.2f, %.2f", sp.lat, sp.lon);
                ImGui::Text("altitude %.0f km, sees a circle of %.0f km radius", sp.altKm, sp.footprintKm);
            }
    } else if (S.selLayer == dect2::LayerCell) {
        for (const auto& c : S.towers)
            if (c.label == S.selId) { ImGui::Text("%s", c.label.c_str()); ImGui::TextDisabled("cell tower, %.5f, %.5f, range %d m", c.lat, c.lon, c.rangeM); }
    } else ImGui::TextDisabled("gone from the list");
    ImGui::EndPopup();
}

void layerPanel(const ImVec2& p0, const ImVec2& size) {
    const float w = 190 * gUi;
    const float h = (dect2::LayerCount + 1) * ImGui::GetFrameHeightWithSpacing() + 8 * gUi;
    const ImVec2 a(p0.x + size.x - w - 8 * gUi, p0.y + 8 * gUi);
    if (a.x < p0.x + 120 * gUi) return;   // no room in a narrow map
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32((adsbmap::panelColour() >> 16) & 255, (adsbmap::panelColour() >> 8) & 255, adsbmap::panelColour() & 255, 205), 4 * gUi);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 6 * gUi, a.y + 4 * gUi));
    ImGui::BeginGroup();
    for (int i = 0; i <= dect2::LayerCount; i++) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(layerColour(i)));
        char l[64]; snprintf(l, sizeof l, "%s (%d)", kLayerName[i], countOf(i));
        if (i == dect2::LayerCount) snprintf(l, sizeof l, "%s", kLayerName[i]);
        if (ImGui::Checkbox(l, &S.layerOn[i])) { char k[24]; snprintf(k, sizeof k, "mapLayer%d", i); plat::prefs().setB(k, S.layerOn[i]); }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndGroup();
}

void place(double lat, double lon, bool centre, int zoom = 0) {
    S.marker = true; S.mLat = lat; S.mLon = lon; S.msg.clear();
    if (centre) { S.map.lat = lat; S.map.lon = lon; if (zoom) S.map.zoom = std::max(S.map.zoom, zoom); }
}

void inputs() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Coordinates");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(230 * gUi);
    if (ImGui::InputTextWithHint("##mapcoord", "54.687, 25.280 or 54 41 13 N 25 16 48 E", S.coordBuf, sizeof S.coordBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double la, lo;
        if (dect2::parseCoordinates(S.coordBuf, la, lo)) place(la, lo, true);
        else S.msg = "not a position: type latitude then longitude, decimal or degrees minutes seconds";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Latitude and longitude, then Enter: 54.687, 25.280 / -33.9 151.2 / 54.687N 25.28E / 54\xC2\xB0" "41'13\"N 25\xC2\xB0" "16'48\"E");
    ImGui::SameLine(0, 12 * gUi);
    ImGui::TextDisabled("Locator");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputTextWithHint("##maploc", "KO24ab", S.locBuf, sizeof S.locBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double la, lo;
        const size_t n = std::strlen(S.locBuf);
        if (dect2::fromMaidenhead(S.locBuf, la, lo)) place(la, lo, true, n <= 2 ? 0 : n <= 4 ? 7 : n <= 6 ? 11 : 14);
        else S.msg = "not a locator: 2, 4, 6 or 8 characters, e.g. KO24, KO24ab, KO24ab12";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A Maidenhead grid locator (4, 6 or 8 characters), then Enter: the marker goes to the centre of the square");
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", S.msg.c_str()); }
}

void markerInfo(App& a) {
    if (!S.marker) { ImGui::TextDisabled("Click the map, or type coordinates or a locator, to place the marker."); return; }
    char b[160];
    const std::string loc6 = dect2::toMaidenhead(S.mLat, S.mLon, 6), loc8 = dect2::toMaidenhead(S.mLat, S.mLon, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.6f, %.6f", S.mLat, S.mLon);
    ImGui::SameLine(); ImGui::TextDisabled("%s %s", dect2::toDms(S.mLat, true).c_str(), dect2::toDms(S.mLon, false).c_str());
    ImGui::SameLine(0, 12 * gUi);
    ImGui::Text("%s", loc6.c_str());
    ImGui::SameLine(); ImGui::TextDisabled("(%s)", loc8.c_str());
    double rl, ro; receiverLocation(rl, ro);
    double km, brg; dect2::greatCircle(rl, ro, S.mLat, S.mLon, km, brg);
    ImGui::SameLine(0, 12 * gUi);
    if (km < 10) ImGui::Text("%.2f km", km); else ImGui::Text("%.0f km", km);
    ImGui::SameLine(); ImGui::Text("%.0f\xC2\xB0", brg);
    ImGui::SameLine(); ImGui::TextDisabled(receiverLocationSet() ? "from my location" : "from the placeholder location");
    if (ImGui::SmallButton("Copy coordinates")) { snprintf(b, sizeof b, "%.6f, %.6f", S.mLat, S.mLon); ImGui::SetClipboardText(b); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy locator")) ImGui::SetClipboardText(loc6.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies %s (the 6-character locator)", loc6.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Use as my location")) setReceiverLocation(a, S.mLat, S.mLon);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make this the receiver location (ADS-B ranges, the pass planner, ...)");
}

void mapView(App& a) {
    refresh(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120 * gUi, ImGui::GetContentRegionAvail().y));
    adsbmap::draw(S.map, size);
    ImVec2 click;
    if (adsbmap::clickedAt(click)) {
        if (hitTest(click)) ImGui::OpenPopup("##mapinfo");
        else { S.selLayer = -1; double la, lo; adsbmap::unproject(click, la, lo); lo = std::fmod(lo + 540, 360) - 180; place(la, lo, false); }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    double rl, ro; receiverLocation(rl, ro);
    const ImVec2 h = adsbmap::project(rl, ro);   // the receiver: a ringed dot
    wxoverlay::draw(dl);   // a weather-satellite picture being received lies on the ground, under every marker
    drawLayers(dl, p0, size);
    if (S.layerOn[dect2::LayerCount]) {
        dl->AddCircleFilled(h, 4 * gUi, IM_COL32(110, 210, 230, 255));
        dl->AddCircle(h, 8 * gUi, IM_COL32(110, 210, 230, 220), 24, 1.5f * gUi);
        dl->AddText(ImVec2(h.x + 10 * gUi, h.y - 7 * gUi), IM_COL32(110, 210, 230, 255), "me");
    }
    if (S.marker) {   // the marker: a pin with its point on the position
        const ImVec2 q = adsbmap::project(S.mLat, S.mLon);
        const ImU32 c = IM_COL32(240, 90, 80, 255);
        dl->AddLine(h, q, IM_COL32(255, 255, 255, 110), 1 * gUi);
        dl->AddTriangleFilled(ImVec2(q.x - 4 * gUi, q.y - 10 * gUi), ImVec2(q.x + 4 * gUi, q.y - 10 * gUi), q, c);
        dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 6 * gUi, c);
        dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 2.2f * gUi, IM_COL32(255, 255, 255, 230));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("me")) { S.map.lat = rl; S.map.lon = ro; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centre the map on my location");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the marker or your location");
    layerPanel(p0, size);
    wxoverlay::controls(a, p0.x + 8 * gUi, p0.y + 8 * gUi + ImGui::GetFrameHeightWithSpacing() * 1.4f, std::min(300.f * gUi, size.x - 220 * gUi));
    infoPopup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(size.x, "click a marker for details or the map to place the marker, drag to pan, wheel to zoom");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // ImGui wants an item after moving the cursor
}

} // namespace

void mapWindow(App& a) {
    if (!a.mapOpen) return;
    load();
    firstWindowSize(820 * gUi, 600 * gUi);
    if (!ImGui::Begin("Map", &a.mapOpen)) { ImGui::End(); return; }
    inputs();
    markerInfo(a);
    mapView(a);
    static int savedZoom = -1;
    if (savedZoom != S.map.zoom) { plat::prefs().setI("mapZoom", S.map.zoom); savedZoom = S.map.zoom; }
    ImGui::End();
}
