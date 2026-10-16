// The receiver-range view shared by ADS-B and AIS: a polar plot (north up) of the furthest position heard in each 5 degree sector, rings in
// the display unit, a rolling-window choice, Reset, and export to PNG or CSV. The maths is dect2/rangeplot.h; this is only drawing.
#pragma once
#include "app.h"
#include "dect2/rangeplot.h"
#include "results_export.h"
#include "dect2/u8file.h"
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>

struct RangeView {
    dect2::RangePlot plot{5.0, 0.0, 1000.0};
    double refLat = 1e9, refLon = 1e9;   // the station position the plot was built for
    int win = 0;                         // 0 all time, 1 last 24 hours, 2 last hour
    std::string note;
};

inline double rangeNow() { return (double)std::time(nullptr); }

// the station moved: ranges from the old position mean nothing
inline void rangeSetStation(RangeView& v, double lat, double lon) {
    if (std::fabs(lat - v.refLat) > 1e-4 || std::fabs(lon - v.refLon) > 1e-4) { v.plot.reset(); v.refLat = lat; v.refLon = lon; }
}

// one heard position
inline void rangeFeed(RangeView& v, double lat, double lon) { v.plot.addPosition(v.refLat, v.refLon, lat, lon, rangeNow()); }

// unitPerKm: 1 for km, 0.5399568 for nautical miles; unit: its name; kind: "adsb" or "ais" (file names); positionSet: the receiver position was entered
inline void rangeView(RangeView& v, ImVec2 size, double unitPerKm, const char* unit, const char* kind, bool positionSet) {
    dect2::RangePlot& rp = v.plot;
    const double now = rangeNow();
    ImGui::BeginGroup();
    if (ImGui::Button(tr("Reset"))) { rp.reset(); v.note.clear(); }
    ImGui::SameLine();
    const bool any = rp.overallMaxKm(now) > 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button(tr("Save PNG..."))) {
        const std::string path = saveFileDialog((std::string("range-") + kind + "-" + resexp::stamp() + ".png").c_str());
        if (!path.empty()) {
            const std::vector<uint8_t> px = rp.renderRgba(1024, now, unitPerKm);
            v.note = (gfx::writePng(path.c_str(), px.data(), 1024, 1024) ? std::string(tr("saved")) : std::string(tr("could not write"))) + " " + path;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save CSV..."))) {
        const std::string path = saveFileDialog((std::string("range-") + kind + "-" + resexp::stamp() + ".csv").c_str());
        if (!path.empty()) {
            std::ofstream o(dect2::u8file(path), std::ios::binary);
            o << rp.toCsv(now);
            o.flush();
            v.note = (o ? std::string(tr("saved")) : std::string(tr("could not write"))) + " " + path;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        static const char* const kWin[3] = {N_("All time"), N_("Last 24 hours"), N_("Last hour")};
        static const double kSec[3] = {0, 86400, 3600};
        ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##rwin", tr(kWin[v.win]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kWin[i]), v.win == i)) { v.win = i; rp.setWindow(kSec[i]); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Rolling window: a sector's furthest position counts only for this long"));
    }
    if (!v.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", v.note.c_str()); }
    if (!positionSet) ImGui::TextColored(pal::warnAmber(), "%s", tr("Set the receiver position first: ranges are measured from it."));
    else if (!any) ImGui::TextDisabled("%s", tr("Nothing heard yet: the plot fills in as positions arrive."));
    else {
        char b[200];
        snprintf(b, sizeof b, tr("Longest range: %.0f %s at %.0f deg (%llu reports)"), rp.overallMaxKm(now) * unitPerKm, unit, rp.farthestAzDeg(now), (unsigned long long)rp.samples());
        ImGui::TextUnformatted(b);
    }
    ImGui::EndGroup();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(80.f, std::min(size.x, std::min(avail.x, avail.y)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##rangeplot", ImVec2(side, side));
    const float R = side * 0.5f - 14 * gUi;
    const ImVec2 c(p0.x + side * 0.5f, p0.y + side * 0.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(8, 9, 10, 255));
    const double maxU = rp.overallMaxKm(now) * unitPerKm, step = dect2::RangePlot::ringStep(maxU > 0 ? maxU : 10);
    const int nRings = std::max(1, (int)std::ceil(maxU / step - 1e-9));
    const double scaleU = nRings * step;
    const ImU32 grid = IM_COL32(60, 64, 66, 255), lab = IM_COL32(110, 114, 118, 255);
    for (int k = 1; k <= nRings; k++) {
        dl->AddCircle(c, R * k / nRings, grid, 96);
        char b[32]; snprintf(b, sizeof b, "%g %s", step * k, unit);
        dl->AddText(ImVec2(c.x + 3 * gUi, c.y - R * k / nRings), lab, b);
    }
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), grid);
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), grid);
    dl->AddText(ImVec2(c.x - 3 * gUi, p0.y), lab, "N");
    dl->AddText(ImVec2(p0.x + side - 10 * gUi, c.y - 6 * gUi), lab, "E");
    dl->AddText(ImVec2(c.x - 3 * gUi, p0.y + side - ImGui::GetTextLineHeight()), lab, "S");
    dl->AddText(ImVec2(p0.x + 2, c.y - 6 * gUi), lab, "W");
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(pal::accent(0.35f)), edge = ImGui::ColorConvertFloat4ToU32(pal::accent());
    const float k2 = (float)(R / scaleU), bd = (float)rp.bucketDeg();
    for (int b = 0; b < rp.buckets(); b++) {
        const double m = rp.maxKm(b, now) * unitPerKm;
        if (m <= 0) continue;
        ImVec2 pts[10];
        int n = 0;
        pts[n++] = c;
        const int seg = 6;
        for (int s = 0; s <= seg; s++) {
            const float az = (float)((b * bd + bd * s / seg) * 3.14159265 / 180);
            pts[n++] = ImVec2(c.x + (float)m * k2 * std::sin(az), c.y - (float)m * k2 * std::cos(az));
        }
        dl->AddConvexPolyFilled(pts, n, fill);
        dl->AddPolyline(pts + 1, n - 1, edge, 0, 1.5f);
    }
    dl->AddCircleFilled(c, 3 * gUi, edge);
}
