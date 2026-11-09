// ADS-L screens: one aircraft table and one map for ADS-L and for the FLARM aircraft heard in the same capture (the engine feeds both receivers in this mode, docs/ADSL-3.8.md).
// One click: the mode tunes 868.875 MHz at 2 Msps and needs no settings. The receiver position only adds distance and bearing and lives under Advanced.
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/adsl/adsl_proto.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/adsl/adsl_tel.h"
#include "dect2/flarm/flarm_proto.h"
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
    double lat = 0, lon = 0;
    bool haveRef = false, flarmRefSet = false;
    double pushedLat = 1e9, pushedLon = 1e9;
    uint64_t sel = ~0ull;
    int view = 0;
    adsbmap::View map;
    char posText[64] = "";
    std::string msg;
};
State S;

constexpr int kStd = 301;
bool live(const App& a) { return a.rx.standard == kStd; }

// A row of either source: ADS-L or the FLARM receiver fed from the same capture
struct Row {
    uint64_t key = 0;
    bool flarm = false;
    std::string id, src, cat;
    bool hasPos = false; double lat = 0, lon = 0;
    bool hasAlt = false; int altM = 0;
    bool hasSpeed = false; float speedKmh = 0;
    bool hasTrack = false; float trackDeg = 0;
    bool hasClimb = false; float climbMs = 0;
    float levelDb = 0, ageSec = 0;
    bool hasRange = false; float distKm = 0, bearingDeg = 0;
    uint32_t packets = 0;
    const std::vector<dect2::AdslTrackPoint>* adslTrack = nullptr;
    const std::vector<dect2::FlarmTrackPoint>* flarmTrack = nullptr;
};

dect2::FlarmTelemetry gFlarm;      // refreshed once a frame from the receiver running beside ADS-L

std::string hex6(uint32_t i) { char b[12]; snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }

std::vector<Row> rows(App& a) {
    std::vector<Row> v;
    if (!live(a)) return v;
    for (const dect2::AdslAircraft& ac : a.rx.adsl.aircraft) {
        Row r;
        r.key = (uint64_t)ac.amt << 24 | ac.id; r.id = hex6(ac.id);
        r.src = std::string("ADS-L ") + dect2::adsl::amtName(ac.amt); if (ac.viaUplink) r.src += " (uplink)";
        r.cat = dect2::adsl::categoryName(ac.category);
        r.hasPos = ac.hasPos; r.lat = ac.lat; r.lon = ac.lon;
        r.hasAlt = ac.hasAlt; r.altM = ac.altM; r.hasSpeed = ac.hasSpeed; r.speedKmh = ac.speedKmh;
        r.hasTrack = ac.hasTrack; r.trackDeg = ac.trackDeg; r.hasClimb = ac.hasClimb; r.climbMs = ac.climbMs;
        r.levelDb = ac.levelDb; r.ageSec = ac.ageSec; r.hasRange = ac.hasRange; r.distKm = ac.distKm; r.bearingDeg = ac.bearingDeg; r.packets = ac.packets;
        r.adslTrack = &ac.track;
        v.push_back(std::move(r));
    }
    for (const dect2::FlarmAircraft& ac : gFlarm.aircraft) {
        Row r;
        r.key = 1ull << 40 | ac.id; r.flarm = true; r.id = hex6(ac.id); r.src = "FLARM"; r.cat = dect2::flarm::aircraftTypeName(ac.acType);
        r.hasPos = ac.hasPos; r.lat = ac.lat; r.lon = ac.lon; r.hasAlt = true; r.altM = ac.altM; r.hasSpeed = true; r.speedKmh = ac.speedKmh;
        r.hasTrack = true; r.trackDeg = ac.trackDeg; r.hasClimb = true; r.climbMs = ac.climbMs;
        r.levelDb = ac.levelDb; r.ageSec = ac.ageSec; r.hasRange = ac.hasRange; r.distKm = ac.distKm; r.bearingDeg = ac.bearingDeg; r.packets = ac.packets;
        r.flarmTrack = &ac.track;
        v.push_back(std::move(r));
    }
    return v;
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("adsbLat") && d.has("adsbLon")) { S.lat = d.getD("adsbLat", 0); S.lon = d.getD("adsbLon", 0); S.haveRef = true; }
    S.map.zoom = (int)d.getI("adslZoom", 9);
    S.map.lat = S.lat; S.map.lon = S.lon;
    snprintf(S.posText, sizeof S.posText, "%.5f, %.5f", S.lat, S.lon);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", tr(k)); kvColumn(150 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

const char* dir16(float deg) {
    static const char* n[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    return n[((int)std::lround(deg / 22.5f) % 16 + 16) % 16];
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
    ImGui::TextDisabled("%s", tr("receiver position"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(200 * gUi, ImGui::GetContentRegionAvail().x));
    const bool enter = ImGui::InputText("##adslpos", S.posText, sizeof S.posText, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 5 * gUi);
    if (ImGui::Button(trw("Set")) || enter) {
        double la, lo;
        if (parseCoordinates(S.posText, la, lo)) { setPosition(a, la, lo); S.msg = tr("position set"); }
        else if (fromMaidenhead(S.posText, la, lo)) { setPosition(a, la, lo); S.msg = tr("position set from the locator"); }
        else S.msg = tr("not a position: try 54.687, 25.280 or a locator such as KO24");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Optional, for distance and bearing. ADS-L positions are complete without it (FLARM needs it, or takes the position of the first ADS-L aircraft)."));
    if (!S.msg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.msg.c_str()); }
}

void tableView(App&, const std::vector<Row>& v) {
    if (ImGui::BeginTable("##adlt", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("ID")); ImGui::TableSetupColumn(tr("Source")); ImGui::TableSetupColumn(tr("Category"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Altitude")); ImGui::TableSetupColumn(tr("Speed")); ImGui::TableSetupColumn(tr("Track")); ImGui::TableSetupColumn(tr("Climb"));
        ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Packets"));
        ImGui::TableHeadersRow();
        for (const Row& r : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((void*)r.key);
            if (ImGui::Selectable(r.id.c_str(), S.sel == r.key, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == r.key ? ~0ull : r.key;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.src.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.cat.c_str());
            ImGui::TableNextColumn(); if (r.hasAlt) ImGui::Text("%d m", r.altM); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.hasSpeed) ImGui::Text("%.0f km/h", r.speedKmh); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.hasTrack) ImGui::Text("%.0f %s", r.trackDeg, dir16(r.trackDeg)); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.hasClimb) ImGui::Text("%+.1f m/s", r.climbMs); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", r.ageSec);
            ImGui::TableNextColumn(); if (r.hasRange) ImGui::Text("%.1f km %s", r.distKm, dir16(r.bearingDeg)); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", r.packets);
        }
        ImGui::EndTable();
    }
}

void mapView(App&, const std::vector<Row>& v) {
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
    for (const Row& ac : v) {
        if (!ac.hasPos) continue;
        const ImVec2 q = adsbmap::project(ac.lat, ac.lon);
        const bool sel = S.sel == ac.key;
        const ImU32 col = adsbmap::acColour(ac.flarm ? adsbmap::AcSrc::Flarm : adsbmap::AcSrc::Adsl, ac.ageSec);
        auto drawPath = [&](auto const& t) {
            if (t.size() < 2) return;
            ImVec2 prev = adsbmap::project(t[0].lat, t[0].lon);
            for (size_t i = 1; i < t.size(); i++) { const ImVec2 p = adsbmap::project(t[i].lat, t[i].lon); dl->AddLine(prev, p, (col & 0x00FFFFFF) | 0x90000000, 1.5f); prev = p; }
        };
        if (ac.adslTrack) drawPath(*ac.adslTrack); else if (ac.flarmTrack) drawPath(*ac.flarmTrack);
        const float r = (sel ? 13.f : 11.f) * gUi, ang = ac.trackDeg * 3.14159265f / 180.f, ch = std::cos(ang), sh = std::sin(ang);
        auto gv = [&](float x, float y) { return ImVec2(q.x + (x * ch - y * sh) * r, q.y + (x * sh + y * ch) * r); };
        ImVec2 g[12] = { gv(0, -1), gv(0.08f, -0.2f), gv(1.15f, 0.05f), gv(1.15f, 0.18f), gv(0.08f, 0.08f), gv(0.07f, 0.55f),
                         gv(-0.07f, 0.55f), gv(-0.08f, 0.08f), gv(-1.15f, 0.18f), gv(-1.15f, 0.05f), gv(-0.08f, -0.2f), gv(0, -1) };
        dl->AddConcavePolyFilled(g, 12, col);
        dl->AddPolyline(g, 12, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, sel ? 1.6f : 1.f);
        char b[96]; snprintf(b, sizeof b, "%s  %s  %d m", ac.id.c_str(), ac.cat.c_str(), ac.altM);
        dl->AddText(ImVec2(q.x + 9 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), b);
        if (clicked) { const float dd = std::hypot(click.x - q.x, click.y - q.y); if (dd < bestD) { bestD = dd; S.sel = ac.key; } }
    }
    dl->PopClipRect();
    adsbmap::legend(size.x, "%zu aircraft", v.size());
}

void note() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only. Positions are what the aircraft broadcast. Packets from a random privacy address are shown here but never shared to the public map."));
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    loadState();
    subNav("adv", S.view, {tr("Map"), tr("Table")});
    if (S.view == 0) { ImGui::SameLine(0, 12 * gUi); adsbmap::tileControls("never the position itself"); }
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); note(); return; }
    const std::vector<Row> v = rows(a);
    if (v.empty()) ImGui::TextDisabled("%s", tr(a.rx.adsl.state >= 1 ? "energy on the channels, no decodable packet yet" : "listening on 868.2, 868.4 and 869.525 MHz"));
    if (S.view == 0) mapView(a, v); else tableView(a, v);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const std::vector<Row> v = rows(a);
    if (ImGui::BeginTable("##adll", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(tr("ID")); ImGui::TableSetupColumn(tr("Category"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Alt"));
        for (const Row& r : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, r.ageSec < 10 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::PushID((void*)r.key);
            if (ImGui::Selectable(r.id.c_str(), S.sel == r.key, ImGuiSelectableFlags_SpanAllColumns)) S.sel = r.key;
            ImGui::PopID();
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.cat.c_str());
            ImGui::TableNextColumn(); if (r.hasAlt) ImGui::Text("%d", r.altM); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    if (v.empty()) ImGui::TextDisabled("%s", tr("no aircraft heard yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const dect2::AdslTelemetry& t = a.rx.adsl;
    kvRow(a, N_("state"), "%s", dect2::adslSummary(t).c_str());
    kvRow(a, N_("packets"), "%llu good, %llu bad, %llu repaired, %.1f per second", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.corrected, t.packetsPerSec);
    kvRow(a, N_("channels"), "868.2 MHz %llu%s, 868.4 MHz %llu%s, 869.525 LDR %llu%s, 869.525 HDR %llu%s",
          (unsigned long long)t.perBand[0], t.bandOn[0] ? "" : " (off)", (unsigned long long)t.perBand[1], t.bandOn[1] ? "" : " (off)", (unsigned long long)t.perBand[2], t.bandOn[2] ? "" : " (off)", (unsigned long long)t.perBand[3], t.bandOn[3] ? "" : " (off)");
    kvRow(a, N_("payloads"), "traffic %llu, status %llu, uplink %llu, FIS-B %llu, Remote ID %llu, other %llu", (unsigned long long)t.traffic, (unsigned long long)t.status, (unsigned long long)t.uplink, (unsigned long long)t.fisB, (unsigned long long)t.remoteId, (unsigned long long)t.other);
    kvRow(a, N_("reserved key"), "%llu counted, no key is published (signed packets: %llu, not verified)", (unsigned long long)t.encrypted, (unsigned long long)t.signedPackets);
    kvRow(a, N_("FLARM beside it"), "%u aircraft", gFlarm.aircraftCount);
    kvRow(a, N_("signal"), "%.1f dB over the noise, noise %.1f dB", t.snrDb, t.noiseDb);
    kvRow(a, N_("farthest"), "%.1f km", t.maxRangeKm);
    kvRow(a, N_("input"), "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    note();
}

void decoder(App& a, bool&) {
    loadState();
    if (ImGui::SmallButton(trw("Clear"))) { a.engine.adsl().clearResults(); a.engine.flarm().clearResults(); S.sel = ~0ull; }
    if (ImGui::CollapsingHeader(trw("Advanced"))) positionControls(a);
}

void synth(App& a, bool& changed) {
    loadState();
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("%s", tr("simulated aircraft around the receiver position, four made-up devices on every channel"));
    if (S.haveRef && (sc.modeVal[0] != S.lat || sc.modeVal[1] != S.lon)) { sc.modeVal[0] = S.lat; sc.modeVal[1] = S.lon; changed = true; }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Packets", !on ? 0 : a.rx.adsl.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? tr(run ? "starting" : "stopped") : dect2::adslSummary(a.rx.adsl).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ADS-L";
    if (live(a)) l2 = dect2::adslSummary(a.rx.adsl);
}

void tick(App& a) {
    loadState();
    if (S.pushedLat != S.lat || S.pushedLon != S.lon) {
        S.pushedLat = S.lat; S.pushedLon = S.lon;
        if (S.haveRef) { a.engine.adsl().setReference(S.lat, S.lon); a.engine.flarm().setReference(S.lat, S.lon); S.flarmRefSet = true; }
    }
    if (!live(a)) return;
    a.engine.flarm().telemetry(gFlarm, 0);
    if (!S.flarmRefSet) for (const dect2::AdslAircraft& ac : a.rx.adsl.aircraft) if (ac.hasPos) {     // ADS-L positions are absolute: the first one anchors FLARM's relative ones
        a.engine.flarm().setReference(ac.lat, ac.lon);
        S.flarmRefSet = true;
        break;
    }
}

} // namespace

extern const ModeUi kAdslUi;
const ModeUi kAdslUi = {
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
