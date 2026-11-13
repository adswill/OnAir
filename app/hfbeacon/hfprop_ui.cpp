// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF propagation dashboard: which bands are open now (from what this receiver heard in the last 30 minutes), a 24 hour history per band and the
// band to try now. See hfprop_ui.h.
#include "dect2/common/timecompat.h"
#include "hfprop_ui.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace hfprop_ui {

namespace {
using namespace dect2;

struct St {
    bool loaded = false;
    hfp::Model m;
    std::vector<double> lastBeacon;                 // per beacon: the lastHeard already counted
    std::unordered_set<std::string> seen;           // decodes already counted: mode/slot/call
    double lastSave = 0;
    bool dirty = false;
};
St G;

double utcNow() { return (double)std::time(nullptr); }
std::string path() { return plat::dataDir() + "/hf-propagation.txt"; }

void ensureLoaded() {
    if (G.loaded) return;
    G.loaded = true;
    std::ifstream f(path(), std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    G.m.load(ss.str(), utcNow());
    G.lastSave = utcNow();
}

void maybeSave() {
    if (!G.dirty || utcNow() - G.lastSave < 300) return;
    G.lastSave = utcNow(); G.dirty = false;
    std::ofstream f(path(), std::ios::binary | std::ios::trunc);
    f << G.m.save();
}

ImU32 stateCol(int s, int a = 255) {
    return s == hfp::Open ? IM_COL32(90, 210, 130, a) : s == hfp::Marginal ? IM_COL32(230, 180, 70, a) : IM_COL32(120, 120, 120, a);
}
const char* stateText(int s) { return s == hfp::Open ? tr("open") : s == hfp::Marginal ? tr("marginal") : tr("closed"); }

} // namespace

hfp::Model& model() { ensureLoaded(); return G.m; }

void feedBeacons(const std::vector<hfb::Beacon>& list, const HfbTelemetry& t) {
    ensureLoaded();
    if (!t.clockValid || t.beacons.size() != list.size()) return;   // without the computer clock the times are only stream seconds
    if (G.lastBeacon.size() != list.size()) G.lastBeacon.assign(list.size(), 0);
    for (size_t i = 0; i < list.size(); i++) {
        const HfbState& s = t.beacons[i];
        if (!s.heard || s.lastHeard <= G.lastBeacon[i]) continue;
        G.lastBeacon[i] = s.lastHeard;
        if (s.conf < 2 && list[i].kind != hfb::Carrier) continue;   // identity confirmed by the keying / schedule only (carriers have none)
        hfp::Obs o;
        o.utc = s.lastHeard; o.hz = list[i].khz * 1e3; o.id = list[i].name;
        o.snrDb = std::isnan(s.snr) ? 0.f : s.snr;
        o.distKm = std::isnan(s.distKm) ? -1 : s.distKm;
        o.source = list[i].kind == hfb::Ncdxf ? hfp::Ncdxf : list[i].kind == hfb::Letter ? hfp::Letter : list[i].kind == hfb::Dashes ? hfp::Dashes : hfp::TimeSignal;
        G.m.add(o);
        G.dirty = true;
    }
    maybeSave();
}

void feedFtx(App& a, const HfdigFtxTelemetry& f) {
    ensureLoaded();
    double lat = 0, lon = 0; receiverLocation(lat, lon);
    const bool have = receiverLocationSet();
    for (const FtxDecode& d : f.decodes) {
        const int src = d.mode == 0 ? hfp::Ft8 : d.mode == 1 ? hfp::Ft4 : d.mode == 2 ? hfp::Ft2 : d.mode == 3 ? hfp::Wspr : -1;
        if (src < 0 || d.call.empty() || d.slotUtc <= 0) continue;
        char key[96]; snprintf(key, sizeof key, "%d/%.0f/%s", d.mode, d.slotUtc, d.call.c_str());
        if (!G.seen.insert(key).second) continue;
        if (G.seen.size() > 6000) { G.seen.clear(); G.seen.insert(key); }
        hfp::Obs o;
        o.utc = d.slotUtc; o.source = src; o.id = d.call; o.snrDb = d.snrDb;
        const double dial = d.dialHz > 0 ? d.dialHz : a.freqMhz * 1e6;
        o.hz = dial + (d.mirrored ? -d.hz : d.hz);
        o.distKm = -1;
        double sl, so;
        if (have && !d.grid.empty() && fromMaidenhead(d.grid, sl, so)) { double bearing; greatCircle(lat, lon, sl, so, o.distKm, bearing); }
        G.m.add(o);
        G.dirty = true;
    }
    maybeSave();
}

void view(App& a) {
    ensureLoaded();
    const double now = utcNow();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("What this receiver has heard, by band: beacons (NCDXF/IARU, Long Dashes, single letters, carriers) and the FT8 / FT4 / FT2 / WSPR stations decoded in the HF digital screen. "
                                 "A band is open when at least 3 stations were heard in the last 30 minutes. Own receiver only: nothing is downloaded, so no solar flux or K index."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    std::string why;
    const int best = G.m.best(now, &why);
    ImGui::AlignTextToFramePadding();
    if (best >= 0) {
        ImGui::Text("%s", tr("Best band now"));
        ImGui::SameLine();
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(ImVec4(0.35f, 0.82f, 0.5f, 1.f), "%s", hfp::bandName(best)); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextDisabled("%s", why.c_str());
    } else {
        ImGui::TextDisabled("%s", tr("Nothing heard in the last 30 minutes: start the HF beacons or the HF digital screen and leave it running."));
    }
    ImGui::Spacing();
    const auto v = G.m.now(now);
    if (ImGui::BeginTable("##hfpn", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Band")); ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Stations"));
        ImGui::TableSetupColumn(tr("Best SNR")); ImGui::TableSetupColumn(tr("Farthest"));
        ImGui::TableSetupColumn(tr("Heard from"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& r : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hfp::bandName(r.band));
            ImGui::TableNextColumn(); ImGui::TextColored(ImColor(stateCol(r.state)).Value, "%s", stateText(r.state));
            ImGui::TableNextColumn(); ImGui::Text("%d", r.stations);
            ImGui::TableNextColumn(); if (r.stations) ImGui::Text("%+.0f dB", (double)r.bestSnr); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.maxKm > 0) ImGui::Text("%.0f km", r.maxKm); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            std::string s;
            for (int q = 0; q < hfp::kSources; q++) if (r.bySource[q]) { char b[64]; snprintf(b, sizeof b, "%s%s %d", s.empty() ? "" : "  ", hfp::sourceName(q), r.bySource[q]); s += b; }
            if (s.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(s.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Last 24 hours (UTC), 15 minute cells, darker: fewer stations"));
    const float rowH = std::max(14.f * gUi, ImGui::GetTextLineHeight()), labW = 50 * gUi;
    const float w = std::max(200.f * gUi, ImGui::GetContentRegionAvail().x - labW - 8 * gUi), cell = w / hfp::kBins;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int b = 0; b < hfp::kBands; b++) {
        const float y = p0.y + b * (rowH + 2 * gUi);
        dl->AddText(ImVec2(p0.x, y), IM_COL32(200, 200, 200, 255), hfp::bandName(b));
        const auto h = G.m.history(b, now);
        for (int i = 0; i < hfp::kBins; i++) {
            const float x = p0.x + labW + i * cell;
            const int n = h[(size_t)i].stations;
            const int al = n == 0 ? 0 : std::min(255, 70 + n * 30);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell + 0.5f, y + rowH), IM_COL32(255, 255, 255, 14));
            if (n) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell + 0.5f, y + rowH), n >= 3 ? stateCol(hfp::Open, al) : stateCol(hfp::Marginal, al));
        }
    }
    // hour ticks along the bottom
    const float yb = p0.y + hfp::kBands * (rowH + 2 * gUi);
    const double cur = std::floor(now / hfp::kBinSec) * hfp::kBinSec;
    for (int i = 0; i < hfp::kBins; i++) {
        const double st = cur - (hfp::kBins - 1 - i) * hfp::kBinSec;
        if (std::fmod(st, 3 * 3600.0) != 0) continue;
        char tx[8]; snprintf(tx, sizeof tx, "%02d", (int)std::fmod(st / 3600.0, 24.0));
        dl->AddText(ImVec2(p0.x + labW + i * cell, yb), IM_COL32(160, 160, 160, 255), tx);
    }
    ImGui::Dummy(ImVec2(labW + w, hfp::kBands * (rowH + 2 * gUi) + ImGui::GetTextLineHeight() + 2 * gUi));
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int b = (int)((m.y - p0.y) / (rowH + 2 * gUi)), i = (int)((m.x - p0.x - labW) / cell);
        if (b >= 0 && b < hfp::kBands && i >= 0 && i < hfp::kBins) {
            const hfp::Bin c = G.m.history(b, now)[(size_t)i];
            if (c.count) {
                const time_t tt = (time_t)c.stamp; struct tm g;
                dect2::gmTime(tt, &g);
                ImGui::SetTooltip("%s %02d:%02d UTC: %d, %.0f km, %+.0f dB", hfp::bandName(b), g.tm_hour, g.tm_min, c.stations, c.maxKm, (double)c.bestSnr);
            }
        }
    }
    if (ImGui::SmallButton(tr("Forget the history"))) { G.m.clear(); G.seen.clear(); G.dirty = true; }
}

} // namespace hfprop_ui
