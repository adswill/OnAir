// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "My ionosphere": see iono_obs_ui.h. The maths is in core/src/iono/iono_obs.cpp (unit tested); this file keeps the local log and draws it.
#include "dect2/common/timecompat.h"
#include "iono_obs_ui.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/iono/iono_obs.h"
#include "dect2/prop/prop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <unordered_set>

namespace ionoobs_ui {

namespace {
namespace io = dect2::ionoobs;

struct St {
    bool loaded = false, dirty = false;
    std::vector<io::Obs> obs;
    std::unordered_set<std::string> seen;
    std::vector<double> lastBeacon;
    double lastSave = 0;
    int window = 0, minDecodes = 1;
    std::string note;
};
St G;

int64_t nowUtc() { return (int64_t)std::time(nullptr); }
std::string path() { return plat::dataDir() + "/ionosphere-observations.tsv"; }

void ensureLoaded() {
    if (G.loaded) return;
    G.loaded = true;
    io::loadFile(path(), G.obs);
    io::trim(G.obs, nowUtc());
    G.lastSave = (double)nowUtc();
}
void maybeSave(bool force = false) {
    if (!G.dirty || (!force && (double)nowUtc() - G.lastSave < 300)) return;
    io::trim(G.obs, nowUtc());   // 30 days, 4 MB, 200000 records
    G.lastSave = (double)nowUtc();
    G.dirty = !io::saveFile(path(), G.obs);
}
void add(const io::Obs& o) {
    if (io::addObs(G.obs, o)) G.dirty = true;
}

ImVec2 proj(const ImVec2& p0, float w, float h, double lat, double lon) {
    return ImVec2(p0.x + (float)((lon + 180.0) / 360.0 * w), p0.y + (float)((90.0 - lat) / 180.0 * h));
}

void gridView(const io::HeardGrid& g, int64_t now) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float labW = 44 * gUi, rowH = 14 * gUi, cell = std::max(8.f, (ImGui::GetContentRegionAvail().x - labW) / 24.f);
    for (int b = 0; b < io::kBands; b++) {
        const float y = p0.y + (io::kBands - 1 - b) * (rowH + 2 * gUi);
        dl->AddText(ImVec2(p0.x, y), IM_COL32(190, 190, 190, 255), io::bandName(b));
        for (int h = 0; h < 24; h++) {
            const ImVec2 a(p0.x + labW + h * cell, y), z(a.x + cell - 1, y + rowH);
            if (g.count[b][h]) dl->AddRectFilled(a, z, io::snrColor(g.bestSnr[b][h]));
            else dl->AddRectFilled(a, z, IM_COL32(45, 45, 50, 255));
        }
    }
    const float yb = p0.y + io::kBands * (rowH + 2 * gUi);
    for (int h = 0; h < 24; h += 3) {
        char tx[8]; snprintf(tx, sizeof tx, "%02d", h);
        dl->AddText(ImVec2(p0.x + labW + h * cell, yb), IM_COL32(160, 160, 160, 255), tx);
    }
    const int cur = io::utcHourOf(now);
    dl->AddRect(ImVec2(p0.x + labW + cur * cell - 1, p0.y - 1), ImVec2(p0.x + labW + (cur + 1) * cell, yb), IM_COL32(255, 255, 255, 120));
    ImGui::Dummy(ImVec2(labW + 24 * cell, io::kBands * (rowH + 2 * gUi) + ImGui::GetTextLineHeight() + 2 * gUi));
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int row = (int)((m.y - p0.y) / (rowH + 2 * gUi)), h = (int)((m.x - p0.x - labW) / cell);
        const int b = io::kBands - 1 - row;
        if (b >= 0 && b < io::kBands && h >= 0 && h < 24) {
            if (g.count[b][h]) ImGui::SetTooltip("%s %02d UTC: %d heard, best %+.0f dB", io::bandName(b), h, g.count[b][h], (double)g.bestSnr[b][h]);
            else ImGui::SetTooltip("%s %02d UTC: nothing heard (or not listened to)", io::bandName(b), h);
        }
    }
}

void mapView(const std::vector<io::Obs>& v, int64_t now, float h) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 28, 40, 255));
    dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + h), true);
    // the night side: a shade per 5 degree cell (no coastline data is needed to see the line)
    for (int la = -90; la < 90; la += 5)
        for (int lo = -180; lo < 180; lo += 5)
            if (io::isNight(la + 2.5, lo + 2.5, now))
                dl->AddRectFilled(proj(p0, w, h, la + 5, lo), proj(p0, w, h, la, lo + 5), IM_COL32(0, 0, 0, 90));
    for (int lo = -150; lo <= 180; lo += 30) dl->AddLine(proj(p0, w, h, -90, lo), proj(p0, w, h, 90, lo), IM_COL32(255, 255, 255, 18));
    for (int la = -60; la <= 60; la += 30) dl->AddLine(proj(p0, w, h, la, -180), proj(p0, w, h, la, 180), IM_COL32(255, 255, 255, 18));
    const auto term = io::terminatorLine(now);
    for (size_t i = 1; i < term.size(); i++) {
        if (std::fabs(term[i].second - term[i - 1].second) > 180) continue;   // wrapped over the date line
        dl->AddLine(proj(p0, w, h, term[i - 1].first, term[i - 1].second), proj(p0, w, h, term[i].first, term[i].second), IM_COL32(255, 200, 80, 150), 1.5f);
    }
    double sl, so;
    io::subSolarPoint(now, sl, so);
    dl->AddCircleFilled(proj(p0, w, h, sl, so), 4 * gUi, IM_COL32(255, 220, 90, 255));
    std::vector<const io::Obs*> order;   // weak first, so the strong paths end on top
    for (const io::Obs& o : v) order.push_back(&o);
    std::sort(order.begin(), order.end(), [](const io::Obs* a, const io::Obs* b) { return a->snrDb < b->snrDb; });
    for (const io::Obs* o : order) {
        const double d = io::pathKm(*o);
        const int n = std::clamp((int)(d / 400.0), 2, 40);
        const double brg = io::bearingDeg(o->rxLat, o->rxLon, o->txLat, o->txLon);
        ImVec2 prev = proj(p0, w, h, o->rxLat, o->rxLon);
        double prevLon = o->rxLon;
        for (int i = 1; i <= n; i++) {
            double la, lo;
            dect2::prop::destination(o->rxLat, o->rxLon, brg, d * i / n, la, lo);
            const ImVec2 q = proj(p0, w, h, la, lo);
            if (std::fabs(lo - prevLon) < 180) dl->AddLine(prev, q, io::snrColor(o->snrDb), 1.2f);
            prev = q; prevLon = lo;
        }
    }
    if (!v.empty()) dl->AddCircleFilled(proj(p0, w, h, v.back().rxLat, v.back().rxLon), 4 * gUi, IM_COL32(120, 200, 255, 255));
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h));
}

} // namespace

void feedFtx(App& a, const dect2::HfdigFtxTelemetry& f) {
    ensureLoaded();
    if (!receiverLocationSet()) return;   // without our own position there is no path
    double lat = 0, lon = 0; receiverLocation(lat, lon);
    for (const dect2::FtxDecode& d : f.decodes) {
        const io::Source src = d.mode == 0 ? io::Source::Ft8 : d.mode == 1 ? io::Source::Ft4 : d.mode == 3 ? io::Source::Wspr : io::Source::Other;
        if (src == io::Source::Other || d.call.empty() || d.grid.size() < 4 || d.slotUtc <= 0) continue;
        char key[96]; snprintf(key, sizeof key, "%d/%.0f/%s", d.mode, d.slotUtc, d.call.c_str());
        if (!G.seen.insert(key).second) continue;
        if (G.seen.size() > 6000) { G.seen.clear(); G.seen.insert(key); }
        double tl, to;
        if (!dect2::fromMaidenhead(d.grid, tl, to)) continue;
        io::Obs o;
        o.utc = (int64_t)d.slotUtc; o.src = src; o.id = d.call; o.snrDb = d.snrDb;
        const double dial = d.dialHz > 0 ? d.dialHz : a.freqMhz * 1e6;
        o.fHz = dial + (d.mirrored ? -d.hz : d.hz);
        if (io::bandOf(o.fHz) < 0) o.fHz = dial;
        o.rxLat = lat; o.rxLon = lon; o.txLat = tl; o.txLon = to;
        add(o);
    }
    maybeSave();
}

void feedBeacons(const std::vector<dect2::hfb::Beacon>& list, const dect2::HfbTelemetry& t) {
    ensureLoaded();
    if (!receiverLocationSet() || !t.clockValid || t.beacons.size() != list.size()) return;
    double lat = 0, lon = 0; receiverLocation(lat, lon);
    if (G.lastBeacon.size() != list.size()) G.lastBeacon.assign(list.size(), 0);
    for (size_t i = 0; i < list.size(); i++) {
        const dect2::HfbState& s = t.beacons[i];
        if (!s.heard || s.lastHeard <= G.lastBeacon[i]) continue;
        G.lastBeacon[i] = s.lastHeard;
        if (s.conf < 2 && list[i].kind != dect2::hfb::Carrier) continue;
        if (list[i].lat == 0 && list[i].lon == 0) continue;
        io::Obs o;
        o.utc = (int64_t)s.lastHeard; o.fHz = list[i].khz * 1e3; o.id = list[i].name;
        o.src = list[i].kind == dect2::hfb::Carrier ? io::Source::TimeStation : io::Source::Beacon;
        o.snrDb = std::isnan(s.snr) ? 0.f : s.snr;
        o.rxLat = lat; o.rxLon = lon; o.txLat = list[i].lat; o.txLon = list[i].lon;
        add(o);
    }
    maybeSave();
}

void view(App& a) {
    ensureLoaded();
    const int64_t now = nowUtc();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("What this receiver has actually heard, kept in a local file (30 days, 4 MB) and never uploaded: FT8 / FT4 / WSPR stations with a locator and beacons "
                                 "with a known place. 'Not heard' only means something on bands you were listening to."));
    ImGui::PopTextWrapPos();
    if (!receiverLocationSet()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Set your position (ADS-B antenna position) first: the paths start there."));
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::Combo("##iowin", &G.window, "last 24 h\0last 7 days\0last 30 days\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::SliderInt("##iomin", &G.minDecodes, 1, 5, "min %d");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Decodes needed in a distance bin before a band counts as heard there (for the estimate)."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export CSV"))) {
        const std::string p = plat::dataDir() + "/ionosphere-observations.csv";
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << io::exportCsv(G.obs);
        G.note = f ? "saved " + p : "could not write " + p;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Forget all"))) { G.obs.clear(); G.seen.clear(); G.dirty = true; maybeSave(true); }
    if (!G.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", G.note.c_str()); }

    const std::vector<io::Obs> v = io::inWindow(G.obs, now, (io::Window)G.window);
    ImGui::Text("%zu receptions in the window, %zu in the log", v.size(), G.obs.size());
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Heard, per band and UTC hour (colour: best SNR)"));
    gridView(io::heardGrid(v), now);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Reception map (paths coloured by SNR red to green, shaded: night, orange line: terminator, yellow dot: sub-solar point)"));
    mapView(v, now, std::max(160 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
    ImGui::Spacing();

    // the estimate per distance next to the model's MUF; the model is evaluated at the receiver, today's date, the current sun
    dect2::prop::Params p;
    double lat = 0, lon = 0; receiverLocation(lat, lon);
    struct tm g; dect2::gmTime((time_t)now, &g);
    p.year = g.tm_year + 1900; p.month = g.tm_mon + 1; p.day = g.tm_mday;
    p.txLat = lat; p.txLon = lon;
    const int curHour = io::utcHourOf(now);
    auto mufAt = [&](double km, int) { dect2::prop::Params q = p; q.utcHour = curHour + 0.5; return dect2::prop::mufMHz(lat, lon, km, q); };
    const io::MufRow mr = io::mufEstimate(v, G.minDecodes);
    const io::PredObs po = io::compare(v, mufAt, -1);
    ImGui::TextDisabled("%s", tr("MUF estimate per distance = the highest band with decodes there (an observation, not a model), and the model's MUF now"));
    if (ImGui::BeginTable("##iomuf", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("distance"); ImGui::TableSetupColumn("observed MUF"); ImGui::TableSetupColumn("model MUF now"); ImGui::TableSetupColumn("bands heard");
        ImGui::TableHeadersRow();
        for (int d = 0; d < io::kDistBins; d++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(io::distBinName(d));
            ImGui::TableNextColumn(); if (mr.mufMHz[d] > 0) ImGui::Text(">= %.1f MHz", mr.mufMHz[d]); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f MHz", mufAt(io::distBinMidKm(d), curHour));
            ImGui::TableNextColumn();
            std::string bs;
            for (int b = 0; b < io::kBands; b++) if (mr.bandCount[d][b] >= std::max(1, G.minDecodes)) bs += std::string(bs.empty() ? "" : " ") + io::bandName(b);
            ImGui::TextUnformatted(bs.empty() ? "-" : bs.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Predicted vs observed (green: agree, orange: heard but not predicted, blue: predicted open but not heard, grey: no data)"));
    if (ImGui::BeginTable("##iopo", io::kDistBins + 1, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("");
        for (int d = 0; d < io::kDistBins; d++) ImGui::TableSetupColumn(io::distBinName(d));
        ImGui::TableHeadersRow();
        for (int b = io::kBands - 1; b >= 0; b--) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(io::bandName(b));
            for (int d = 0; d < io::kDistBins; d++) {
                ImGui::TableNextColumn();
                const io::Verdict vd = po.v[b][d];
                const ImU32 c = vd == io::Verdict::BothOpen ? IM_COL32(60, 170, 90, 255) : vd == io::Verdict::BothClosed ? IM_COL32(60, 110, 70, 255)
                              : vd == io::Verdict::ObservedOnly ? IM_COL32(230, 150, 50, 255) : vd == io::Verdict::PredictedOnly ? IM_COL32(70, 120, 200, 255)
                              : IM_COL32(60, 60, 64, 255);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, c);
                ImGui::Text(" ");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s: %s", io::bandName(b), io::distBinName(d), io::verdictName(vd));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("agree %d, disagree %d (all the window's receptions against the model for this hour, today's date and a default solar flux)", po.agree, po.disagree);
    (void)a;
    maybeSave();
}

} // namespace ionoobs_ui
