// ADS-B overhead log (planefence): the Overhead view, the "now overhead" line, the day's CSV in the data folder. Core: dect2/adsb/adsb_overhead.h.
#include "adsb_internal.h"
#include "dect2/adsb/adsb_overhead.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace adsbui {

namespace {
namespace fs = std::filesystem;
struct Oh {
    bool loaded = false;
    dect2::AdsbOverheadLog log;
    float radiusKm = 3.0f;
    int maxAltFt = 0;
    std::string note;
    double lastUpdate = 0;
};
Oh O;

int tzOff() {
    const time_t t = time(nullptr);
    struct tm l = *localtime(&t), g = *gmtime(&t);
    l.tm_isdst = 0; g.tm_isdst = 0;
    return (int)difftime(mktime(&l), mktime(&g));
}
fs::path dir() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/adsb-overhead").c_str())); }
fs::path dayFile(const std::string& day) { return dir() / (day + ".csv"); }

void load() {
    if (O.loaded) return;
    O.loaded = true;
    O.radiusKm = (float)plat::prefs().getD("adsbOverheadKm", 3.0);
    O.maxAltFt = (int)plat::prefs().getI("adsbOverheadAltFt", 0);
    O.log.setRadiusKm(O.radiusKm);
    O.log.setMaxAltFt(O.maxAltFt);
    std::ifstream f(dayFile(dect2::adsbOverheadDay((int64_t)time(nullptr), tzOff())), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    for (const auto& p : dect2::adsbOverheadFromCsv(ss.str())) O.log.add(p);
}

void save(const std::vector<dect2::AdsbOverheadPass>& ended) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    for (const auto& p : ended) {
        const fs::path fp = dayFile(dect2::adsbOverheadDay(p.closestUtc, tzOff()));
        const bool fresh = !fs::exists(fp, ec);
        std::string csv = dect2::adsbOverheadCsv({p});
        if (!fresh) csv = csv.substr(csv.find('\n') + 1);   // no second header
        std::ofstream o(fp, std::ios::binary | std::ios::app);
        o << csv;
    }
}

std::string clock(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm l = *localtime(&tt);
    char b[16]; std::strftime(b, sizeof b, "%H:%M:%S", &l);
    return b;
}
}  // namespace

void overheadTick(App& a) {
    load();
    if (!live(a) || ImGui::GetTime() - O.lastUpdate < 1.0) return;
    O.lastUpdate = ImGui::GetTime();
    const auto ended = O.log.update(a.rx.adsb.aircraft, (int64_t)time(nullptr), [](const AdsbAircraft& ac) {
        dect2::AdsbOverheadInfo in; in.type = typeOf(ac); in.registration = registrationOf(ac.icao); return in;
    });
    if (!ended.empty()) save(ended);
}

void overheadBanner(App& a) {
    if (!live(a)) return;
    const auto now = O.log.now();
    if (now.empty()) return;
    std::string s;
    for (size_t i = 0; i < now.size() && i < 3; i++) {
        const auto& p = now[i];
        char b[160];
        std::snprintf(b, sizeof b, "%s%s %s %.1f km%s", i ? "   " : "", p.callsign.empty() ? icaoText(p.icao).c_str() : p.callsign.c_str(), p.type.c_str(), p.closestKm,
                      p.hasAlt ? (", " + uAlt(p.altFt)).c_str() : "");
        s += b;
    }
    ImGui::TextColored(kGood, "%s %s", tr("Overhead now:"), s.c_str());
}

void overheadView(App& a) {
    load();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Within")); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputFloat("km##adsboh", &O.radiusKm, 0.5f, 1.f, "%.1f")) { O.radiusKm = std::max(0.2f, std::min(200.f, O.radiusKm)); O.log.setRadiusKm(O.radiusKm); plat::prefs().setD("adsbOverheadKm", O.radiusKm); }
    ImGui::SameLine(); ImGui::TextUnformatted(tr("below")); ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt("ft##adsbohalt", &O.maxAltFt, 1000, 5000)) { O.maxAltFt = std::max(0, O.maxAltFt); O.log.setMaxAltFt(O.maxAltFt); plat::prefs().setI("adsbOverheadAltFt", O.maxAltFt); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("0: any altitude"));
    ImGui::SameLine();
    const std::string today = dect2::adsbOverheadDay((int64_t)time(nullptr), tzOff());
    std::vector<dect2::AdsbOverheadPass> rows;
    for (const auto& p : O.log.done()) if (dect2::adsbOverheadDay(p.closestUtc, tzOff()) == today) rows.push_back(p);
    if (ImGui::Button(tr("Save CSV...##adsboh"))) {
        const std::string p = saveFileDialog(("overhead-" + today + ".csv").c_str());
        if (!p.empty()) { std::ofstream o(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary); o << dect2::adsbOverheadCsv(rows); O.note = o ? tr("saved") : tr("could not write the file"); }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%zu today; every day is kept in %s"), rows.size(), (plat::dataDir() + "/adsb-overhead").c_str());
    if (!O.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", O.note.c_str()); }
    if (!receiverLocationSet()) ImGui::TextColored(kWarn, "%s", tr("Set the receiver position first (Tuner, Locate): distances are measured from it."));
    overheadBanner(a);
    if (!ImGui::BeginTable("##adsbohl", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Closest")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Altitude")); ImGui::TableSetupColumn(tr("Callsign"));
    ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Reg")); ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const auto& p = *it;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.closestUtc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.2f km", p.closestKm);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.ground ? tr("ground") : p.hasAlt ? uAlt(p.altFt).c_str() : "-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.callsign.empty() ? "-" : p.callsign.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.type.empty() ? "-" : p.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.registration.empty() ? "-" : p.registration.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(icaoText(p.icao).c_str());
    }
    ImGui::EndTable();
    if (rows.empty()) ImGui::TextDisabled("%s", tr("Nothing has come this close today."));
}

}  // namespace adsbui
