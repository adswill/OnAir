// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The theoretical range outline on the ADS-B map: imported from heywhatsthat (upintheair.json) or worked out from the elevation files of the
// propagation tool. Core: dect2/adsb/adsb_horizon.h.
#include "adsb_internal.h"
#include "dect2/adsb/adsb_horizon.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace adsbui {

namespace {
struct Hz {
    bool loaded = false, show = true;
    int source = 0;                 // 0 none, 1 file, 2 worked out
    std::string file, msg;
    float aglM = 10;
    std::vector<dect2::HorizonRing> rings;
};
Hz H;

bool loadFile(const std::string& path) {
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    std::string err;
    std::vector<dect2::HorizonRing> r;
    if (!dect2::parseUpintheair(ss.str(), r, err)) { H.msg = err; return false; }
    H.rings = std::move(r);
    H.msg = std::to_string(H.rings.size()) + " rings from " + path;
    return true;
}

void workOut() {
    dect2::prop::Terrain t;
    std::string errs;
    const std::string tf = plat::prefs().getS("prop_terrain", "");   // the elevation files of the propagation tool (Tools > Propagation, Terrain)
    std::stringstream ss(tf);
    std::string f;
    while (std::getline(ss, f)) if (!f.empty()) { std::string e; if (!dect2::prop::loadElevationFile(f, t, e)) errs += f + ": " + e + "; "; }
    H.rings = dect2::terrainHorizon(t, S.refLat, S.refLon, H.aglM, {3048, 6096, 12192});
    H.msg = t.empty() ? std::string(tr("no elevation files (add them in Propagation, Terrain): worked out over sea level")) : t.describe();
    if (!errs.empty()) H.msg += "  " + errs;
}

void load() {
    if (H.loaded) return;
    H.loaded = true;
    plat::Prefs& p = plat::prefs();
    H.source = (int)p.getI("adsbHorizonSrc", 0);
    H.file = p.getS("adsbHorizonFile", "");
    H.aglM = (float)p.getD("adsbHorizonAgl", 10);
    H.show = p.getB("adsbHorizonShow", true);
    if (H.source == 1 && !H.file.empty()) loadFile(H.file);
    else if (H.source == 2) workOut();
}
}  // namespace

void horizonControls() {
    load();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Theoretical range (terrain horizon)"));
    if (ImGui::Checkbox(tr("Show on the map##hz"), &H.show)) plat::prefs().setB("adsbHorizonShow", H.show);
    if (ImGui::Button(tr("Import upintheair.json...##hz"))) {
        const std::string p = openFileDialog();
        if (!p.empty() && loadFile(p)) { H.source = 1; H.file = p; plat::prefs().setI("adsbHorizonSrc", 1); plat::prefs().setS("adsbHorizonFile", p); }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The file heywhatsthat.com makes for your panorama (the tar1090 range outline). Download it in your browser; OnAir only reads it."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Work out from elevation files##hz"))) { workOut(); H.source = 2; plat::prefs().setI("adsbHorizonSrc", 2); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("From the SRTM / GeoTIFF files you added in the propagation tool, at the antenna position, for 10 000, 20 000 and 40 000 ft."));
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputFloat(tr("Antenna above ground (m)##hz"), &H.aglM, 1, 5, "%.0f")) { H.aglM = std::max(0.f, std::min(1000.f, H.aglM)); plat::prefs().setD("adsbHorizonAgl", H.aglM); }
    if (H.source && ImGui::SmallButton(tr("Remove##hz"))) { H.source = 0; H.rings.clear(); H.msg.clear(); plat::prefs().setI("adsbHorizonSrc", 0); }
    if (!H.msg.empty()) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360 * gUi); ImGui::TextDisabled("%s", H.msg.c_str()); ImGui::PopTextWrapPos(); }
}

void drawHorizon(ImDrawList* dl) {
    load();
    if (!H.show || H.rings.empty()) return;
    const ImU32 col[3] = {IM_COL32(240, 150, 70, 190), IM_COL32(120, 200, 120, 190), IM_COL32(90, 170, 240, 190)};
    for (size_t i = 0; i < H.rings.size(); i++) {
        const auto& r = H.rings[i];
        std::vector<ImVec2> pts;
        for (const auto& p : r.pts) pts.push_back(adsbmap::project(p.first, p.second));
        if (pts.size() < 3) continue;
        // dashed: every other segment
        for (size_t k = 0; k < pts.size(); k++) if (k % 2 == 0) dl->AddLine(pts[k], pts[(k + 1) % pts.size()], col[i % 3], 1.5f * gUi);
        char b[32]; std::snprintf(b, sizeof b, "%.0f ft", r.altM / 0.3048);
        dl->AddText(pts[0], col[i % 3], b);
    }
}

}  // namespace adsbui
