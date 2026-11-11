// Radiosonde launch sites in the app: kept in <data folder>/stats/sonde-sites.txt, the Sites view, the landing dots on the map.
// Core: dect2/sonde/sonde_sites.h.
#include "app.h"
#include "adsb/adsb_map.h"
#include "dect2/sonde/sonde_sites.h"
#include "dect2/sonde/sonde_launch_clock.h"
#include "i18n/i18n.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

void sondeSitesTick(App& a, bool live);
void sondeSitesView(App& a);
void sondeSitesDraw(ImDrawList* dl);

namespace {
namespace fs = std::filesystem;
struct Ss { bool loaded = false, show = true; dect2::SondeSites st; double last = 0; char rename[40] = {}; int renameId = 0; };
Ss G;
fs::path file() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/stats").c_str())) / "sonde-sites.txt"; }
void load() {
    if (G.loaded) return;
    G.loaded = true;
    std::ifstream f(file(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    G.st.parse(ss.str());
    G.show = plat::prefs().getB("sondeSitesShow", true);
}
void save() {
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    std::ofstream o(file(), std::ios::binary | std::ios::trunc);
    o << G.st.serialize();
    G.st.clearDirty();
}
}  // namespace

void sondeSitesTick(App& a, bool live) {
    load();
    if (!live || ImGui::GetTime() - G.last < 10) return;
    G.last = ImGui::GetTime();
    G.st.update(a.rx.sonde.sondes, (int64_t)time(nullptr));
    if (G.st.dirty()) save();
}

void sondeSitesDraw(ImDrawList* dl) {
    load();
    if (!G.show) return;
    for (const auto& s : G.st.sites()) {
        const ImVec2 c = adsbmap::project(s.lat, s.lon);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 7 * gUi), ImVec2(c.x - 6 * gUi, c.y + 5 * gUi), ImVec2(c.x + 6 * gUi, c.y + 5 * gUi), IM_COL32(120, 200, 255, 200));
        dl->AddText(ImVec2(c.x + 8 * gUi, c.y - 7 * gUi), IM_COL32(200, 230, 255, 220), s.name.c_str());
        for (const auto& l : s.landings) dl->AddCircleFilled(adsbmap::project(l.first, l.second), 4 * gUi, IM_COL32(255, 120, 60, 70), 12);   // a heat map of where they come down
    }
}

void sondeSitesView(App& a) {
    (void)a;
    load();
    if (ImGui::Checkbox(tr("Sites and landings on the map##sondesites"), &G.show)) plat::prefs().setB("sondeSitesShow", G.show);
    ImGui::TextDisabled("%s", tr("A site is where sondes are first heard low and climbing (within 5 km is the same site). A landing is the last position after a burst, below 1500 m."));
    const auto& v = G.st.sites();
    if (v.empty()) { ImGui::TextDisabled("%s", tr("No launch seen yet.")); return; }
    if (!ImGui::BeginTable("##sondesitest", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn(tr("Site")); ImGui::TableSetupColumn(tr("Position")); ImGui::TableSetupColumn(tr("Launches")); ImGui::TableSetupColumn(tr("Landings"));
    ImGui::TableSetupColumn(tr("Usually lands")); ImGui::TableSetupColumn(tr("Next launch (UTC)")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& s : v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(s.id);
        if (ImGui::Selectable(s.name.c_str(), false)) { G.renameId = s.id; std::snprintf(G.rename, sizeof G.rename, "%s", s.name.c_str()); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to rename"));
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::Text("%.3f, %.3f", s.lat, s.lon);
        ImGui::TableNextColumn(); ImGui::Text("%d", s.launches);
        ImGui::TableNextColumn(); ImGui::Text("%zu", s.landings.size());
        ImGui::TableNextColumn();
        if (!s.landings.empty()) {   // the mean landing: distance and bearing from the site
            double la = 0, lo = 0;
            for (const auto& l : s.landings) { la += l.first; lo += l.second; }
            la /= s.landings.size(); lo /= s.landings.size();
            const double r = 3.14159265358979 / 180, dy = (la - s.lat) * 111.2, dx = (lo - s.lon) * 111.2 * std::cos(s.lat * r);
            ImGui::Text("%.0f km at %.0f deg", std::hypot(dx, dy), std::fmod(std::atan2(dx, dy) / r + 360, 360));
        } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        {   // the launch clock: when this site usually sends a sonde, and "tune now" inside the window
            const int64_t now = (int64_t)std::time(nullptr);
            const auto p = dect2::predictNextLaunch(s.launchTimes, now);
            if (!p.valid) ImGui::TextDisabled("%s", tr("learning"));
            else {
                const int sod = (int)(p.nextUtc % 86400); char b[24]; std::snprintf(b, sizeof b, "%02d:%02d", sod / 3600, sod / 60 % 60);
                if (dect2::launchWindowOpen(p, now)) ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), "%s  %s", b, tr("now: tune in"));
                else ImGui::Text("%s  (+/- %d min)", b, p.spreadMin / 2 + 1);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Learned from the launches this station heard: the usual time of day of this site's sondes. Tune in about 15 minutes before."));
        }
        ImGui::TableNextColumn(); { const time_t t = (time_t)s.lastUtc; struct tm l = *localtime(&t); char b[24]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &l); ImGui::TextUnformatted(b); }
    }
    ImGui::EndTable();
    if (G.renameId) {
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::InputText("##sondesiter", G.rename, sizeof G.rename, ImGuiInputTextFlags_EnterReturnsTrue)) { G.st.rename(G.renameId, G.rename); save(); G.renameId = 0; }
        ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Enter to rename"));
    }
}
