// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "photo_links_ui.h"
#include "photo_links.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "dect2/engine/updater.h"
#include "imgui.h"
#include <vector>

namespace photolinks {
namespace {
const char* const kPrefKey = "photoSite";

void items(std::vector<Link> v) {
    if (v.empty()) { ImGui::TextDisabled("%s", tr("No identifier to look up")); return; }
    const std::string pref = plat::prefs().getS(kPrefKey, "");
    preferFirst(v, pref);
    for (const Link& l : v) {
        const bool first = &l == &v[0] && !pref.empty() && pref == l.site;
        if (ImGui::MenuItem((std::string(l.site) + (first ? "  *" : "")).c_str())) dect2::openUrl(l.url);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Opens %s in your browser (nothing is loaded until you click)"), l.site);
    }
    if (ImGui::BeginMenu(tr("Preferred site"))) {
        for (const Link& l : v)
            if (ImGui::MenuItem(l.site, nullptr, pref == l.site)) plat::prefs().setS(kPrefKey, pref == l.site ? "" : l.site);
        ImGui::EndMenu();
    }
}
void button(const std::vector<Link>& v) {
    if (ImGui::SmallButton(tr("Photo / info"))) ImGui::OpenPopup("##photolinks");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pictures and details in a public database: opens your browser"));
    if (ImGui::BeginPopup("##photolinks")) { items(v); ImGui::EndPopup(); }
}
}  // namespace

void aircraftButton(uint32_t i, const std::string& r, const std::string& c) { button(aircraftLinks(i, r, c)); }
void aircraftMenuItems(uint32_t i, const std::string& r, const std::string& c) { if (ImGui::BeginMenu(tr("Photo / info"))) { items(aircraftLinks(i, r, c)); ImGui::EndMenu(); } }
void shipButton(uint32_t m, uint32_t imo, const std::string& n) { button(shipLinks(m, imo, n)); }
void shipMenuItems(uint32_t m, uint32_t imo, const std::string& n) { if (ImGui::BeginMenu(tr("Photo / info"))) { items(shipLinks(m, imo, n)); ImGui::EndMenu(); } }

}  // namespace photolinks
