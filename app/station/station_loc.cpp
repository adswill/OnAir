// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "station/station_loc.h"
#include "app.h"
#include "dect2/geo/station_location.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/scale.h"
#include "imgui.h"
#include <cctype>
#include <cstdio>

namespace stationloc {

namespace {

// Screens that kept their own locator text before there was a global one: the preference holding it, and the flag that it is their own
struct OwnKey { const char* pref; const char* own; };
const OwnKey kOwn[] = {{"pskrLocator", "pskrLocatorOwn"}, {"wsprNetLocator", "wsprNetLocatorOwn"}, {"winlinkGrid", "winlinkGridOwn"}, {"ham.grid", "ham.gridOwn"}};

}

void migrate() {
    plat::Prefs& p = plat::prefs();
    if (p.getB("stationMigrated", false)) return;
    const bool have = p.has("adsbLat") && p.has("adsbLon") && !p.getB("adsbLocCleared", false) && dect2::station::isSetLatLon(p.getD("adsbLat", 0), p.getD("adsbLon", 0));
    if (!have) {
        std::vector<dect2::station::Candidate> c;
        c.push_back({"uatLat", p.getD("uatLat", 0), p.getD("uatLon", 0), ""});
        c.push_back({"rfiLat", p.getD("rfiLat", 0), p.getD("rfiLon", 0), ""});
        c.push_back({"setupLoc", 0, 0, p.getS("setupLoc", "")});
        for (const OwnKey& k : kOwn) c.push_back({k.pref, 0, 0, p.getS(k.pref, "")});
        const dect2::station::Pick pk = dect2::station::firstNonEmpty(c);
        if (pk.found) {
            p.setD("adsbLat", pk.lat); p.setD("adsbLon", pk.lon); p.setB("adsbLocCleared", false);
        }
    }
    const double la = p.getD("adsbLat", 0), lo = p.getD("adsbLon", 0);
    const bool set = p.has("adsbLat") && dect2::station::isSetLatLon(la, lo) && !p.getB("adsbLocCleared", false);
    for (const OwnKey& k : kOwn) {
        const std::string v = p.getS(k.pref, "");
        p.setB(k.own, !v.empty() && dect2::station::isOwnLocator(v, set ? la : 0, set ? lo : 0));   // a different saved locator stays that screen's own
    }
    p.setB("stationMigrated", true);
    p.flush();
}

std::string locator() {
    if (!receiverLocationSet()) return std::string();
    double la, lo;
    receiverLocation(la, lo);
    return dect2::station::locatorOf(la, lo, 6);
}

std::string name() { return plat::prefs().getS("stationName", ""); }

bool setFromText(App& a, const std::string& text) {
    double la, lo;
    if (!dect2::station::resolveText(text, la, lo)) return false;
    setReceiverLocation(a, la, lo);
    propagate(a);
    return true;
}

void propagate(App& a) {
    const std::string g = locator();
    if (g.empty()) return;
    plat::Prefs& p = plat::prefs();
    for (const OwnKey& k : kOwn) if (!p.getB(k.own, false)) p.setS(k.pref, g);
    if (!p.getB("pskrLocatorOwn", false)) a.pskr.locator = g;
    if (!p.getB("wsprNetLocatorOwn", false)) a.wsprNet.locator = g;
}

bool locatorRow(const char* label, std::string& value, const char* ownKey, float width) {
    plat::Prefs& p = plat::prefs();
    const std::string g = locator();
    bool changed = false;
    const bool own = p.getB(ownKey, false) || g.empty();
    if (!own) {
        if (value != g) { value = g; changed = true; }
        ImGui::TextUnformatted(tr(label));
        ImGui::SameLine();
        ImGui::TextDisabled("%s - %s", value.c_str(), tr("using the global location"));
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string(tr("change")) + "##" + ownKey).c_str())) settingsOpenPage("Location");
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string(tr("use another")) + "##o" + ownKey).c_str())) p.setB(ownKey, true);
        return changed;
    }
    ImGui::SetNextItemWidth(width * gUi);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    if (ImGui::InputText((std::string(tr(label)) + "##" + ownKey).c_str(), buf, sizeof buf, ImGuiInputTextFlags_CharsUppercase) && value != buf) { value = buf; changed = true; }
    if (!g.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string(tr("use the global location")) + " (" + g + ")##g" + ownKey).c_str())) { p.setB(ownKey, false); value = g; changed = true; }
    }
    return changed;
}

}
