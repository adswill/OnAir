// HFDL screens: the message log with a filter and a detail pane (ACARS text), a map of the ground stations and the aircraft positions,
// the aircraft, the ground stations with their status, the channel list with its activity, and the range plot (furthest position report
// per bearing from the receiver). Band presets in the tuner.
#include "dect2/common/time_fmt.h"
#include "../adsb/adsb_map.h"
#include "app.h"
#include "map/pos_predict.h"
#include "map/derived_kin.h"
#include "state/results_export.h"
#include "map/rangeplot_ui.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/hfdl/hfdl_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

struct State {
    bool loaded = false;
    int viewMode = 0;                    // 0 messages, 1 map, 2 aircraft, 3 ground stations, 4 channels, 5 range
    char fAir[24] = "";
    char fText[32] = "";
    int fKind = 0;                       // 0 all, 1 ACARS, 2 squitters, 3 positions, 4 logon / link
    uint64_t selected = 0;
    char chans[200] = "";                // channel list in kHz, empty = every table frequency in the band
    char pushedChans[200] = "\x01";
    adsbmap::View map;
    bool mapFollow = true;
    uint32_t selAir = 0;
    RangeView range;                     // the receiver-range plot (Range view)
    std::unordered_map<uint32_t, double> rangeFedT;   // per aircraft: the position report already counted in it
};
State S;
dect2::BasebandFilterCfg gBbfPushed;
bool gBbfInit = false;

// the HFDL bands: where several ground stations share a few hundred kHz (centre MHz)
struct Band { const char* name; double mhz; };
const Band kBands[] = {{"5.5 MHz (5.45 - 5.72)", 5.58}, {"6.6 MHz (6.53 - 6.71)", 6.62}, {"8.9 MHz (8.82 - 8.98)", 8.90}, {"10.06 MHz (10.03 - 10.09)", 10.06},
                       {"11.3 MHz (11.18 - 11.39)", 11.29}, {"13.3 MHz (13.26 - 13.36)", 13.31}, {"17.94 MHz (17.90 - 17.99)", 17.945}, {"21.96 MHz (21.93 - 22.00)", 21.963}};

bool live(const App& a) { return a.rx.standard == 36; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    snprintf(S.chans, sizeof S.chans, "%s", d.getS("hfdlChans", "").c_str());
    S.map.zoom = (int)d.getI("hfdlZoom", 2);
    S.map.lat = 40; S.map.lon = -30;
    S.range.plot.configure(5.0, 0.0, 20040.0);   // HF reaches anywhere: the sanity limit is half way round the Earth, not the VHF 1000 km
    if (const char* e = getenv("DECT2_HFDL_VIEW")) S.viewMode = std::max(0, std::min(5, atoi(e)));   // dev: open on a view (screenshots)
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool contains(const std::string& hay, const char* needle) { return !*needle || lower(hay).find(lower(needle)) != std::string::npos; }
std::string hex6(uint32_t a) { char b[8]; snprintf(b, sizeof b, "%06X", a & 0xFFFFFF); return b; }

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

std::string ago(double now, double then) {
    if (then <= 0) return "-";
    const double s = std::max(0.0, now - then);
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s); else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60); else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

std::string gsLabel(int id) { const dect2::HfdlStationInfo* s = dect2::hfdlStation(id); return std::to_string(id) + (s ? std::string(" ") + s->name : ""); }
std::string airName(const dect2::HfdlAircraft& c) {
    std::string s = c.icao ? hex6(c.icao) : "id " + std::to_string(c.acId) + "@" + std::to_string(c.gsId);
    if (!c.reg.empty()) s += " " + c.reg;
    if (!c.flight.empty()) s += " " + c.flight;
    return s;
}
// ranges in the ADS-B screen's unit (aviation: nm, metric: km), from the receiver position
bool metric() { return plat::prefs().getI("adsbUnits", 0) == 1; }
double unitPerKm() { return metric() ? 1.0 : 0.5399568; }
const char* distUnit() { return metric() ? "km" : "nm"; }
bool rangeOf(double lat, double lon, double& km, double& brg) {
    if (!receiverLocationSet()) return false;
    double rl, ro; receiverLocation(rl, ro);
    dect2::RangePlot::bearingDistance(rl, ro, lat, lon, brg, km);
    return std::isfinite(km);
}
std::string rangeText(const dect2::HfdlAircraft& c, bool bearing = true) {
    double km, brg;
    if (!c.hasPos || !rangeOf(c.lat, c.lon, km, brg)) return "-";
    char b[48];
    if (bearing) snprintf(b, sizeof b, "%.0f %s at %.0f deg", km * unitPerKm(), distUnit(), brg); else snprintf(b, sizeof b, "%.0f %s", km * unitPerKm(), distUnit());
    return b;
}

const dect2::HfdlAircraft* findAir(const dect2::HfdlTelemetry& t, uint32_t key) { for (const auto& c : t.aircraft) if (c.key == key) return &c; return nullptr; }

std::string khzList(const std::vector<int>& k) {
    std::string s;
    for (int v : k) if (v > 0) { if (!s.empty()) s += " "; s += std::to_string(v); }
    return s.empty() ? "-" : s;
}

// ---- position estimates (core/src/track): HF position reports come every few minutes, so the aircraft are dead-reckoned between them
pospred::Layer gPred;

// speed, track and vertical rate from the filter, else from the last two positions (~): HFDL reports carry none (the track points have no altitude: no vertical rate)
derivedkin::Info kinOf(const dect2::HfdlAircraft& c, double now) {
    return derivedkin::compute(c.track, dect2::geo::KinMeasured(), derivedkin::estimated(gPred.hub, gPred.predict, dect2::track::keyFromIcao(c.icao ? c.icao : c.key), now, c.track.size()));
}
std::unordered_map<uint32_t, double> gFedPosT;

void feedEstimates(const dect2::HfdlTelemetry& t) {
    gPred.load("hfdl", true, true);
    if (!gPred.predict) return;
    for (const auto& c : t.aircraft) {
        const uint32_t key = c.icao ? c.icao : c.key;
        if (c.gsId) if (const auto* s = dect2::hfdlStation(c.gsId)) { gPred.hub.setStation((uint32_t)c.gsId, s->lat, s->lon, 1500); gPred.hub.noteStation(dect2::track::keyFromIcao(key), (uint32_t)c.gsId, c.lastHeardSec); }
        if (!c.hasPos) continue;
        double& last = gFedPosT[key];
        if (last == c.posTime) continue;
        last = c.posTime;
        dect2::track::Fix f;
        if (dect2::track::fixFromHfdl(c, f)) gPred.hub.feed(dect2::track::keyFromIcao(key), f);
    }
    gPred.hub.purge(t.nowSec, 6 * 3600);
}

// each new position report once into the range plot (the telemetry repeats the last one until the next arrives)
void feedRange(const dect2::HfdlTelemetry& t) {
    if (!receiverLocationSet()) return;
    double rl, ro; receiverLocation(rl, ro);
    if (std::fabs(rl - S.range.refLat) > 1e-4 || std::fabs(ro - S.range.refLon) > 1e-4) S.rangeFedT.clear();
    rangeSetStation(S.range, rl, ro);
    for (const auto& c : t.aircraft) {
        if (!c.hasPos) continue;
        double& last = S.rangeFedT[c.key];
        if (last == c.posTime) continue;
        last = c.posTime;
        rangeFeed(S.range, c.lat, c.lon);
    }
}

void tick(App& a) {
    feedEstimates(a.rx.hfdl);
    loadState();
    if (live(a)) feedRange(a.rx.hfdl);
    if (!a.engine.running()) { S.pushedChans[0] = '\x01'; S.pushedChans[1] = 0; return; }
    a.engine.hfdl().setWholeSpan(a.family == 35 && a.wideDecode);   // the Whole span switch (top right of the spectrum)
    if (std::strcmp(S.pushedChans, S.chans) != 0) {
        std::vector<double> hz;
        const char* p = S.chans;
        while (*p) {
            char* e = nullptr;
            const double khz = std::strtod(p, &e);
            if (e == p) { p++; continue; }
            if (khz >= 2000 && khz <= 30000) hz.push_back(std::round(khz) * 1e3);
            p = e;
        }
        a.engine.hfdl().setChannels(hz);
        std::snprintf(S.pushedChans, sizeof S.pushedChans, "%s", S.chans);
    }
    { const dect2::BasebandFilterCfg c = dect2::bbf::get("hfdl"); if (!gBbfInit || c != gBbfPushed) { a.engine.hfdl().setBasebandFilter(c); gBbfPushed = c; gBbfInit = true; } }
}

// ---------------------------------------------------------------- messages

bool passes(const dect2::HfdlMessage& m) {
    if (S.fKind == 1 && !m.acars) return false;
    if (S.fKind == 2 && !m.squitter) return false;
    if (S.fKind == 3 && !m.hasPos) return false;
    if (S.fKind == 4 && (m.squitter || m.acars || !m.hfnpdu.empty())) return false;
    if (S.fAir[0] && !contains((m.icao ? hex6(m.icao) : "") + " " + m.reg + " " + m.flight + " " + gsLabel(m.gsId), S.fAir)) return false;
    if (S.fText[0] && !contains(m.summary, S.fText) && !contains(m.detail, S.fText)) return false;
    return true;
}

void filterRow() {
    ImGui::TextDisabled("Aircraft / station"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputTextWithHint("##hffa", "ICAO, reg, flight, GS", S.fAir, sizeof S.fAir);
    flowNext(12 * gUi);
    ImGui::TextDisabled("Text"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputText("##hfft", S.fText, sizeof S.fText);
    flowNext(12 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    static const char* kinds[] = {"all", "ACARS", "squitters", "positions", "logon / link"};
    ImGui::Combo("##hffk", &S.fKind, kinds, 5);
    flowEnd();
}

void detailPane(const App& a, const dect2::HfdlMessage* m) {
    if (!m) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Click a message to see every layer of it."); ImGui::PopTextWrapPos(); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(100 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("time", "%s%s  %.3f MHz  %d bit/s (%d slot%s)  %.0f dBFS  SNR %.0f dB  %+.0f Hz", clock(m->wallTime).c_str(), dect2::timefmt::sfx(), m->freqHz / 1e6, m->bps, m->slots, m->slots == 1 ? "" : "s", m->levelDb, m->snrDb, m->cfoHz);
    kv("station", "%s, %s", gsLabel(m->gsId).c_str(), m->uplink ? "uplink (ground to air)" : "downlink (air to ground)");
    if (m->icao || m->acId >= 0) kv("aircraft", "%s%s", m->icao ? hex6(m->icao).c_str() : "-", m->acId >= 0 ? (", id " + std::to_string(m->acId)).c_str() : "");
    ImGui::Spacing();
    ImGui::TextDisabled("Decoded");
    ImGui::BeginChild("##hfdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(m->detail.c_str());
    if (m->acars && !m->acarsText.empty()) { ImGui::Spacing(); ImGui::TextDisabled("ACARS text"); ImGui::TextUnformatted(m->acarsText.c_str()); }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
}

void messageView(App& a) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    const bool on = live(a);
    filterRow();
    const float H = ImGui::GetContentRegionAvail().y;
    const float listH = std::max(120.f * gUi, H * 0.55f);
    const dect2::HfdlMessage* sel = nullptr;
    if (ImGui::BeginTable("##hfmsg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("bit/s"); ImGui::TableSetupColumn("GS");
        ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int shown = 0;
        if (on) {
            for (const auto& m : t.messages) {
                if (m.serial == S.selected) sel = &m;
                if (!passes(m)) continue;
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[24]; snprintf(id, sizeof id, "##h%llu", (unsigned long long)m.serial);
                if (ImGui::Selectable((clock(m.wallTime) + id).c_str(), S.selected == m.serial, ImGuiSelectableFlags_SpanAllColumns)) { S.selected = m.serial; sel = &m; }
                ImGui::TableNextColumn(); ImGui::Text("%.0f", m.freqHz / 1e3);
                ImGui::TableNextColumn(); ImGui::Text("%d", m.bps);
                ImGui::TableNextColumn(); ImGui::Text("%s %d", m.uplink ? "up" : "dn", m.gsId);
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, pal::accent()); ImGui::TextUnformatted(m.hfnpdu.empty() ? m.kind.c_str() : m.hfnpdu.c_str()); ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.summary, ImGui::GetContentRegionAvail().x).c_str());
            }
        }
        ImGui::EndTable();
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); }
        else if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.messages.empty() ? "nothing decoded yet: HFDL bursts come every few seconds when the band is open" : "no message matches the filter"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::BeginChild("##hfpane", ImVec2(0, 0));
    detailPane(a, on ? sel : nullptr);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- map

void mapCanvas(App& a, ImVec2 size) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        auto add = [&](double la, double lo) { la0 = std::min(la0, la); la1 = std::max(la1, la); lo0 = std::min(lo0, lo); lo1 = std::max(lo1, lo); n++; };
        for (const auto& c : t.aircraft) if (c.hasPos) add(c.lat, c.lon);
        for (const auto& g : t.stations) if (g.hasPos && g.heardDirect) add(g.lat, g.lon);
        if (n && receiverLocationSet()) { double rl, ro; receiverLocation(rl, ro); add(rl, ro); }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(5.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(1, std::min(9, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    float best = 18 * gUi; uint32_t hit = 0;
    int placed = 0;
    for (const auto& s : dect2::hfdlStations()) {   // every station of the table; the ones heard are bright
        const dect2::HfdlGroundStation* g = nullptr;
        if (on) for (const auto& x : t.stations) if (x.id == s.id) g = &x;
        const bool heard = g && g->heardDirect;
        const ImVec2 q = adsbmap::project(s.lat, s.lon);
        const ImU32 col = heard ? IM_COL32(120, 170, 230, 255) : g ? IM_COL32(100, 120, 150, 220) : IM_COL32(90, 96, 104, 180);
        dl->AddRectFilled(ImVec2(q.x - 5 * gUi, q.y - 5 * gUi), ImVec2(q.x + 5 * gUi, q.y + 5 * gUi), col);
        dl->AddRect(ImVec2(q.x - 5 * gUi, q.y - 5 * gUi), ImVec2(q.x + 5 * gUi, q.y + 5 * gUi), IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), heard ? IM_COL32(200, 220, 245, 255) : IM_COL32(140, 146, 152, 220), (std::to_string(s.id) + " " + s.name).c_str());
    }
    if (on) {
        for (const auto& c : t.aircraft) {
            const ImU32 col = adsbmap::acColour(adsbmap::AcSrc::Hfdl, t.nowSec - c.lastHeardSec);
            dect2::track::PosEstimate pe;
            const dect2::track::Key ekey = dect2::track::keyFromIcao(c.icao ? c.icao : c.key);
            if (!c.hasPos) {   // no position at all: the reach of the ground station it talks to
                if (gPred.predict && gPred.hub.estimate(ekey, t.nowSec, pe) && pe.coarse) pospred::drawCoarse(dl, pe, col);
                continue;
            }
            placed++;
            const bool est = gPred.get(ekey, t.nowSec, pe, t.nowSec - c.posTime) && pe.valid && !pe.coarse;
            for (size_t i = 1; i < c.track.size(); i++) dl->AddLine(adsbmap::project(c.track[i - 1].lat, c.track[i - 1].lon), adsbmap::project(c.track[i].lat, c.track[i].lon), (col & 0x00FFFFFF) | 0x90000000, 1.5f);
            const ImVec2 q = est ? adsbmap::project(pe.lat, pe.lon) : adsbmap::project(c.lat, c.lon);
            const bool sel = c.key == S.selAir;
            if (est) { pospred::drawPredictionMarks(dl, pe, q, col, gPred.ellipses); pospred::hoverTooltip(pe, q); }
            dl->AddCircleFilled(q, (sel ? 6.f : 5.f) * gUi, col);
            dl->AddCircle(q, (sel ? 6.f : 5.f) * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), 16, sel ? 1.6f : 1.f);
            if (const auto* s = dect2::hfdlStation(c.gsId)) dl->AddLine(q, adsbmap::project(s->lat, s->lon), IM_COL32(120, 170, 230, 60), 1.f);
            const std::string s = c.flight.empty() ? (c.reg.empty() ? airName(c) : c.reg) : c.flight;
            const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            const ImVec2 lp(q.x + 9 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), s.c_str());
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = c.key; }
        }
    }
    if (clicked) S.selAir = hit;
    double rl, ro; receiverLocation(rl, ro);
    const ImVec2 h = adsbmap::project(rl, ro);   // the receiver: a ringed dot, as on the main map
    if (const dect2::HfdlAircraft* sc = on ? findAir(t, S.selAir) : nullptr; sc && sc->hasPos && receiverLocationSet())
        dl->AddLine(h, adsbmap::project(sc->lat, sc->lon), IM_COL32(110, 210, 230, 110), 1.f * gUi);   // the range line to the picked aircraft
    dl->AddCircleFilled(h, 4 * gUi, IM_COL32(110, 210, 230, 255));
    dl->AddCircle(h, 8 * gUi, IM_COL32(110, 210, 230, 220), 24, 1.5f * gUi);
    dl->AddText(ImVec2(h.x + 10 * gUi, h.y - 7 * gUi), IM_COL32(110, 210, 230, 255), receiverLocationSet() ? "me" : "me (placeholder: set it in View > Map)");
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("me")) { S.map.lat = rl; S.map.lon = ro; S.mapFollow = false; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centre the map on my location");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    gPred.controls();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("hfdlZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!placed) adsbmap::legend(size.x, "no position report yet");
    else adsbmap::legend(size.x, "%d of %zu aircraft placed", placed, t.aircraft.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

void mapSide(App& a) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    const bool on = live(a);
    const dect2::HfdlAircraft* c = on ? findAir(t, S.selAir) : nullptr;
    if (!c) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Dots: aircraft positions from performance and frequency data and ACARS position reports. Squares: the ground stations (bright: heard).");
        ImGui::PopTextWrapPos();
        if (!on) return;
        int n = 0;
        for (const auto& x : t.aircraft) {
            if (!x.hasPos) continue;
            char id[16]; snprintf(id, sizeof id, "##hp%d", ++n);
            const std::string rg = rangeText(x, false);
            const std::string row = airName(x) + "  " + (rg == "-" ? "" : rg + "  ") + ago(t.nowSec, x.posTime) + " ago" + id;
            if (ImGui::Selectable(row.c_str(), false)) S.selAir = x.key;
        }
        return;
    }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(84 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    ImGui::TextUnformatted(airName(*c).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) S.selAir = 0;
    if (c->hasPos) kv("position", "%.4f %.4f", c->lat, c->lon);
    {
        derivedkin::Lines L;
        if (c->hasPos && derivedkin::lines(kinOf(*c, t.nowSec), L)) {
            kv("speed", "%s", L.gs.c_str()); kv("track", "%s", L.track.c_str());
            if (!L.note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", L.note.c_str()); ImGui::PopTextWrapPos(); }
        }
    }
    if (c->hasPos) kv("range", "%s%s", rangeText(*c).c_str(), receiverLocationSet() ? "" : " (set my location in View > Map)");
    if (!c->posKind.empty()) kv("source", "%s, %s ago", c->posKind.c_str(), ago(t.nowSec, c->posTime).c_str());
    kv("station", "%s%s", gsLabel(c->gsId).c_str(), c->loggedOn ? ", logged on" : "");
    kv("messages", "%u, last %s ago on %.0f kHz", c->messages, ago(t.nowSec, c->lastHeardSec).c_str(), c->freqHz / 1e3);
}

void mapView(App& a) {
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sideW = std::max(220.f * gUi, std::min(340.f * gUi, av.x * 0.3f));
    const ImVec2 size(std::max(200.f, av.x - sideW - 8 * gUi), std::max(160.f, av.y));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    mapCanvas(a, size);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x + 8 * gUi, p0.y));
    ImGui::BeginChild("##hfmapside", ImVec2(sideW, size.y));
    mapSide(a);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- tables

void aircraftView(const App& a) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    {
        static std::string note;
        resexp::Table x; x.cols = {"icao", "registration", "flight", "lat", "lon", "alt_ft", "gs_derived_kt", "track_derived_deg", "vrate_derived_fpm", "derived_dt_s"};
        if (live(a)) for (const auto& c : t.aircraft) {
            if (!c.hasPos) continue;
            const derivedkin::Info k = kinOf(c, t.nowSec);
            x.add({c.icao ? hex6(c.icao) : "", c.reg, c.flight, resexp::num(c.lat, 5), resexp::num(c.lon, 5), c.hasAlt ? std::to_string(c.altFt) : "",
                   k.d.ok ? resexp::num(k.d.gsKt, 0) : "", k.d.ok && k.d.hasTrack ? resexp::num(k.d.trackDeg, 0) : "", k.d.ok && k.d.hasVs ? resexp::num(k.d.vsFpm, 0) : "",
                   k.d.ok ? resexp::num(k.d.dtSec, 0) : ""});
        }
        resexp::saveButton(x, "hfdl_aircraft", note, "svhfair");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (!ImGui::BeginTable("##hfair", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Flight"); ImGui::TableSetupColumn("Station");
    ImGui::TableSetupColumn("Msgs"); ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Track");
    ImGui::TableSetupColumn("Range");
    ImGui::TableHeadersRow();
    if (live(a))
        for (const auto& c : t.aircraft) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.icao ? hex6(c.icao).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.reg.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.flight.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s", gsLabel(c.gsId).c_str(), c.loggedOn ? "" : " (not logged on)");
            ImGui::TableNextColumn(); ImGui::Text("%u", c.messages);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.freqHz / 1e3);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.nowSec, c.lastHeardSec).c_str());
            ImGui::TableNextColumn(); if (c.hasPos) ImGui::Text("%.3f %.3f", c.lat, c.lon); else ImGui::TextUnformatted("-");
            const derivedkin::Info k = c.hasPos ? kinOf(c, t.nowSec) : derivedkin::Info();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(derivedkin::gsText(k.v).c_str()); if (ImGui::IsItemHovered() && !derivedkin::explain(k).empty()) ImGui::SetTooltip("%s", derivedkin::explain(k).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(derivedkin::trackText(k.v).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(rangeText(c).c_str());
        }
    ImGui::EndTable();
}

void stationView(const App& a) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    if (!ImGui::BeginTable("##hfgs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Squitters"); ImGui::TableSetupColumn("Heard on");
    ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("Frame"); ImGui::TableSetupColumn("In use (kHz)", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& s : dect2::hfdlStations()) {
        const dect2::HfdlGroundStation* g = nullptr;
        if (live(a)) for (const auto& x : t.stations) if (x.id == s.id) g = &x;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d %s", s.id, s.name);
        ImGui::TableNextColumn();
        if (g && g->heardDirect) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.5f, 1)); ImGui::TextUnformatted(g->utcSync ? "heard, UTC" : "heard"); ImGui::PopStyleColor(); }
        else ImGui::TextDisabled("%s", g ? "named by another station" : "not heard");
        ImGui::TableNextColumn(); ImGui::Text("%u", g ? g->squitters : 0u);
        ImGui::TableNextColumn(); if (g && g->freqHz > 0) ImGui::Text("%.0f", g->freqHz / 1e3); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g ? ago(t.nowSec, g->lastHeardSec).c_str() : "-");
        ImGui::TableNextColumn(); if (g && g->frameIndex >= 0) ImGui::Text("%d", g->frameIndex); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g && !g->activeKhz.empty() ? khzList(g->activeKhz).c_str() : ("table: " + khzList(s.khz)).c_str());
    }
    ImGui::EndTable();
}

void channelView(const App& a) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    const bool on = live(a);
    if (!on || t.channels.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", on ? "no channel" : a.engine.running() ? "starting" : "start the receiver to see the channels"); ImGui::PopTextWrapPos(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    const float rowH = ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    const float labW = 150 * gUi, txtW = std::min(330.f * gUi, W * 0.5f), barW = std::max(60.f, W - labW - txtW - 12 * gUi);
    ImGui::BeginChild("##hfch", ImVec2(0, 0));
    for (const auto& c : t.channels) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(W, rowH));
        const float y0 = p.y + 2, y1 = p.y + rowH - 2, ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        char b[120];
        const dect2::HfdlStationInfo* s = dect2::hfdlStation(c.gsId);
        snprintf(b, sizeof b, "%.0f %s", c.freqHz / 1e3, s ? s->name : "");
        dl->AddCircleFilled(ImVec2(p.x + 5 * gUi, p.y + rowH * 0.5f), 4 * gUi, !c.inBand ? IM_COL32(70, 70, 70, 255) : c.active ? IM_COL32(110, 210, 120, 255) : IM_COL32(120, 124, 128, 255));
        dl->AddText(ImVec2(p.x + 14 * gUi, ty), c.inBand ? IM_COL32(220, 222, 225, 255) : IM_COL32(110, 114, 118, 255), b);
        const float bx = p.x + labW;
        dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + barW, y1), IM_COL32(8, 9, 10, 255));
        auto xOf = [&](float db) { return bx + barW * std::max(0.f, std::min(1.f, (db + 110.f) / 90.f)); };
        if (c.inBand && c.levelDb > -119) dl->AddRectFilled(ImVec2(bx, y0), ImVec2(xOf(c.levelDb), y1), c.active ? IM_COL32(110, 190, 110, 230) : IM_COL32(110, 120, 110, 110));
        if (c.inBand && c.noiseDb > -119) dl->AddLine(ImVec2(xOf(c.noiseDb), y0), ImVec2(xOf(c.noiseDb), y1), IM_COL32(230, 160, 70, 255), 2.f);
        if (!c.inBand) snprintf(b, sizeof b, "outside the sample band");
        else snprintf(b, sizeof b, "%llu bursts  %llu good  %llu bad  %d bit/s  SNR %.0f  %s", (unsigned long long)c.bursts, (unsigned long long)c.good, (unsigned long long)c.bad, c.lastBps,
                      c.snrDb, c.lastHeardSec > 0 ? (ago(t.nowSec, c.lastHeardSec) + " ago").c_str() : "-");
        dl->AddText(ImVec2(bx + barW + 10 * gUi, ty), c.active ? IM_COL32(200, 204, 208, 255) : IM_COL32(130, 134, 138, 255), b);
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Bar: level of the last burst (-110 to -20 dBFS); orange line: the channel's noise floor. Green dot: a good burst in the last 40 s.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    loadState();
    subNav("hfdlv", S.viewMode, {"Messages", "Map", "Aircraft", "Ground stations", "Channels", tr("Range")});
    if (S.viewMode == 0) messageView(a);
    else if (S.viewMode == 1) mapView(a);
    else if (S.viewMode == 2) aircraftView(a);
    else if (S.viewMode == 3) stationView(a);
    else if (S.viewMode == 4) channelView(a);
    else rangeView(S.range, ImGui::GetContentRegionAvail(), unitPerKm(), distUnit(), "hfdl", receiverLocationSet());
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("hfdl")) a.engine.hfdl().clearResults();
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    int heard = 0; for (const auto& g : t.stations) heard += g.heardDirect;
    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu aircraft, %d ground stations heard", t.aircraft.size(), heard); ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##hfl_s", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Aircraft"); ImGui::TableSetupColumn("Msgs");
    ImGui::TableHeadersRow();
    for (const auto& c : t.aircraft) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char id[24]; snprintf(id, sizeof id, "##hl%u", c.key);
        if (ImGui::Selectable((airName(c) + id).c_str(), S.selAir == c.key, ImGuiSelectableFlags_SpanAllColumns)) { S.selAir = c.key; snprintf(S.fAir, sizeof S.fAir, "%s", c.icao ? hex6(c.icao).c_str() : c.reg.c_str()); }
        ImGui::TableNextColumn(); ImGui::Text("%u", c.messages);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int inBand = 0, act = 0;
    uint64_t bursts = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; act += c.active; bursts += c.bursts; }
    kv("centre", "%.4f MHz, %d of %zu channels in the band, %d active", t.centerHz / 1e6, inBand, t.channels.size(), act);
    kv("rates", "%.0f kS/s in, 7.2 kS/s per channel (4 samples per symbol, 1800 baud)", t.inputRate / 1e3);
    kv("bursts", "%llu preambles, %llu failed", (unsigned long long)bursts, (unsigned long long)t.blocksBad);
    kv("PDUs", "%llu good: %llu squitters, %llu LPDUs, %llu ACARS", (unsigned long long)t.blocksOk, (unsigned long long)t.squitters, (unsigned long long)t.lpdus, (unsigned long long)t.acarsMsgs);
    kv("by rate", "300: %llu  600: %llu  1200: %llu  1800: %llu", (unsigned long long)t.byRate[0], (unsigned long long)t.byRate[1], (unsigned long long)t.byRate[2], (unsigned long long)t.byRate[3]);
    kv("last burst", "%+.1f Hz from the channel, SNR %.1f dB", t.cfoHz, t.snrDb);
    kv("positions", "%llu", (unsigned long long)t.positionsTotal);
    const double maxKm = receiverLocationSet() ? S.range.plot.overallMaxKm(rangeNow()) : 0;
    if (maxKm > 0) kv("farthest", "%.0f %s at %.0f deg", maxKm * unitPerKm(), distUnit(), S.range.plot.farthestAzDeg(rangeNow()));
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    int act = 0;
    for (const auto& c : t.channels) act += c.active;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Preambles", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("PDUs", !on ? 0 : t.state == 2 ? 1 : t.blocksOk > 0 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Preambles" : "Listening");
    snprintf(b, sizeof b, "%d", act); ro("Active", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("PDUs / bad", b);
    snprintf(b, sizeof b, "%zu", t.aircraft.size()); ro("Aircraft", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HFDL";
    if (live(a)) l2 = dect2::hfdlSummary(a.rx.hfdl);
}

void tuner(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("Band");
    ImGui::SetNextItemWidth(std::min(200 * gUi, ImGui::GetContentRegionAvail().x));
    static int bandSel = -1;   // the chosen band stays shown in the box
    if (ImGui::BeginCombo("##hfband", bandSel >= 0 ? kBands[bandSel].name : "presets")) {
        int i = 0;
        for (const auto& b : kBands) { if (ImGui::Selectable(b.name, i == bandSel)) { bandSel = i; tuneFreq(a, b.mhz); } i++; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune to an HFDL band: every ground station frequency of the built-in table inside the sample band is decoded at once.");
    ImGui::TextDisabled("Channels kHz");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputTextWithHint("##hfch", "the station table", S.chans, sizeof S.chans)) plat::prefs().setS("hfdlChans", S.chans);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The HFDL channels to decode (dial frequencies in kHz, upper sideband), separated by spaces.\nEmpty: every frequency of the built-in ground station table inside the sample band.\nWhole span (top right of the spectrum): the table plus this list; with it off, this list replaces the table.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated aircraft"); ImGui::PopTextWrapPos();
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 6;
    if (ImGui::SliderInt("##hfn", &n, 1, 24, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(150 * gUi);
    static const char* ch[] = {"clean channel", "HF moderate (1 ms, 0.5 Hz)", "HF poor (2 ms, 1 Hz)"};
    int c = std::max(0, std::min(2, sc.modeOpt[2]));
    if (ImGui::Combo("##hfpath", &c, ch, 3)) { sc.modeOpt[2] = c; changed = true; }
    flowNext(); ImGui::TextDisabled("rate"); ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    float rate = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##hfrate", &rate, 0.5f, 4.f, "x%.1f")) { sc.modeVal[0] = rate; changed = true; }
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float cfo = (float)sc.cfoHz;
    if (ImGui::SliderFloat("##hfcfo", &cfo, -200, 200, "%.0f Hz")) { sc.cfoHz = cfo; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::HfdlTelemetry& t = a.rx.hfdl;
    int act = 0;
    for (const auto& c : t.channels) act += c.active;
    out.push_back({"ACTIVE CH.", "%.0f", (double)act, 0, 8, act > 0 ? 1 : 0});
    out.push_back({"SNR  dB", "%.0f", t.snrDb, 0, 30, t.snrDb >= 15 ? 1 : t.snrDb >= 8 ? 2 : 3});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraft.size(), 0, 30, t.aircraft.empty() ? 0 : 1});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"GOOD  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 80 ? 1 : good >= 50 ? 2 : 3});
}

} // namespace

// For the map upload (app/map/map_send.cpp): the estimated position of an aircraft whose last fix is old (dead reckoning); false when there is none.
bool hfdlEstimatedPosition(uint32_t key, double now, double& lat, double& lon, double& ageSec) {
    if (!gPred.predict) return false;
    dect2::track::PosEstimate pe;
    if (!gPred.hub.estimate(dect2::track::keyFromIcao(key), now, pe) || !pe.valid || pe.coarse || !pe.predicted || pe.ageSec > 1800) return false;
    lat = pe.lat; lon = pe.lon; ageSec = pe.ageSec;
    return true;
}



// For the map upload (app/map/map_send.cpp)
derivedkin::Info hfdlKinematics(const dect2::HfdlAircraft& c, double now) { return kinOf(c, now); }

extern const ModeUi kHfdlUi;
const ModeUi kHfdlUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
