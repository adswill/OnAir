// FLARM screens: the aircraft table (device ID, type, altitude, speed, track, climb, level, last heard, distance and bearing from the receiver),
// the live map through the shared slippy map of ADS-B (adsb_map.h), the receiver position, and the privacy note.
// Receive only, as the Open Glider Network does. Positions are what the aircraft broadcast openly; aircraft that set the no-track or stealth
// flag are only counted ("privacy-flagged"), never listed, drawn, saved or sent anywhere.
#include "dect2/flarm/flarm_outland.h"
#include "dect2/airports/airports.h"
#include <deque>
#include <unordered_map>
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/flarm/flarm_proto.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    double lat = 25.25, lon = 55.36;       // the receiver's position, shared with the ADS-B screen's setting
    bool haveRef = false;
    double pushedLat = 1e9, pushedLon = 1e9;
    uint32_t sel = 0xFFFFFFFFu;
    int view = 0;                          // 0 map, 1 table
    adsbmap::View map;
    char posText[64] = "";
    std::string msg;
};
State S;

bool live(const App& a) { return a.rx.standard == 67; }

// Out-landing alert (3.9, docs/SMART5-avia-3.9.md): an aircraft that flew and now stands still away from every airfield in the airport table
dect2::OutlandingWatch gOut;
std::deque<dect2::OutlandingAlert> gOutAlerts;
std::unordered_map<uint32_t, uint32_t> gOutPackets;   // the packet count last fed, per aircraft (the telemetry repeats a packet until a new one comes)
void outlandFeed(const dect2::FlarmTelemetry& t) {
    if (!gOut.nearestAirfieldKm) gOut.nearestAirfieldKm = [](double lat, double lon) {
        const auto h = dect2::AirportDb::builtin().nearby(lat, lon, 30, 1, false);
        return h.empty() ? -1.0 : h[0].km;
    };
    const double now = ImGui::GetTime();
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        uint32_t& last = gOutPackets[ac.id];
        if (ac.packets == last) continue;
        last = ac.packets;
        dect2::TrafficFix f; f.id = ac.id; f.t = now - ac.ageSec; f.lat = ac.lat; f.lon = ac.lon; f.altM = ac.altM;
        f.speedMs = ac.speedKmh / 3.6; f.climbMs = ac.climbMs; f.reportsGround = !ac.airborne;
        dect2::OutlandingAlert al;
        if (gOut.add(f, &al)) { gOutAlerts.push_back(al); if (gOutAlerts.size() > 50) gOutAlerts.pop_front(); }
    }
}
void outlandView() {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("An aircraft that was flying and now stands still on the ground more than 3 km from any airfield in the airport table: a possible out-landing (a retrieve, or a crew that may need help). Each landing is listed once."));
      ImGui::PopTextWrapPos(); }
    if (gOutAlerts.empty()) { ImGui::TextDisabled("%s", tr("none")); return; }
    for (auto it = gOutAlerts.rbegin(); it != gOutAlerts.rend(); ++it) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%06X: %s", (unsigned)(it->id & 0xFFFFFF), it->text.c_str());
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("adsbLat") && d.has("adsbLon")) { S.lat = d.getD("adsbLat", S.lat); S.lon = d.getD("adsbLon", S.lon); S.haveRef = true; }
    S.map.zoom = (int)d.getI("flarmZoom", 10);
    S.map.lat = S.lat; S.map.lon = S.lon;
    snprintf(S.posText, sizeof S.posText, "%.5f, %.5f", S.lat, S.lon);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string idText(uint32_t i) { char b[12]; snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }
const char* dir16(float deg) {
    static const char* n[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    return n[((int)std::lround(deg / 22.5f) % 16 + 16) % 16];
}
ImU32 typeColour(int t) {
    switch (t) {
    case 1: return IM_COL32(110, 200, 120, 255);     // glider
    case 2: return IM_COL32(230, 170, 70, 255);      // tow plane
    case 3: return IM_COL32(200, 120, 220, 255);     // helicopter
    case 7: case 6: return IM_COL32(110, 190, 230, 255);
    default: return IM_COL32(230, 230, 120, 255);
    }
}

void setPosition(App& a, double lat, double lon) {
    S.lat = std::max(-90.0, std::min(90.0, lat)); S.lon = std::max(-180.0, std::min(180.0, lon));
    S.haveRef = true;
    plat::prefs().setD("adsbLat", S.lat); plat::prefs().setD("adsbLon", S.lon);
    S.map.lat = S.lat; S.map.lon = S.lon;
    snprintf(S.posText, sizeof S.posText, "%.5f, %.5f", S.lat, S.lon);
    savePrefs(a);
}

void positionControls(App& a) {
    ImGui::TextDisabled("receiver position");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(200 * gUi, ImGui::GetContentRegionAvail().x));
    const bool enter = ImGui::InputText("##flpos", S.posText, sizeof S.posText, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 5 * gUi);
    if (ImGui::Button("Set") || enter) {
        double la, lo;
        if (parseCoordinates(S.posText, la, lo)) { setPosition(a, la, lo); S.msg = "position set"; }
        else if (fromMaidenhead(S.posText, la, lo)) { setPosition(a, la, lo); S.msg = "position set from the locator"; }
        else S.msg = "not a position: try 54.687, 25.280 or a locator such as KO24";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where your antenna is (decimal degrees, degrees minutes seconds, or a Maidenhead locator). FLARM sends only the low bits of a position;\n"
                                                  "the receiver completes them around this point (within about 300 km). The same setting as the ADS-B screen's.");
    if (!S.msg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.msg.c_str()); }
}

void privacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only, as the Open Glider Network does. Positions are what the aircraft broadcast openly. The no-track and stealth flags are respected: "
                        "such aircraft are never listed, mapped, saved or sent anywhere, only counted. The newer FLARM protocol cannot be decoded and is only counted.");
    ImGui::PopTextWrapPos();
}

void tableView(App& a, const dect2::FlarmTelemetry& t) {
    if (ImGui::BeginTable("##flt", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Altitude");
        ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Track"); ImGui::TableSetupColumn("Climb"); ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Packets");
        ImGui::TableHeadersRow();
        for (const auto& ac : t.aircraft) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(idText(ac.id).c_str(), S.sel == ac.id, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == ac.id ? 0xFFFFFFFFu : ac.id;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::flarm::aircraftTypeName(ac.acType));
            ImGui::TableNextColumn(); ImGui::Text("%d m", ac.altM);
            ImGui::TableNextColumn(); ImGui::Text("%.0f km/h", ac.speedKmh);
            ImGui::TableNextColumn(); ImGui::Text("%.0f %s", ac.trackDeg, dir16(ac.trackDeg));
            ImGui::TableNextColumn(); ImGui::Text("%+.1f m/s", ac.climbMs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", ac.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s ago", ac.ageSec);
            ImGui::TableNextColumn();
            if (ac.hasRange) ImGui::Text("%.1f km %s", ac.distKm, dir16(ac.bearingDeg)); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", ac.packets);
        }
        ImGui::EndTable();
    }
    (void)a;
}

void mapView(App& a, const dect2::FlarmTelemetry& t) {
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
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        const ImVec2 q = adsbmap::project(ac.lat, ac.lon);
        const bool sel = S.sel == ac.id;
        const ImU32 col = adsbmap::acColour(adsbmap::AcSrc::Flarm, ac.ageSec);
        if (ac.track.size() > 1) {
            ImVec2 prev = adsbmap::project(ac.track[0].lat, ac.track[0].lon);
            for (size_t i = 1; i < ac.track.size(); i++) {
                const ImVec2 p = adsbmap::project(ac.track[i].lat, ac.track[i].lon);
                dl->AddLine(prev, p, (col & 0x00FFFFFF) | 0x90000000, 1.5f);
                prev = p;
            }
        }
        const float r = (sel ? 13.f : 11.f) * gUi, h = ac.trackDeg * 3.14159265f / 180.f, ch = std::cos(h), sh = std::sin(h);
        auto gv = [&](float x, float y) { return ImVec2(q.x + (x * ch - y * sh) * r, q.y + (x * sh + y * ch) * r); };
        ImVec2 g[12] = { gv(0, -1), gv(0.08f, -0.2f), gv(1.15f, 0.05f), gv(1.15f, 0.18f), gv(0.08f, 0.08f), gv(0.07f, 0.55f),
                         gv(-0.07f, 0.55f), gv(-0.08f, 0.08f), gv(-1.15f, 0.18f), gv(-1.15f, 0.05f), gv(-0.08f, -0.2f), gv(0, -1) };
        dl->AddConcavePolyFilled(g, 12, col);
        dl->AddPolyline(g, 12, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, sel ? 1.6f : 1.f);
        char b[80]; snprintf(b, sizeof b, "%s  %s  %dm", idText(ac.id).c_str(), dect2::flarm::aircraftTypeName(ac.acType), ac.altM);
        dl->AddText(ImVec2(q.x + 9 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), b);
        if (clicked) { const float dd = std::hypot(click.x - q.x, click.y - q.y); if (dd < bestD) { bestD = dd; S.sel = ac.id; } }
    }
    dl->PopClipRect();
    adsbmap::legend(size.x, "%u aircraft, %u on the map   |   right of the table: details", t.aircraftCount, t.withPosition);
    (void)a;
}

void tab(App& a) {
    loadState();
    subNav("flv", S.view, {"Map", "Table", "Out-landings"});
    ImGui::SameLine(0, 12 * gUi);
    positionControls(a);
    if (S.view == 0) { ImGui::SameLine(0, 12 * gUi); adsbmap::tileControls("never the position itself"); }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); privacyNote(); return; }
    const dect2::FlarmTelemetry& t = a.rx.flarm;
    if (!S.haveRef) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "Set the receiver position: without it the packets give no positions.");
    if (t.aircraft.empty() && !t.newProtocol)
        ImGui::TextDisabled("%s", t.state >= 1 ? "energy on 868.2 / 868.4 MHz, no decodable packet yet" : "listening on 868.2 and 868.4 MHz (tune 868.3 MHz, at least 1 Msps)");
    if (S.view == 0) mapView(a, t); else if (S.view == 2) outlandView(); else tableView(a, t);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::FlarmTelemetry& t = a.rx.flarm;
    if (ImGui::BeginTable("##fll", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Alt");
        for (const auto& ac : t.aircraft) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ac.ageSec < 10 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Selectable(idText(ac.id).c_str(), S.sel == ac.id, ImGuiSelectableFlags_SpanAllColumns)) S.sel = ac.id;
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::flarm::aircraftTypeName(ac.acType));
            ImGui::TableNextColumn(); ImGui::Text("%d", ac.altM);
        }
        ImGui::EndTable();
    }
    if (t.aircraft.empty()) ImGui::TextDisabled("no aircraft heard yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::FlarmTelemetry& t = a.rx.flarm;
    kvRow(a, "state", "%s", dect2::flarmSummary(t).c_str());
    kvRow(a, "packets", "%llu old protocol decoded, %llu bad (CRC or parity), %.1f per second", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.packetsPerSec);
    kvRow(a, "channels", "868.2 MHz %llu, 868.4 MHz %llu", (unsigned long long)t.perChannel[0], (unsigned long long)t.perChannel[1]);
    kvRow(a, "FLARM (new protocol)", "%llu packets from %llu devices, not decodable: no positions", (unsigned long long)t.newProtocol, (unsigned long long)t.newProtocolDevices);
    kvRow(a, "privacy-flagged aircraft", "%llu (no-track / stealth / anonymous): counted only, nothing kept", (unsigned long long)t.privacyFlagged);
    kvRow(a, "other packets", "%llu", (unsigned long long)t.otherType);
    kvRow(a, "signal", "%.1f dB over the noise, noise %.1f dB", t.snrDb, t.noiseDb);
    kvRow(a, "farthest", "%.1f km", t.maxRangeKm);
    kvRow(a, "key time", "%s", t.clockFromWall ? "the computer's clock (keep it right to a few seconds)" : "fixed");
    kvRow(a, "input", "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    privacyNote();
}

void decoder(App& a, bool&) {
    loadState();
    positionControls(a);
    flowNext();
    if (ImGui::SmallButton("Clear")) { a.engine.flarm().clearResults(); S.sel = 0xFFFFFFFFu; }
}

void synth(App& a, bool& changed) {
    loadState();
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated aircraft around the receiver position, six made-up devices (one no-track, one stealth, one new protocol)");
    if (sc.modeVal[0] != S.lat || sc.modeVal[1] != S.lon) { sc.modeVal[0] = S.lat; sc.modeVal[1] = S.lon; changed = true; }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Packets", !on ? 0 : a.rx.flarm.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::flarmSummary(a.rx.flarm).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "FLARM";
    if (live(a)) l2 = dect2::flarmSummary(a.rx.flarm);
}

void tick(App& a) {
    loadState();
    if (S.pushedLat != S.lat || S.pushedLon != S.lon) {
        S.pushedLat = S.lat; S.pushedLon = S.lon;
        if (S.haveRef) a.engine.flarm().setReference(S.lat, S.lon); else a.engine.flarm().clearReference();
    }
    if (live(a)) outlandFeed(a.rx.flarm);
}

} // namespace

extern const ModeUi kFlarmUi;
const ModeUi kFlarmUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Aircraft",
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
