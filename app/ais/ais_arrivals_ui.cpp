// AIS arrivals board: the port the user picks (name, UN/LOCODE, other spellings, position) and the vessels heading there by ETA.
// Core: dect2/ais/ais_arrivals.h.
#include "app.h"
#include "ais/ais_seen_ui.h"
#include "dect2/ais/ais_arrivals.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <ctime>

namespace {
struct Arr {
    bool loaded = false;
    dect2::AisPort port;
    char name[48] = {}, locode[12] = {}, aliases[160] = {};
};
Arr R;
void save() { plat::prefs().setS("aisArrivalsPort", dect2::aisPortToText(R.port)); }
std::string when(int64_t t, int64_t now) {
    if (!t) return "-";
    const time_t tt = (time_t)t;
    struct tm l = *localtime(&tt);
    char b[40];
    std::strftime(b, sizeof b, "%a %d.%m %H:%M", &l);
    const double h = (double)(t - now) / 3600.0;
    char o[64];
    if (h < -0.1) std::snprintf(o, sizeof o, "%s (%.0f h late)", b, -h);
    else if (h < 48) std::snprintf(o, sizeof o, "%s (in %.1f h)", b, h);
    else std::snprintf(o, sizeof o, "%s", b);
    return o;
}
}  // namespace

void aisArrivalsView(App& a) {
    if (!R.loaded) {
        R.loaded = true;
        R.port = dect2::aisPortFromText(plat::prefs().getS("aisArrivalsPort", ""));
        std::snprintf(R.name, sizeof R.name, "%s", R.port.name.c_str());
        std::snprintf(R.locode, sizeof R.locode, "%s", R.port.locode.c_str());
        std::string al;
        for (const auto& x : R.port.aliases) { if (!al.empty()) al += ", "; al += x; }
        std::snprintf(R.aliases, sizeof R.aliases, "%s", al.c_str());
    }
    bool ch = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Port")); ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi); ch |= ImGui::InputTextWithHint("##aport", "Klaipeda", R.name, sizeof R.name);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputTextWithHint("##aloc", "LTKLJ", R.locode, sizeof R.locode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("UN/LOCODE: two letters for the country and three for the place, as many crews type it."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(200 * gUi); ch |= ImGui::InputTextWithHint("##aali", tr("other spellings, comma separated"), R.aliases, sizeof R.aliases);
    if (ch) {
        R.port.name = R.name; R.port.locode = R.locode; R.port.aliases.clear();
        std::string cur;
        for (const char* p = R.aliases;; p++) {
            if (*p == ',' || !*p) { size_t b = cur.find_first_not_of(' '), e = cur.find_last_not_of(' '); if (b != std::string::npos) R.port.aliases.push_back(cur.substr(b, e - b + 1)); cur.clear(); if (!*p) break; }
            else cur += *p;
        }
        save();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Port is here (receiver position)##aposr")) && receiverLocationSet()) { receiverLocation(R.port.lat, R.port.lon); R.port.hasPos = true; save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The port's position gives the distance and an ETA from each vessel's speed. Without it only the declared ETA is shown."));
    ImGui::SameLine();
    if (R.port.hasPos) ImGui::TextDisabled("%.4f, %.4f", R.port.lat, R.port.lon); else ImGui::TextDisabled("%s", tr("no position"));
    if (R.port.name.empty() && R.port.locode.empty()) { ImGui::TextDisabled("%s", tr("Type a port to see the vessels that give it as their destination.")); return; }
    const int64_t now = (int64_t)time(nullptr);
    const auto rows = dect2::aisArrivals(a.rx.ais.vessels, R.port, now);
    ImGui::TextDisabled(tr("%zu vessels heading for %s"), rows.size(), R.port.name.empty() ? R.port.locode.c_str() : R.port.name.c_str());
    if (!ImGui::BeginTable("##aisarr", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("ETA (declared)")); ImGui::TableSetupColumn(tr("ETA (speed)")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Speed"));
    ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Draught")); ImGui::TableSetupColumn(tr("Destination as sent"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(when(r.etaDeclared, now).c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", when(r.etaWorked, now).c_str());
        ImGui::TableNextColumn(); if (r.distKm >= 0) ImGui::Text("%.1f km", r.distKm); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (r.sog >= 0) ImGui::Text("%.1f kn", r.sog); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%s", r.name.empty() ? std::to_string(r.mmsi).c_str() : r.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.type.empty() ? "-" : r.type.c_str());
        ImGui::TableNextColumn(); if (r.draughtM > 0) ImGui::Text("%.1f m", r.draughtM); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.destination.c_str());
    }
    ImGui::EndTable();
}
