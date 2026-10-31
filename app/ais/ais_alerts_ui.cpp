// AIS: station filter, CPA / TCPA and geofence alerts (core/include/dect2/ais/ais_alerts.h, docs/AIS_ALERTS.md).
// The alerts are off until the user ticks "Close-approach alerts" or writes a geofence. The position CPA is measured from is the receiver position
// (Settings > Receiver position); a moving own ship can be entered under Advanced. The alerts reach the automations as ais.cpa and ais.geofence.
#include "app.h"
#include "ais_alerts_ui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace {

struct State {
    bool loaded = false;
    dect2::AisFilter filter;
    bool cpaOn = false;
    float cpaNm = 0.5f, tcpaMin = 15.f, cooldownMin = 10.f;
    bool ownMoving = false; float ownSog = 0, ownCog = 0;
    char fences[2048] = "";
    std::vector<std::string> fenceErrors;
    bool dirty = true;
    double lastPush = 0;
    double lastLat = 1e9, lastLon = 1e9;
};
State S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.filter = dect2::AisFilter::fromText(d.getS("aisFilter", ""));
    S.cpaOn = d.getB("aisCpaOn", false);
    S.cpaNm = (float)std::max(0.01, std::min(50.0, d.getD("aisCpaNm", 0.5)));
    S.tcpaMin = (float)std::max(0.5, std::min(240.0, d.getD("aisTcpaMin", 15)));
    S.cooldownMin = (float)std::max(0.5, std::min(1440.0, d.getD("aisCpaCooldownMin", 10)));
    S.ownMoving = d.getB("aisOwnMoving", false);
    S.ownSog = (float)d.getD("aisOwnSog", 0); S.ownCog = (float)d.getD("aisOwnCog", 0);
    const std::string f = d.getS("aisFences", "");
    snprintf(S.fences, sizeof S.fences, "%s", f.c_str());
    S.dirty = true;
}

void save() {
    plat::Prefs& d = plat::prefs();
    d.setS("aisFilter", S.filter.toText());
    d.setB("aisCpaOn", S.cpaOn); d.setD("aisCpaNm", S.cpaNm); d.setD("aisTcpaMin", S.tcpaMin); d.setD("aisCpaCooldownMin", S.cooldownMin);
    d.setB("aisOwnMoving", S.ownMoving); d.setD("aisOwnSog", S.ownSog); d.setD("aisOwnCog", S.ownCog);
    d.setS("aisFences", S.fences);
}

dect2::AisAlertConfig makeConfig() {
    dect2::AisAlertConfig c;
    c.cpaOn = S.cpaOn;
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    c.ownValid = receiverLocationSet();
    c.ownLat = lat; c.ownLon = lon;
    c.ownSogKn = S.ownMoving ? S.ownSog : 0; c.ownCogDeg = S.ownMoving ? S.ownCog : 0;
    c.cpaNm = S.cpaNm; c.tcpaMin = S.tcpaMin; c.cooldownSec = S.cooldownMin * 60.0;
    c.filter = S.filter;
    S.fenceErrors.clear();
    c.fences = dect2::aisParseGeofences(S.fences, &S.fenceErrors);
    return c;
}

void push(App& a) {
    a.engine.ais().setAlertConfig(makeConfig());
    S.dirty = false;
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    S.lastLat = lat; S.lastLon = lon;
}

std::string clock(double t) {
    if (t <= 0) return "-";
    const time_t s = (time_t)t;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

} // namespace

const dect2::AisFilter& aisUiFilter() { load(); return S.filter; }

bool aisFilterBar(App& a) {
    load();
    bool changed = false;
    char lab[48];
    snprintf(lab, sizeof lab, "%s##aisflt", S.filter.active() ? tr("Filter (on)") : tr("Filter"));
    if (ImGui::SmallButton(lab)) ImGui::OpenPopup("##aisfltpop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show only some stations: Class A, Class B, base stations, aids to navigation, and groups of ship types"));
    if (ImGui::BeginPopup("##aisfltpop")) {
        ImGui::TextDisabled("%s", tr("Station class"));
        for (int c = 0; c < 6; c++) {
            bool on = S.filter.classes >> c & 1;
            if (ImGui::Checkbox(dect2::aisClassText(c), &on)) { S.filter.classes = on ? (S.filter.classes | 1u << c) : (S.filter.classes & ~(1u << c)); changed = true; }
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Ship type (ships without a type heard yet are \"Not received\")"));
        for (int g = 0; g < dect2::AIS_GRP_COUNT; g++) {
            bool on = S.filter.groups >> g & 1;
            if (ImGui::Checkbox(dect2::aisShipGroupText(g), &on)) { S.filter.groups = on ? (S.filter.groups | 1u << g) : (S.filter.groups & ~(1u << g)); changed = true; }
        }
        ImGui::Separator();
        changed |= ImGui::Checkbox(tr("Moving only (0.5 kn or more)"), &S.filter.movingOnly);
        if (ImGui::SmallButton(tr("Show everything"))) { S.filter = dect2::AisFilter(); changed = true; }
        ImGui::EndPopup();
    }
    if (changed) { save(); S.dirty = true; }
    (void)a;
    return changed;
}

void aisAlertsTick(App& a) {
    load();
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    if (S.dirty || lat != S.lastLat || lon != S.lastLon) push(a);
}

void aisAlertsLoadPrefs(App& a) {
    load();
    push(a);
}

void aisAlertsPanel(App& a) {
    load();
    bool ch = false;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Alerts go to Automations as ais.cpa and ais.geofence (notification, sound, webhook, recording ...). Both are off until you turn them on."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ch |= ImGui::Checkbox(tr("Close-approach alerts (CPA)"), &S.cpaOn);
    if (S.cpaOn) {
        ImGui::Indent();
        if (!receiverLocationSet()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.96f, 0.6f, 0.25f, 1), "%s", tr("Set the receiver position first (Settings): the closest approach is measured to it.")); ImGui::PopTextWrapPos(); }
        ch |= ImGui::SliderFloat(tr("closer than (nm)"), &S.cpaNm, 0.05f, 5.f, "%.2f");
        ch |= ImGui::SliderFloat(tr("within (minutes)"), &S.tcpaMin, 1.f, 60.f, "%.0f");
        ImGui::Unindent();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Geofences"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("One per line:  name;lat,lon,radius_nm   or   name;poly;lat,lon lat,lon lat,lon ..."));
    ImGui::PopTextWrapPos();
    ch |= ImGui::InputTextMultiline("##aisfences", S.fences, sizeof S.fences, ImVec2(-1, 5.f * ImGui::GetTextLineHeightWithSpacing()));
    if (!S.fenceErrors.empty()) for (const std::string& e : S.fenceErrors) ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1), "%s", e.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Alerts use the station filter (the Filter button above the table): only the stations it shows raise them."));
    if (ImGui::TreeNode(trw("Advanced"))) {
        ch |= ImGui::SliderFloat(tr("same ship again after (minutes)"), &S.cooldownMin, 1.f, 120.f, "%.0f");
        ch |= ImGui::Checkbox(tr("my own ship is moving"), &S.ownMoving);
        if (S.ownMoving) {
            ch |= ImGui::SliderFloat(tr("speed (kn)"), &S.ownSog, 0.f, 40.f, "%.1f");
            ch |= ImGui::SliderFloat(tr("course (deg)"), &S.ownCog, 0.f, 359.f, "%.0f");
        }
        ImGui::TreePop();
    }
    if (ch) { save(); S.dirty = true; push(a); }
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Recent alerts"));
    const dect2::AisTelemetry& t = a.rx.ais;
    if (t.alerts.empty()) { ImGui::TextDisabled("%s", tr("none yet")); return; }
    if (ImGui::BeginTable("##aisalerts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("UTC"));
        ImGui::TableSetupColumn(tr("Alert"));
        ImGui::TableSetupColumn(tr("Ship"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto it = t.alerts.rbegin(); it != t.alerts.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(it->utc).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->kind == "cpa" ? "CPA" : it->kind == "enter" ? "enter" : "exit");
            ImGui::TableNextColumn(); ImGui::Text("%s (%09u)", it->name.empty() ? "-" : it->name.c_str(), it->mmsi);
            ImGui::TableNextColumn();
            if (it->kind == "cpa") ImGui::Text("%.2f nm in %.0f min, now %.1f nm", it->dcpaNm, it->tcpaMin, it->distNm);
            else ImGui::Text("%s", it->fence.c_str());
        }
        ImGui::EndTable();
    }
}
