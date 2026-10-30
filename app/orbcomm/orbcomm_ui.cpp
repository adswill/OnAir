// Orbcomm screens: the satellites on the map (position from their own ephemeris, with the recent track), the satellite list (ID,
// last heard, Doppler, position), the packet log by type, and per downlink the differential constellation and the eye.
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include "ui/ui_common.h"
#include "../adsb/adsb_map.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using dect2::OrbcommTelemetry;
using dect2::OrbcommSat;
using dect2::OrbcommChannel;
namespace oc = dect2::orbcomm;

struct State {
    bool loaded = false;
    int view = 0;                 // 0 map, 1 packets, 2 satellites, 3 signal
    int selSat = -1;
    adsbmap::View map;
    bool mapFollow = true;
    int maxCh = 2;
    int pushedMaxCh = -1;
    bool show[oc::kTypeSlots] = {true, true, true, true, true, true, true, true, true};
    bool follow = true;
};
State S;

bool live(const App& a) { return a.rx.standard == 45; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("orbcommZoom", 2);
    S.maxCh = std::clamp((int)d.getI("orbcommMaxCh", 2), 1, 2);
}

ImU32 satColour(int id, float alpha = 1.f) {
    static const int c[8][3] = {{90, 210, 150}, {240, 160, 70}, {90, 170, 245}, {230, 100, 110}, {190, 140, 240}, {80, 205, 200}, {240, 210, 90}, {200, 200, 205}};
    const int* k = c[(id < 0 ? 0 : id) % 8];
    return IM_COL32(k[0], k[1], k[2], (int)(255 * alpha));
}

std::string ageText(double s) {
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", std::max(0.0, s));
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

void kvLine(App& a, const char* k, const char* v, float col = 110) {
    ImGui::TextDisabled("%s", k); kvColumn(col * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

const OrbcommSat* findSat(const OrbcommTelemetry& t, int id) {
    for (const auto& s : t.sats) if (s.id == id) return &s;
    return nullptr;
}

// ---------------------------------------------------------------- the map

void mapView(App& a, ImVec2 size) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& s : t.sats) if (s.hasPos) { la0 = std::min(la0, s.lat); la1 = std::max(la1, s.lat); lo0 = std::min(lo0, s.lon); lo1 = std::max(lo1, s.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(40.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));   // a footprint is some 50 degrees across
            S.map.zoom = std::max(1, std::min(6, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    adsbmap::fullCorner(p0, size);   // the shared full-screen button (#195)
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    int hit = -1; float best = 16 * gUi;
    if (on) {
        for (const auto& s : t.sats) {
            for (size_t i = 3; i < s.track.size(); i += 2)
                dl->AddLine(adsbmap::project(s.track[i - 3], s.track[i - 2]), adsbmap::project(s.track[i - 1], s.track[i]), satColour(s.id, 0.5f), 1.5f);
            if (!s.hasPos) continue;
            const ImVec2 q = adsbmap::project(s.lat, s.lon);
            const bool sel = s.id == S.selSat;
            const float r = (sel ? 7.f : 6.f) * gUi;
            const ImVec2 d[4] = {{q.x, q.y - r}, {q.x + r, q.y}, {q.x, q.y + r}, {q.x - r, q.y}};
            dl->AddConvexPolyFilled(d, 4, satColour(s.id, s.chan >= 0 ? 1.f : 0.5f));
            dl->AddPolyline(d, 4, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, sel ? 1.6f : 1.f);
            char b[24]; snprintf(b, sizeof b, "FM%d", s.id);
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
    mapCornerButtons(S.map, S.mapFollow, "never a position");
    plat::prefs().setI("orbcommZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& s : t.sats) if (s.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no ephemeris yet (each satellite sends its position once a second)");
    else adsbmap::legend(size.x, "%d of %zu satellites with a position from their ephemeris", withPos, t.sats.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- lists

void satTable(App& a, bool compact) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    const int cols = compact ? 4 : 8;
    if (!ImGui::BeginTable("##orbsats", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("ID");
    ImGui::TableSetupColumn("heard");
    ImGui::TableSetupColumn("Doppler");
    ImGui::TableSetupColumn(compact ? "pos" : "position");
    if (!compact) { ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Es/N0"); ImGui::TableSetupColumn("packets"); ImGui::TableSetupColumn("bad"); }
    ImGui::TableHeadersRow();
    std::vector<const OrbcommSat*> v;
    for (const auto& s : t.sats) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](const OrbcommSat* x, const OrbcommSat* y) { return x->lastHeard > y->lastHeard; });
    ImGui::PushFont(a.mono, 0);
    for (const OrbcommSat* s : v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char b[48]; snprintf(b, sizeof b, "%d%s##orbs%d", s->id, s->chan >= 0 ? " *" : "", s->id);
        if (ImGui::Selectable(b, S.selSat == s->id, ImGuiSelectableFlags_SpanAllColumns)) S.selSat = s->id;
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(t.timeSec - s->lastHeard).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", s->dopplerHz);
        ImGui::TableNextColumn();
        if (s->hasPos) ImGui::Text(compact ? "%.0f %.0f" : "%.2f %.2f %.0f km", s->lat, s->lon, s->altKm); else ImGui::TextDisabled("-");
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::Text("%.4f", s->freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s->snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s->packets);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s->bad);
        }
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void satDetail(App& a) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    const OrbcommSat* s = findSat(t, S.selSat);
    if (!s && !t.sats.empty()) s = &t.sats.front();
    if (!s) { ImGui::TextDisabled(live(a) ? "no satellite yet" : "start the receiver"); return; }
    char b[160];
    snprintf(b, sizeof b, "%d%s", s->id, s->chan >= 0 ? "  (heard now)" : ""); kvLine(a, "satellite", b);
    snprintf(b, sizeof b, "%.4f MHz, channel %.4f", s->freqHz / 1e6, s->nominalHz / 1e6); kvLine(a, "carrier", b);
    snprintf(b, sizeof b, "%+.0f Hz", s->dopplerHz); kvLine(a, "Doppler", b);
    kvLine(a, "last heard", (ageText(t.timeSec - s->lastHeard) + " ago").c_str());
    if (s->hasPos) { snprintf(b, sizeof b, "%.3f, %.3f  %.0f km", s->lat, s->lon, s->altKm); kvLine(a, "position", b); }
    if (s->hasElements) { snprintf(b, sizeof b, "incl %.2f  RAAN %.2f  arg lat %.2f  period %.2f min", s->inclDeg, s->raanDeg, s->argLatDeg, s->periodMin); kvLine(a, "elements", b); }
    auto list = [&](const char* k, const std::vector<double>& v) {
        if (v.empty()) return;
        std::string x; for (double f : v) { snprintf(b, sizeof b, "%s%.4f", x.empty() ? "" : "  ", f); x += b; }
        kvLine(a, k, (x + " MHz").c_str());
    };
    list("downlinks", s->downlinksMhz);
    list("uplinks", s->uplinksMhz);
    snprintf(b, sizeof b, "%llu frames, %llu packets, %llu bad", (unsigned long long)s->frames, (unsigned long long)s->packets, (unsigned long long)s->bad); kvLine(a, "received", b);
}

void packetLog(App& a, float H) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    for (int k = 0; k < oc::kTypeSlots; k++) {
        if (k == oc::TSync || k == oc::TFill) continue;   // only counted
        char b[48]; snprintf(b, sizeof b, "%s %llu##orbt%d", oc::typeLabel(k), (unsigned long long)t.typeCount[k], k);
        ImGui::Checkbox(b, &S.show[k]);
        flowNext(8 * gUi);
    }
    ImGui::Checkbox("follow", &S.follow);
    flowEnd();
    ImGui::BeginChild("##orblog", ImVec2(0, std::max(80.f, H - ImGui::GetCursorPosY())), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    for (const auto& p : t.log) {
        if (p.type < 0 || p.type >= oc::kTypeSlots || !S.show[p.type]) continue;
        ImGui::TextDisabled("%8.1f  %3d  [%d]", p.time, p.sat, p.chan);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(satColour(p.sat)), "%-9s", oc::typeLabel(p.type));
        ImGui::SameLine();
        ImGui::TextUnformatted(p.text.c_str());
    }
    if (S.follow) ImGui::SetScrollHereY(1.f);
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- constellation and eye

void constel(const OrbcommChannel& c, ImVec2 size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 18, 255), 3.f);
    const ImVec2 m((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    const float r = std::min(size.x, size.y) * 0.4f;
    dl->AddLine(ImVec2(p0.x, m.y), ImVec2(p1.x, m.y), IM_COL32(50, 54, 58, 255));
    dl->AddLine(ImVec2(m.x, p0.y), ImVec2(m.x, p1.y), IM_COL32(50, 54, 58, 255));
    dl->AddCircle(m, r, IM_COL32(50, 54, 58, 255), 48);
    for (const auto& z : c.constel) dl->AddRectFilled(ImVec2(m.x + z.real() * r - 1, m.y - z.imag() * r - 1), ImVec2(m.x + z.real() * r + 1, m.y - z.imag() * r + 1), IM_COL32(110, 220, 160, 170));
    ImGui::Dummy(size);
}

void eye(const OrbcommChannel& c, ImVec2 size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 18, 255), 3.f);
    auto y = [&](float v) { return p0.y + size.y * 0.5f - std::clamp(v, -2.f, 2.f) * size.y * 0.22f; };
    for (float v : {-1.f, 0.f, 1.f}) dl->AddLine(ImVec2(p0.x, y(v)), ImVec2(p1.x, y(v)), IM_COL32(50, 54, 58, 255));
    const int L = c.eyeLen;
    if (L > 1)
        for (size_t s = 0; s + L <= c.eye.size(); s += L)
            for (int i = 1; i < L; i++) {
                const float a = c.eye[s + i - 1], b = c.eye[s + i];
                if (std::fabs(a - b) > 1.5f) continue;   // the wrap at a half turn
                dl->AddLine(ImVec2(p0.x + (i - 1) * size.x / (L - 1), y(a)), ImVec2(p0.x + i * size.x / (L - 1), y(b)), IM_COL32(240, 190, 90, 90));
            }
    ImGui::Dummy(size);
}

void signalView(App& a, float W, float H) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    const float gap = 12 * gUi, w = std::max(120.f, (W - gap) / 2);
    for (int i = 0; i < dect2::kOrbcommChannels; i++) {
        const OrbcommChannel& c = t.ch[i];
        if (i) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        if (!c.active) captionFit(w, "downlink %d: free", i + 1);
        else captionFit(w, "downlink %d: %.4f MHz  sat %d  %+.0f Hz  %.1f dB", i + 1, c.freqHz / 1e6, c.sat, c.dopplerHz, c.snrDb);
        const float h = std::max(60.f, (H - 3 * ImGui::GetTextLineHeightWithSpacing()) / 2);
        constel(c, ImVec2(w, h));
        eye(c, ImVec2(w, h));
        ImGui::EndGroup();
    }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    kit::modeHeader({44, "Orbcomm", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("orbv", S.view, {{tr("Live"), 0}, {tr("Tracks"), 4}, {tr("Log"), 1, true}, {tr("Satellites"), 2, true}, {tr("Signal"), 3, true}});
    if (S.view == 4) { kit::sessionItems(44, 3, "orbcomm"); return; }
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 1) { packetLog(a, ImGui::GetCursorPosY() + H); return; }
    if (S.view == 2) { ImGui::BeginChild("##orbst", ImVec2(0, 0)); satTable(a, false); ImGui::Spacing(); satDetail(a); ImGui::EndChild(); return; }
    if (S.view == 3) { signalView(a, W, H); return; }
    const float cw = std::min(340.f * gUi, W * 0.36f);
    ImGui::BeginChild("##orb_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##orb_r", ImVec2(0, H));
    satDetail(a);
    ImGui::EndChild();
}

void list(App& a) {
    loadState();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see satellites"); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("orbcomm")) a.engine.orbcomm().clearResults();
    ImGui::TextDisabled("%zu satellites", a.rx.orbcomm.sats.size());
    satTable(a, true);
}

void panels(App& a) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(110.f, (W - 4 * gap) / 4.f), plotH = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    for (int i = 0; i < dect2::kOrbcommChannels; i++) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        captionFit(colW, t.ch[i].active ? "Downlink %d, sat %d" : "Downlink %d", i + 1, t.ch[i].sat);
        constel(t.ch[i], ImVec2(colW, plotH));
        ImGui::EndGroup();
    }
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Eye, downlink 1");
    eye(t.ch[0], ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Packets by type");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##orbtypes", ImVec2(colW, plotH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int nb = oc::kTypeSlots;
        uint64_t mx = 1;
        for (int i = 0; i < nb; i++) mx = std::max(mx, t.typeCount[i]);
        const float bw = colW / nb, base = p.y + plotH - 14 * gUi;
        for (int i = 0; i < nb; i++) {
            const float h = (plotH - 18 * gUi) * (float)t.typeCount[i] / (float)mx;
            dl->AddRectFilled(ImVec2(p.x + i * bw + 2, base - h), ImVec2(p.x + (i + 1) * bw - 2, base), ImGui::ColorConvertFloat4ToU32(pal::accent(i == oc::TFill ? 0.35f : 0.8f)));
            const char* lb = oc::typeLabel(i);
            char sh[4] = {lb[0], lb[1], lb[2], 0};
            while (sh[0] && ImGui::CalcTextSize(sh).x > bw - 2) sh[strlen(sh) - 1] = 0;
            const ImVec2 ts = ImGui::CalcTextSize(sh);
            if (sh[0]) dl->AddText(ImVec2(p.x + i * bw + (bw - ts.x) * 0.5f, base + 1 * gUi), IM_COL32(150, 154, 158, 255), sh);
        }
    }
    ImGui::EndGroup();
}

void receiver(App& a) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    char b[160];
    kvLine(a, "state", t.state == 0 ? "searching for downlinks" : t.state == 1 ? "carrier found, no frames yet" : "decoding frames", 130);
    snprintf(b, sizeof b, "%llu good, %llu bad checksum", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kvLine(a, "packets", b, 130);
    snprintf(b, sizeof b, "%.3f MHz, %.2f Msps", t.centerHz / 1e6, t.inputRate / 1e6); kvLine(a, "centre", b, 130);
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.messages); kvLine(a, "messages", b, 130);
    static const char* const kSt[] = {"free", "finding the carrier", "tracking, looking for sync", "frame lock"};
    for (int i = 0; i < dect2::kOrbcommChannels; i++) {
        const OrbcommChannel& c = t.ch[i];
        ImGui::Spacing();
        ImGui::TextDisabled("DOWNLINK %d", i + 1);
        kvLine(a, "state", kSt[std::clamp(c.state, 0, 3)], 130);
        if (!c.active) continue;
        snprintf(b, sizeof b, "%.4f MHz (offset %+.1f kHz)", c.freqHz / 1e6, c.offsetHz / 1e3); kvLine(a, "carrier", b, 130);
        snprintf(b, sizeof b, "%+.0f Hz", c.dopplerHz); kvLine(a, "Doppler", b, 130);
        snprintf(b, sizeof b, "%.1f dB, clock %+.0f ppm", c.snrDb, c.clockPpm); kvLine(a, "Es/N0", b, 130);
        snprintf(b, sizeof b, "sat %d, %llu frames, %llu ok, %llu bad, sync lost %llu", c.sat, (unsigned long long)c.frames, (unsigned long long)c.pktOk, (unsigned long long)c.pktBad, (unsigned long long)c.syncLost); kvLine(a, "frames", b, 130);
    }
    ImGui::Spacing();
    satTable(a, false);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const OrbcommTelemetry& t = a.rx.orbcomm;
    statusIqLamp(a);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
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
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Carrier" : "Searching");
    snprintf(b, sizeof b, "%zu", t.sats.size()); ro("Satellites", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / bad", b);
    for (int i = 0; i < dect2::kOrbcommChannels; i++)
        if (t.ch[i].active) { snprintf(b, sizeof b, "%+.0f Hz", t.ch[i].dopplerHz); ro(i ? "Doppler 2" : "Doppler 1", b); }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Orbcomm";
    if (live(a)) l2 = dect2::orbcommSummary(a.rx.orbcomm);
}

void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedMaxCh = -1; return; }
    if (S.pushedMaxCh != S.maxCh) { a.engine.orbcomm().setMaxChannels(S.maxCh); S.pushedMaxCh = S.maxCh; }
}

void tuner(App& a, bool& retune) {
    (void)retune;
    ImGui::TextDisabled("Channel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Orbcomm subscriber downlinks, 137.2 - 137.8 MHz. Every downlink in the passband is found; the presets put a channel near the centre.");
    sameLineIf(90 * gUi, 5 * gUi);
    int n = 0;
    const dect2::OrbcommPreset* p = dect2::orbcommPresets(n);
    char cur[32]; snprintf(cur, sizeof cur, "%.4f MHz", a.freqMhz);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##orbch", cur)) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(p[i].name, std::fabs(a.freqMhz - p[i].mhz) < 0.0005)) tuneFreq(a, p[i].mhz);
        ImGui::EndCombo();
    }
}

void decoder(App& a, bool&) {
    (void)a;
    loadState();
    ImGui::TextDisabled("DOWNLINKS"); ImGui::SameLine(0, 5 * gUi);
    static const char* const kN[] = {"1", "2"};
    int k = S.maxCh - 1;
    ImGui::SetNextItemWidth(std::min(60 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##orbmax", &k, kN, 2)) { S.maxCh = k + 1; plat::prefs().setI("orbcommMaxCh", S.maxCh); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many downlinks in the passband are decoded at once.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("downlinks");
    flowNext(); ImGui::SetNextItemWidth(60 * gUi);
    static const char* const kN[] = {"2", "1"};
    int n = sc.modeOpt[0] == 1 ? 1 : 0;
    if (ImGui::Combo("##orbsn", &n, kN, 2)) { sc.modeOpt[0] = n == 1 ? 1 : 0; changed = true; }
    flowNext();
    bool ramp = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("Doppler ramp", &ramp)) { sc.modeOpt[1] = ramp ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sweep the Doppler between -4 and +4 kHz (30 s each way) instead of a fixed offset.");
    flowNext(); ImGui::TextDisabled("bad checksums"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    int bad = sc.modeOpt[2];
    if (ImGui::SliderInt("##orbbad", &bad, 0, 50, "%d %%")) { sc.modeOpt[2] = bad; changed = true; }
    flowNext(); ImGui::TextDisabled("clock"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float ppm = (float)sc.modeVal[1];
    if (ImGui::SliderFloat("##orbppm", &ppm, -300.f, 300.f, "%.0f ppm")) { sc.modeVal[1] = ppm; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const OrbcommTelemetry& t = a.rx.orbcomm;
    out.push_back({"Es/N0  dB", "%.0f", t.snrDb, 0, 25, t.snrDb >= 12 ? 1 : t.snrDb >= 8 ? 2 : 3});
    out.push_back({"SATELLITES", "%.0f", (double)t.sats.size(), 0, 6, t.sats.empty() ? 0 : 1});
    out.push_back({"DOPPLER  Hz", "%+.0f", t.cfoHz, -5000, 5000, t.state ? 1 : 0});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"PACKETS OK  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 90 ? 1 : good >= 60 ? 2 : 3});
}

} // namespace

extern const ModeUi kOrbcommUi;
const ModeUi kOrbcommUi = {
    .sideTitle = "SATELLITES",
    .tabName = "Orbcomm",
    .tabIcon = Ic::Globe,
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
