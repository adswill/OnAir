// Lightning position (network) screens: this station's timing (which source stamps the detections and its estimated error, every source ranked),
// the server's table of stations, the strikes on the map with age colours and error ellipses, the list of strikes. One click starts a role
// with sensible settings; the propagation model, the clock figures and the reference are under Advanced in the decoder options.
#include "app.h"
#include "adsb/adsb_map.h"
#include "dect2/common/time_fmt.h"
#include "dect2/sferics/ltpos_core.h"
#include "dect2/sferics/ltpos_rx.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/kit/kit.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/mode_panel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace {

using namespace dect2::ltpos;

struct State {
    bool loaded = false, wasRunning = false, advanced = false, mapInit = false;
    int role = 0, port = 47390, sysKind = 1, refSel = 0, minStations = 4;   // refSel: 0 automatic, -1 none, else a reference id
    char host[128] = "", key[128] = "", name[32] = "";
    float thrDb = 12.f, sysSigmaUs = 0.f, velocity = 0.9993f, hopKm = 80.f, keepMin = 60.f;
    bool useHw = true, hops = false, serverIsStation = true;
    std::string applied;
    adsbmap::View map;
};
State S;

bool live(const App& a) { return a.rx.standard == 791; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.role = (int)p.getI("ltposRole", 0);
    S.port = (int)p.getI("ltposPort", 47390);
    S.sysKind = (int)p.getI("ltposSysKind", 1);
    S.refSel = (int)p.getI("ltposRef", 0);
    S.minStations = (int)p.getI("ltposMin", 4);
    snprintf(S.host, sizeof S.host, "%s", p.getS("ltposHost", "").c_str());
    snprintf(S.key, sizeof S.key, "%s", p.getS("ltposKey", "").c_str());   // stored in the preferences only; never logged or sent (it only signs)
    std::string nm = p.getS("ltposName", "");
    if (nm.empty()) { std::random_device rd; char b[32]; snprintf(b, sizeof b, "station-%04x", (unsigned)(rd() & 0xFFFF)); nm = b; p.setS("ltposName", nm); }
    snprintf(S.name, sizeof S.name, "%s", nm.c_str());
    S.thrDb = (float)p.getD("ltposThr", 12.0);
    S.sysSigmaUs = (float)p.getD("ltposSysUs", 0.0);
    S.velocity = (float)p.getD("ltposVf", 0.9993);
    S.hopKm = (float)p.getD("ltposHopKm", 80.0);
    S.keepMin = (float)p.getD("ltposKeep", 60.0);
    S.useHw = p.getB("ltposHw", true);
    S.hops = p.getB("ltposHops", false);
    S.serverIsStation = p.getB("ltposSrvSt", true);
}

void save() {
    auto& p = plat::prefs();
    p.setI("ltposRole", S.role); p.setI("ltposPort", S.port); p.setI("ltposSysKind", S.sysKind); p.setI("ltposRef", S.refSel); p.setI("ltposMin", S.minStations);
    p.setS("ltposHost", S.host); p.setS("ltposKey", S.key); p.setS("ltposName", S.name);
    p.setD("ltposThr", S.thrDb); p.setD("ltposSysUs", S.sysSigmaUs); p.setD("ltposVf", S.velocity); p.setD("ltposHopKm", S.hopKm); p.setD("ltposKeep", S.keepMin);
    p.setB("ltposHw", S.useHw); p.setB("ltposHops", S.hops); p.setB("ltposSrvSt", S.serverIsStation);
    p.flush();
}

// the reference in use: the one picked, or the nearest listed station that falls inside the capture
int chosenRef(const App& a) {
    if (S.refSel != 0) return std::max(0, S.refSel);
    const double centre = a.rx.ltpos.centerHz, rate = a.rx.ltpos.inputRate;
    if (rate <= 0) return 0;
    double lat, lon;
    receiverLocation(lat, lon);
    int best = 0;
    double bestD = 1e18;
    for (const RefTx& r : builtinRefs()) {
        if (std::fabs(r.freqHz - centre) >= rate * 0.45) continue;
        const double d = geodesicM(lat, lon, r.lat, r.lon);
        if (d < bestD) { bestD = d; best = r.id; }
    }
    return best;
}

std::string usText(double us) {
    char b[48];
    if (us < 0) return "-";
    if (us < 1) snprintf(b, sizeof b, "%.0f ns", us * 1e3);
    else if (us < 1000) snprintf(b, sizeof b, "%.1f us", us);
    else if (us < 1e6) snprintf(b, sizeof b, "%.1f ms", us / 1e3);
    else snprintf(b, sizeof b, "%.1f s", us / 1e6);
    return b;
}

std::string ago(double s) {
    char b[48];
    if (s < 0) return "-";
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

void notRunning(const App& a) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); }

// ---- this station's clock
void clockPane(App& a) {
    const dect2::LtPosTelemetry& t = a.rx.ltpos;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(170 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    const TimeSrc src = (TimeSrc)t.timeSource;
    kv(tr("state"), "%s", dect2::ltposSummary(t).c_str());
    kv(tr("timing source"), "%s", tr(timeSrcName(src)));
    kv(tr("estimated error"), "%s", usText(t.clockSigmaUs).c_str());
    kv(tr("radio clock"), "%+.3f ppm (+- %.3f)", t.ppm, (double)t.ppmSigma);
    ImGui::PushTextWrapPos(0);
    if (t.fine) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", tr("Good enough for a position fix."));
    else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Coarse: this station's detections help to associate, but are never used for the position (1 us is 300 m; a millisecond is 300 km)."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Timing sources, best first"));
    if (ImGui::BeginTable("##ltsrc", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 1; i <= 6; i++) {
            const float v = (size_t)(i - 1) < t.srcSigmaUs.size() ? t.srcSigmaUs[(size_t)i - 1] : -1.f;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (i == t.timeSource) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", tr(timeSrcName((TimeSrc)i)));
            else ImGui::TextUnformatted(tr(timeSrcName((TimeSrc)i)));
            ImGui::TableNextColumn();
            ImGui::PushFont(a.mono, 0);
            if (v >= 0) ImGui::Text("%s", usText(v).c_str()); else ImGui::TextDisabled("%s", tr("not available"));
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    if (t.refOn) {
        ImGui::Spacing();
        kv(tr("reference"), "%s", t.refName.c_str());
        kv(tr("reference timing"), "%s, %s, %.0f dB", t.refLocked ? tr("locked") : tr("searching"), usText(t.refSigmaUs).c_str(), (double)t.refSnrDb);
        kv(tr("carrier offset"), "%+.2f Hz", t.refFreqErrHz);
    }
    if (!t.netStatus.empty()) { ImGui::Spacing(); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.netStatus.c_str()); ImGui::PopTextWrapPos(); }
}

// ---- the server's stations
void stationsPane(App& a) {
    const dect2::LtPosTelemetry& t = a.rx.ltpos;
    if (t.role != 2) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("The stations and their clocks are shown on the server.")); ImGui::PopTextWrapPos(); return; }
    if (!ImGui::BeginTable("##ltst", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Station"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Timing")); ImGui::TableSetupColumn(tr("Error")); ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("In fixes")); ImGui::TableSetupColumn(tr("Last"));
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const dect2::LtStationRow& r : t.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
        ImGui::TableNextColumn();
        if (r.fine) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", tr(timeSrcName((TimeSrc)r.timeSource)));
        else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr(timeSrcName((TimeSrc)r.timeSource)));
        ImGui::TableNextColumn(); ImGui::Text("%s", usText(r.sigmaUs).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.detections);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.used);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ago(r.ageS).c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

// ---- the map
ImU32 ageColour(double minutes, int alpha) {
    if (minutes < 1) return IM_COL32(255, 255, 255, alpha);
    if (minutes < 5) return IM_COL32(255, 225, 90, alpha);
    if (minutes < 15) return IM_COL32(255, 150, 50, alpha);
    if (minutes < 30) return IM_COL32(235, 80, 50, alpha);
    return IM_COL32(150, 60, 60, alpha);
}

void ellipse(ImDrawList* dl, const dect2::LtStrikeRow& s, ImU32 col) {
    if (s.majorKm <= 0) return;
    ImVec2 pts[37];
    const double az = (double)s.azDeg * 3.14159265358979 / 180;
    for (int i = 0; i <= 36; i++) {
        const double th = i * 10.0 * 3.14159265358979 / 180;
        const double x = (double)s.majorKm * std::cos(th), y = (double)s.minorKm * std::sin(th);   // along and across the major axis, km
        const double north = x * std::cos(az) - y * std::sin(az), east = x * std::sin(az) + y * std::cos(az);
        double la, lo;
        geodesicDirect(s.lat, s.lon, std::atan2(east, north) * 180 / 3.14159265358979, std::hypot(north, east) * 1e3, la, lo);
        pts[i] = adsbmap::project(la, lo);
    }
    dl->AddPolyline(pts, 37, col, 0, 1.f * gUi);
}

void mapPane(App& a) {
    const dect2::LtPosTelemetry& t = a.rx.ltpos;
    if (!S.mapInit) { S.mapInit = true; receiverLocation(S.map.lat, S.map.lon); S.map.zoom = 5; }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(180 * gUi, ImGui::GetContentRegionAvail().y));
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const int64_t nowS = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int drawn = 0;
    for (auto it = t.strikes.rbegin(); it != t.strikes.rend(); ++it) {   // oldest first, the newest on top
        const double minutes = (double)(nowS - it->utcNs / 1000000000LL) / 60.0;
        if (minutes > S.keepMin) continue;
        const ImVec2 q = adsbmap::project(it->lat, it->lon);
        ellipse(dl, *it, ageColour(minutes, 110));
        dl->AddLine(ImVec2(q.x - 4 * gUi, q.y), ImVec2(q.x + 4 * gUi, q.y), ageColour(minutes, 255), 1.6f * gUi);
        dl->AddLine(ImVec2(q.x, q.y - 4 * gUi), ImVec2(q.x, q.y + 4 * gUi), ageColour(minutes, 255), 1.6f * gUi);
        drawn++;
    }
    for (const dect2::LtStationRow& r : t.stations) {
        const ImVec2 q = adsbmap::project(r.lat, r.lon);
        const ImU32 c = r.fine ? IM_COL32(110, 210, 230, 255) : IM_COL32(220, 170, 90, 255);
        dl->AddCircleFilled(q, 4 * gUi, c);
        dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), c, r.name.c_str());
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##ltz", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##ltz", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never your location or the strikes");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(size.x, "%d strike%s: white < 1 min, yellow < 5, orange < 15, red < 30; ellipse = 95 %%", drawn, drawn == 1 ? "" : "s");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void strikesList(App& a) {
    const dect2::LtPosTelemetry& t = a.rx.ltpos;
    if (!ImGui::BeginTable("##ltsk", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Lat")); ImGui::TableSetupColumn(tr("Lon")); ImGui::TableSetupColumn(tr("Error (km)")); ImGui::TableSetupColumn(tr("Stations"));
    ImGui::TableSetupColumn(tr("Residual"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const dect2::LtStrikeRow& s : t.strikes) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(s.utcNs / 1000000000LL).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.3f", s.lat);
        ImGui::TableNextColumn(); ImGui::Text("%.3f", s.lon);
        ImGui::TableNextColumn(); ImGui::Text("%.2f x %.2f", (double)s.majorKm, (double)s.minorKm);
        ImGui::TableNextColumn(); ImGui::Text("%d%s%s", s.stations, s.unchecked ? " (unchecked)" : "", s.ambiguous ? " (ambiguous)" : "");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", usText(s.rmsUs).c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (t.strikes.empty()) ImGui::TextDisabled("%s", t.role == 2 ? tr("no strikes located yet") : tr("strikes are located on the server"));
}

void intro() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Several OnAir stations with radios time the same lightning crackle; one of them, the server, finds the strike from the differences in arrival time. "
                                 "It needs timing to a microsecond at each station: a GPS-disciplined radio, or a VLF / LF time reference all stations can receive. Tune the band with the reference in it (around 20 to 60 kHz) and pick a role under the decoder options. Receive only."));
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    load();
    kit::modeHeader({790, "Lightning position", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    intro();
    ImGui::Spacing();
    if (!live(a)) { notRunning(a); return; }
    modepanel::panes({{"##ltclk", tr("This station"), [&a] { clockPane(a); }, 1.f, 280.f},
                      {"##ltmap", tr("Map"), [&a] { mapPane(a); }, 2.f, 340.f}}, 340.f);
}

void command(App& a) {
    load();
    kit::modeHeader({790, "Lightning position", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    if (!live(a)) { intro(); notRunning(a); return; }
    modepanel::panes({{"##ltmap2", tr("Map"), [&a] { mapPane(a); }, 2.f, 340.f},
                      {"##ltclk2", tr("Timing"), [&a] { clockPane(a); }, 1.f, 260.f},
                      {"##ltst2", tr("Stations"), [&a] { stationsPane(a); }, 1.f, 260.f}}, 300.f);
}

void receiver(App& a) {
    if (!live(a)) { notRunning(a); return; }
    const dect2::LtPosTelemetry& t = a.rx.ltpos;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(170 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    kv(tr("noise floor"), "%.1f dBFS", (double)t.floorDb);
    kv(tr("threshold"), "%.0f dB over the floor", (double)t.thresholdDb);
    kv(tr("sferics timed"), "%llu", (unsigned long long)t.pulses);
    kv(tr("detections sent"), "%llu (%llu errors)", (unsigned long long)t.sent, (unsigned long long)t.sendErrors);
    kv(tr("packets accepted"), "%llu", (unsigned long long)t.packetsOk);
    kv(tr("refused: authentication"), "%llu", (unsigned long long)t.packetsAuth);
    kv(tr("refused: replay"), "%llu", (unsigned long long)t.packetsReplay);
    kv(tr("refused: malformed"), "%llu", (unsigned long long)t.packetsBad);
    kv(tr("groups tried"), "%llu", (unsigned long long)t.groups);
    kv(tr("located"), "%llu", (unsigned long long)t.fixes);
    kv(tr("rejected (false matches)"), "%llu", (unsigned long long)t.rejected);
    kv(tr("unmatched"), "%llu", (unsigned long long)t.unmatched);
    kv(tr("too coarse to locate"), "%llu", (unsigned long long)t.coarseOnly);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    strikesList(a);
}

void list(App& a) { if (live(a)) stationsPane(a); }

void decoder(App& a, bool&) {
    load();
    bool ch = false;
    static const char* roles[] = {"This station only", "Client (send to a server)", "Server (collect and locate)"};
    ImGui::TextDisabled("%s", tr("ROLE")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##ltrole", tr(roles[std::clamp(S.role, 0, 2)]))) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(roles[i]), S.role == i)) { S.role = i; ch = true; }
        ImGui::EndCombo();
    }
    if (S.role != 0) {
        if (S.role == 1) { ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x)); ch |= ImGui::InputTextWithHint("##lthost", tr("server address"), S.host, sizeof S.host); }
        ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
        ch |= ImGui::InputTextWithHint("##ltkey", tr("network key, 12 characters or more"), S.key, sizeof S.key, ImGuiInputTextFlags_Password);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The same key on every station: packets are signed with it and anything else is dropped. Nothing is sent anywhere but to the server you name."));
        ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
        ch |= ImGui::InputTextWithHint("##ltname", tr("station name"), S.name, sizeof S.name);
        if (!receiverLocationSet()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Set this station's position in View > Map: nothing is sent without it.")); ImGui::PopTextWrapPos(); }
    }
    ImGui::TextDisabled("%s", tr("THRESHOLD")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::SliderFloat("##ltthr", &S.thrDb, 6.f, 30.f, "%.0f dB");
    ImGui::Checkbox(tr("Advanced##lt"), &S.advanced);
    if (S.advanced) {
        ImGui::TextDisabled("%s", tr("REFERENCE"));
        const RefTx* cur = S.refSel > 0 ? refById(S.refSel) : nullptr;
        ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##ltref", S.refSel == 0 ? tr("Automatic (nearest in the capture)") : S.refSel < 0 ? tr("None") : (cur ? cur->name.c_str() : "?"))) {
            if (ImGui::Selectable(tr("Automatic (nearest in the capture)"), S.refSel == 0)) { S.refSel = 0; ch = true; }
            if (ImGui::Selectable(tr("None"), S.refSel < 0)) { S.refSel = -1; ch = true; }
            for (const RefTx& r : builtinRefs()) if (ImGui::Selectable(r.name.c_str(), S.refSel == r.id)) { S.refSel = r.id; ch = true; }
            ImGui::EndCombo();
        }
        static const char* kinds[] = {"Computer clock: not synchronised", "Computer clock: NTP", "Computer clock: PTP"};
        ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##ltsys", tr(kinds[std::clamp(S.sysKind, 0, 2)]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kinds[i]), S.sysKind == i)) { S.sysKind = i; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Used to pair detections and to count symbol periods of the reference, never for the position: NTP is good to milliseconds, and a millisecond is 300 km."));
        ImGui::SetNextItemWidth(100 * gUi);
        ch |= ImGui::InputFloat(tr("clock error, us (0: typical)##lt"), &S.sysSigmaUs, 0, 0, "%.0f");
        ch |= ImGui::Checkbox(tr("Use the radio's GPS time when it has it##lt"), &S.useHw);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A radio with a GPSDO or a PPS input: pick that time source in the radio's settings; the samples then carry hardware time."));
        if (S.role == 2) {
            ch |= ImGui::Checkbox(tr("The server's radio is a station too##lt"), &S.serverIsStation);
            ImGui::SetNextItemWidth(100 * gUi);
            ch |= ImGui::SliderInt(tr("stations for a fix##lt"), &S.minStations, 3, 8);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Three stations give a position with nothing left to check it; four or more can reject a wrong match."));
            ImGui::SetNextItemWidth(100 * gUi);
            ch |= ImGui::InputFloat(tr("ground-wave speed / c##lt"), &S.velocity, 0, 0, "%.4f");
            ch |= ImGui::Checkbox(tr("Ionosphere hops on long paths##lt"), &S.hops);
            if (S.hops) { ImGui::SetNextItemWidth(100 * gUi); ch |= ImGui::SliderFloat(tr("reflection height##lt"), &S.hopKm, 60.f, 100.f, "%.0f km"); }
        }
        ImGui::SetNextItemWidth(100 * gUi);
        ch |= ImGui::InputInt(tr("UDP port##lt"), &S.port, 0, 0);
        ImGui::SetNextItemWidth(100 * gUi);
        ch |= ImGui::SliderFloat(tr("keep on the map##lt"), &S.keepMin, 5.f, 240.f, "%.0f min");
    }
    if (ImGui::SmallButton(tr("Clear strikes##lt"))) a.engine.ltpos().clearStrikes();
    if (ch) save();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    bool ref = sc.modeOpt[0] == 1;
    if (ImGui::Checkbox(tr("With an MSK reference##lt"), &ref)) { sc.modeOpt[0] = ref ? 1 : 0; changed = true; }
    float r = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 2.f;
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##ltrate", &r, 0.1f, 20.f, "%.1f / s", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = r; changed = true; }
}

void tick(App& a) {
    load();
    double lat, lon;
    receiverLocation(lat, lon);
    dect2::LtPosSettings s;
    s.role = S.role; s.host = S.host; s.port = S.port; s.key = S.key; s.name = S.name;
    s.lat = lat; s.lon = lon; s.altM = receiverAltitudeM(); s.posSet = receiverLocationSet();
    s.serverIsStation = S.serverIsStation; s.sysKind = S.sysKind; s.sysSigmaUs = S.sysSigmaUs; s.useHwTime = S.useHw;
    s.refId = chosenRef(a); s.thresholdDb = S.thrDb; s.velocityFactor = S.velocity; s.hops = S.hops; s.hopHeightKm = S.hopKm; s.minStations = S.minStations;
    char sig[512];
    snprintf(sig, sizeof sig, "%d|%s|%d|%s|%s|%.7f|%.7f|%.0f|%d|%d|%d|%.0f|%d|%d|%.1f|%.5f|%d|%.0f|%d", s.role, s.host.c_str(), s.port, s.key.c_str(), s.name.c_str(), s.lat, s.lon, s.altM, s.posSet, s.serverIsStation, s.sysKind, s.sysSigmaUs, s.useHwTime, s.refId, s.thresholdDb, s.velocityFactor, s.hops, s.hopHeightKm, s.minStations);
    const bool run = a.engine.running();
    if (run && !S.wasRunning) S.applied.clear();
    S.wasRunning = run;
    if (S.applied != sig) { S.applied = sig; a.engine.ltpos().apply(s); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    lamp("IQ", run ? 1 : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Clock"), !on ? 0 : a.rx.ltpos.fine ? 1 : 2); flowNext(10 * gUi);
    lamp(tr("Network"), !on || a.rx.ltpos.role == 0 ? 0 : a.rx.ltpos.state == 3 ? 1 : 2); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::ltposSummary(a.rx.ltpos).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Lightning position";
    if (live(a)) l2 = dect2::ltposSummary(a.rx.ltpos);
}

} // namespace

extern const ModeUi kLtposUi;
const ModeUi kLtposUi = {
    .sideTitle = "STATIONS",
    .tabName = "Lightning map",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .command = command,
};
