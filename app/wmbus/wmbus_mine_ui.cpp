// "Mine" view of wM-Bus (3.9, docs/SMART5-iot-3.9.md item 12, wired in docs/SMART5-UI-3.9.md): the user marks their own meters; only those are
// followed. The use per hour and day comes from the meter's current register (function 0, storage 0, Volume or Energy) through
// dect2/sensors/meter_usage.h, the same logic as the Sub-GHz sensors' Mine view. Encrypted meters show nothing until the user imports their key.
// The marks are kept in the settings (wmbusMine: one "manufacturer id" per line); the usage starts when the app starts.
#include "app.h"
#include "dect2/sensors/meter_usage.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <cfloat>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace {
std::set<std::string> gMine; bool gLoaded = false;
std::map<std::string, dect2::MeterUsage> gUse;
std::map<std::string, std::string> gUnit;
std::map<std::string, int64_t> gFed;

std::string keyOf(const dect2::WmbusDevice& d) { return d.mfr + " " + d.id; }
void load() {
    if (gLoaded) return;
    gLoaded = true;
    std::istringstream in(plat::prefs().getS("wmbusMine", ""));
    for (std::string l; std::getline(in, l);) if (!l.empty()) gMine.insert(l);
}
void save() { std::string s; for (const auto& k : gMine) s += k + "\n"; plat::prefs().setS("wmbusMine", s); plat::prefs().flush(); }
int tzOffset() { const time_t t = time(nullptr); struct tm lt{}; struct tm gt{};
#ifdef _WIN32
    localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
    localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
    return (int)(mktime(&lt) - mktime(&gt)); }
// the meter's running total: the current value of its volume or energy register
const dect2::wmbus::Record* reg(const dect2::WmbusDevice& d) {
    for (const auto& r : d.records)
        if (r.numeric && r.func == 0 && r.storage == 0 && r.tariff == 0 && (r.quantity == "Volume" || r.quantity == "Energy")) return &r;
    return nullptr;
}
std::string unitOf(const dect2::wmbus::Record& r) { const size_t p = r.value.rfind(' '); return p == std::string::npos ? std::string() : r.value.substr(p + 1); }
}

// every frame from the smart5 tick (app/smart5/smart5_common.cpp), whatever screen is open
void wmbusMineTick(App& a) {
    if (a.rx.standard != 87) return;
    load();
    if (gMine.empty()) return;
    for (const auto& d : a.rx.wmbus.devices) {
        const std::string k = keyOf(d);
        if (!gMine.count(k)) continue;
        const auto* r = reg(d);
        if (!r) continue;
        int64_t& fed = gFed[k];
        if (d.lastSeen <= fed) continue;
        fed = d.lastSeen;
        dect2::MeterUsage& m = gUse[k];
        m.tzOffsetSec = tzOffset();
        gUnit[k] = unitOf(*r);
        m.add(d.lastSeen, r->number);
    }
}

void wmbusMineView(App& a) {
    load();
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Mark your own meters. Only marked meters are followed: the neighbours' meters are never tracked. The figures start when the app starts."));
      ImGui::PopTextWrapPos(); }
    const int64_t now = (int64_t)time(nullptr);
    if (ImGui::BeginTable("##wmmine", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f))) {
        ImGui::TableSetupColumn(tr("Mine")); ImGui::TableSetupColumn(tr("Meter")); ImGui::TableSetupColumn(tr("Register")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& d : a.rx.wmbus.devices) {
            const std::string k = keyOf(d);
            ImGui::TableNextRow(); ImGui::PushID(k.c_str());
            ImGui::TableNextColumn();
            bool mine = gMine.count(k) != 0;
            if (ImGui::Checkbox("##m", &mine)) { if (mine) gMine.insert(k); else { gMine.erase(k); gUse.erase(k); gFed.erase(k); } save(); }
            ImGui::TableNextColumn(); ImGui::Text("%s %s", k.c_str(), d.type.c_str());
            ImGui::TableNextColumn();
            const auto* r = reg(d);
            if (r) ImGui::TextUnformatted(r->value.c_str()); else ImGui::TextDisabled("%s", d.encrypted ? tr("encrypted") : tr("no volume or energy register"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%lld s ago", (long long)(now - d.lastSeen));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const auto& [k, m] : gUse) {
        const std::string& u = gUnit[k];
        ImGui::Text("%s: %s %.3f %s, %s %.3f %s, %s %.3f", k.c_str(), tr("today"), m.today(now), u.c_str(), tr("daily average"), m.dailyAverage(now), u.c_str(), tr("register"), m.lastValue());
        if (m.resets()) { ImGui::SameLine(); ImGui::TextDisabled("(%d %s)", m.resets(), tr("register resets")); }
        std::vector<float> h; for (const auto& b : m.hourly(now, 48)) h.push_back((float)b.use);
        ImGui::PlotHistogram(("##wmh" + k).c_str(), h.data(), (int)h.size(), 0, tr("use per hour, last 48 h"), 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 70 * gUi));
    }
}
