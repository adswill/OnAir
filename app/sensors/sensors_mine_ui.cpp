// "Mine" view of the Sub-GHz sensors (3.9, docs/SMART5-iot-3.9.md): the user marks their own meters and tyre sensors; only those are followed.
// Meters: use per hour and day from the register (dect2/sensors/meter_usage.h). Tyres: pressure at 20 C, slow leak, low, one tyre off
// (dect2/sensors/tpms_watch.h). The marks are kept in the settings (sensMine: one "key|vehicle|position" per line); the usage and pressure
// history start when the app starts (not kept between sessions).
#include "app.h"
#include "dect2/sensors/meter_usage.h"
#include "dect2/sensors/tpms_watch.h"
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace {
struct Mark { std::string vehicle, position; };
std::map<std::string, Mark> gMarks; bool gLoaded = false;
std::map<std::string, dect2::MeterUsage> gMeters;
std::map<std::string, std::string> gMeterUnit;
dect2::TpmsWatch gTyres;
std::map<std::string, int64_t> gFedUtc;   // the newest reading fed, per sensor

void loadMarks() {
    if (gLoaded) return;
    gLoaded = true;
    std::istringstream in(plat::prefs().getS("sensMine", ""));
    for (std::string l; std::getline(in, l);) {
        std::string k, v, p; std::istringstream ls(l); std::getline(ls, k, '|'); std::getline(ls, v, '|'); std::getline(ls, p, '|');
        if (!k.empty()) gMarks[k] = {v, p};
    }
}
void saveMarks() {
    std::string s; for (const auto& [k, m] : gMarks) s += k + "|" + m.vehicle + "|" + m.position + "\n";
    plat::prefs().setS("sensMine", s); plat::prefs().flush();
}
int tzOffset() { const time_t t = time(nullptr); struct tm lt{}; struct tm gt{};
#ifdef _WIN32
    localtime_s(&lt, &t); gmtime_s(&gt, &t);
#else
    localtime_r(&t, &lt); gmtime_r(&t, &gt);
#endif
    return (int)(mktime(&lt) - mktime(&gt)); }
const dect2::SensorSeries* series(const dect2::SensorDevice& d, const char* key) { for (const auto& s : d.history) if (s.key == key) return &s; return nullptr; }
bool isTpms(const dect2::SensorDevice& d) { return d.kind == "tpms"; }
bool isMeter(const dect2::SensorDevice& d) { double v; std::string u; return d.kind == "meter" || dect2::sensorMeterRegister(d, v, u); }
}

void sensorsMineTick(const dect2::SensorsTelemetry& t) {
    loadMarks();
    for (const auto& d : t.devices) {
        auto mk = gMarks.find(d.key);
        if (mk == gMarks.end()) continue;
        int64_t& fed = gFedUtc[d.key];
        if (isTpms(d)) {
            if (!gTyres.isMine(d.key)) gTyres.mark(d.key, mk->second.vehicle.empty() ? "my vehicle" : mk->second.vehicle, mk->second.position);
            double scale = 1; const dect2::SensorSeries* p = series(d, "pressure_kPa");
            if (!p) { p = series(d, "pressure_PSI"); scale = 6.894757; }
            if (!p) { p = series(d, "pressure_bar"); scale = 100; }
            const dect2::SensorSeries* tc = series(d, "temperature_C");
            if (!p) continue;
            for (const auto& x : p->pts) {
                if (x.utc <= fed) continue;
                double temp = NAN;
                if (tc) for (auto it = tc->pts.rbegin(); it != tc->pts.rend(); ++it) if (std::llabs(it->utc - x.utc) <= 2) { temp = it->v; break; }
                gTyres.add(d.key, x.utc, x.v * scale, temp);
                fed = x.utc;
            }
        } else {
            double v = 0; std::string unit;
            if (!dect2::sensorMeterRegister(d, v, unit)) continue;
            dect2::MeterUsage& m = gMeters[d.key]; m.tzOffsetSec = tzOffset(); gMeterUnit[d.key] = unit;
            for (const char* key : {"total_kWh", "energy_kWh", "cumulative_kWh", "reading_kWh", "consumption_data", "consumption", "total_m3", "volume_m3", "total_L"}) {
                const dect2::SensorSeries* s = series(d, key);
                if (!s) continue;
                for (const auto& x : s->pts) if (x.utc > fed) { m.add(x.utc, x.v); fed = x.utc; }
                break;
            }
        }
    }
}

void sensorsMineView(const dect2::SensorsTelemetry& t) {
    loadMarks();
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Mark your own meters and tyre sensors. Only marked sensors are followed: passing cars and the neighbours' meters are never tracked. The figures start when the app starts."));
      ImGui::PopTextWrapPos(); }
    const int64_t now = (int64_t)time(nullptr);
    if (ImGui::BeginTable("##sensmine", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f))) {
        ImGui::TableSetupColumn(tr("Mine")); ImGui::TableSetupColumn(tr("Sensor")); ImGui::TableSetupColumn(tr("Vehicle / position")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& d : t.devices) {
            if (!isTpms(d) && !isMeter(d)) continue;
            ImGui::TableNextRow(); ImGui::PushID(d.key.c_str());
            ImGui::TableNextColumn();
            bool mine = gMarks.count(d.key) != 0;
            if (ImGui::Checkbox("##m", &mine)) { if (mine) gMarks[d.key] = {isTpms(d) ? "my car" : "", ""}; else { gMarks.erase(d.key); gTyres.unmark(d.key); gMeters.erase(d.key); gFedUtc.erase(d.key); } saveMarks(); }
            ImGui::TableNextColumn(); ImGui::Text("%s %s", d.model.c_str(), d.id.c_str());
            ImGui::TableNextColumn();
            if (mine && isTpms(d)) {
                Mark& mk = gMarks[d.key]; char v[48], p[48]; snprintf(v, sizeof v, "%s", mk.vehicle.c_str()); snprintf(p, sizeof p, "%s", mk.position.c_str());
                ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputText("##v", v, sizeof v, ImGuiInputTextFlags_EnterReturnsTrue)) { mk.vehicle = v; saveMarks(); gTyres.unmark(d.key); gFedUtc.erase(d.key); }
                ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputText("##p", p, sizeof p, ImGuiInputTextFlags_EnterReturnsTrue)) { mk.position = p; saveMarks(); gTyres.unmark(d.key); gFedUtc.erase(d.key); }
            } else ImGui::TextDisabled("%s", isTpms(d) ? "TPMS" : tr("meter"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%lld s ago", (long long)(now - d.lastSeen));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const auto& [k, m] : gMeters) {
        const std::string& u = gMeterUnit[k];
        ImGui::Text("%s: %s %.2f %s, %s %.2f %s, %s %.0f", k.c_str(), tr("today"), m.today(now), u.c_str(), tr("daily average"), m.dailyAverage(now), u.c_str(), tr("register"), m.lastValue());
        if (m.resets()) { ImGui::SameLine(); ImGui::TextDisabled("(%d %s)", m.resets(), tr("register resets")); }
        std::vector<float> h; for (const auto& b : m.hourly(now, 48)) h.push_back((float)b.use);
        ImGui::PlotHistogram(("##mh" + k).c_str(), h.data(), (int)h.size(), 0, tr("use per hour, last 48 h"), 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 70 * gUi));
    }
    for (const auto& [k, ty] : gTyres.tyres()) {
        if (!ty.readings) continue;
        const std::string name = ty.position.empty() ? ty.id : ty.position;
        ImGui::Text("%s / %s: %.0f kPa (%.0f kPa at 20 C)", ty.vehicle.c_str(), name.c_str(), ty.lastKpa, ty.lastCorrKpa);
        if (ty.hasTrend) { ImGui::SameLine(); ImGui::TextDisabled("%+.1f kPa/day over %.1f days", ty.kpaPerDay, ty.trendDays); }
        if (!ty.text.empty()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%s", ty.text.c_str());
    }
}
