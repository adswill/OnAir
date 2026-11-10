// ADS-B over days and years (graphs1090, Virtual Radar Server "first seen"): the store of every airframe ever heard, daily records, the all-time
// outline per altitude band, the adsb.first_ever event and the History view. Core: dect2/common/seen_store.h.
#include "adsb_internal.h"
#include "dect2/automation/engine.h"
#include "dect2/common/seen_store.h"
#include "i18n/i18n.h"
#include "adsb/records_ui.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace adsbui {

namespace {
namespace fs = std::filesystem;
struct Seen {
    bool loaded = false;
    dect2::SeenStore st;
    std::map<uint32_t, uint32_t> lastMsgs;   // messages counter per aircraft, for the daily total
    std::map<uint32_t, std::pair<double, double>> lastPos;
    double lastSave = 0;
    uint64_t lastSeq = 0;
    int chart = 30;
    std::string note;
};
Seen G;

int tzOff() {
    const time_t t = time(nullptr);
    struct tm l = *localtime(&t), g = *gmtime(&t);
    l.tm_isdst = 0; g.tm_isdst = 0;
    return (int)difftime(mktime(&l), mktime(&g));
}
fs::path file() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/stats").c_str())) / "adsb-seen.txt"; }
void save() {
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    const fs::path tmp = file().string() + ".tmp";
    { std::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << G.st.serialize(); if (!o) return; }
    fs::rename(tmp, file(), ec);
    G.st.clearDirty();
}
void load() {
    if (G.loaded) return;
    G.loaded = true;
    G.st.setTzOffset(tzOff());
    std::ifstream f(file(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    G.st.parse(ss.str());
    G.st.setTzOffset(tzOff());
}
std::string hexOf(uint32_t i) { char b[8]; std::snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }
int band(const AdsbAircraft& ac) { return !ac.hasAlt || ac.ground || ac.altFt < 10000 ? 0 : ac.altFt < 25000 ? 1 : 2; }
}  // namespace

bool seenNewToday(uint32_t icao) { return G.loaded && G.st.newToday(hexOf(icao), (int64_t)time(nullptr)); }

void seenTick(App& a) {
    load();
    const double now = ImGui::GetTime();
    if (G.st.dirty() && now - G.lastSave > 300) { G.lastSave = now; save(); }
    if (!live(a)) { if (G.st.dirty() && G.lastSeq) { save(); G.lastSeq = 0; } return; }
    if (a.rx.seq == G.lastSeq) return;
    G.lastSeq = a.rx.seq;
    const int64_t utc = (int64_t)time(nullptr);
    const bool settled = G.st.days().size() >= 2;   // the first day everything is new: no alerts until the store knows one full day
    for (const AdsbAircraft& ac : a.rx.adsb.aircraft) {
        if (ac.ageSec > 5 || (ac.icao & 0x1000000)) continue;   // fresh, real ICAO addresses only (not TIS-B track numbers)
        const std::string id = hexOf(ac.icao);
        if (G.st.seen(id, utc, ac.callsign) && settled && dect2::automation::wants(dect2::automation::FAdsb)) {
            dect2::automation::Event ev("adsb.first_ever");
            ev.at((double)utc, "ADS-B", 1090e6);
            ev.str("hex", id).str("callsign", ac.callsign).str("registration", registrationOf(ac.icao)).str("type", typeOf(ac)).str("operator", operatorOf(ac.icao));
            if (ac.hasAlt) ev.num("alt_ft", ac.altFt);
            if (ac.hasRange) ev.num("distance_km", ac.distNm * 1.852);
            ev.num("unique_ever", (double)G.st.uniqueEver());
            dect2::automation::emit(std::move(ev));
        }
        uint32_t& m = G.lastMsgs[ac.icao];
        if (ac.messages > m) { G.st.messages(utc, ac.messages - m); }
        m = ac.messages;
        if (ac.hasPos && ac.hasRange && ac.posAgeSec < 5) {
            auto& lp = G.lastPos[ac.icao];
            if (lp.first != ac.lat || lp.second != ac.lon) {
                lp = {ac.lat, ac.lon}; G.st.position(id, utc, ac.distNm * 1.852, ac.bearingDeg, band(ac));
                if (ac.posKind == 2 && !ac.ground) recordsOffer("ADS-B", "", ac.distNm * 1.852, ac.callsign.empty() ? id : ac.callsign + " " + id, ac.hasAlt ? std::to_string(ac.altFt) + " ft" : std::string());   // global fixes only
            }
        }
    }
    if (G.lastMsgs.size() > 20000) { G.lastMsgs.clear(); G.lastPos.clear(); }
}

void seenView(App& a) {
    load();
    const int64_t utc = (int64_t)time(nullptr);
    const dect2::SeenDay* t = G.st.today(utc);
    ImGui::Text(tr("Airframes ever heard: %zu   today: %u, of them new: %u   positions today: %llu   farthest today: %.0f km"), G.st.uniqueEver(), t ? t->unique : 0u, t ? t->fresh : 0u,
                (unsigned long long)(t ? t->positions : 0), t ? t->maxKm : 0.0);
    ImGui::TextDisabled(tr("Farthest ever: %.0f km (%s). Kept in %s, also after a restart."), G.st.farthestKm(), G.st.farthestId().c_str(), file().string().c_str());
    ImGui::RadioButton(tr("30 days"), &G.chart, 30); ImGui::SameLine(); ImGui::RadioButton(tr("365 days"), &G.chart, 365); ImGui::SameLine();
    if (ImGui::Button(tr("Save CSV...##adsbseen"))) {
        const std::string p = saveFileDialog("adsb-days.csv");
        if (!p.empty()) { std::ofstream o(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary); o << dect2::seenDaysCsv(G.st); G.note = o ? tr("saved") : tr("could not write the file"); }
    }
    if (!G.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", G.note.c_str()); }
    const auto& d = G.st.days();
    const size_t from = d.size() > (size_t)G.chart ? d.size() - (size_t)G.chart : 0;
    std::vector<float> uq, pos, far;
    for (size_t i = from; i < d.size(); i++) { uq.push_back((float)d[i].unique); pos.push_back((float)d[i].positions); far.push_back((float)d[i].maxKm); }
    const float w = ImGui::GetContentRegionAvail().x * 0.62f, h = 70 * gUi;
    recordsPanel();
    if (uq.empty()) { ImGui::TextDisabled("%s", tr("No days yet: the store fills while the receiver runs.")); return; }
    ImGui::BeginGroup();
    ImGui::PlotHistogram("##adsbsu", uq.data(), (int)uq.size(), 0, tr("unique aircraft per day"), 0, FLT_MAX, ImVec2(w, h));
    ImGui::PlotHistogram("##adsbsp", pos.data(), (int)pos.size(), 0, tr("positions per day"), 0, FLT_MAX, ImVec2(w, h));
    ImGui::PlotHistogram("##adsbsf", far.data(), (int)far.size(), 0, tr("farthest per day, km"), 0, FLT_MAX, ImVec2(w, h));
    ImGui::TextDisabled("%s .. %s", d[from].day.c_str(), d.back().day.c_str());
    ImGui::EndGroup();
    ImGui::SameLine();
    // the all-time outline: farthest position per 10 degree sector, one line per altitude band
    const float sz = std::min(ImGui::GetContentRegionAvail().x, 3 * h + 20 * gUi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##adsbsout", ImVec2(sz, sz));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + sz / 2, p0.y + sz / 2);
    float mx = 1;
    for (int b = 0; b < dect2::SeenStore::kBands; b++) for (float v : G.st.outline(b)) mx = std::max(mx, v);
    const float R = sz / 2 - 4 * gUi;
    for (int r = 1; r <= 3; r++) dl->AddCircle(c, R * r / 3, ImGui::GetColorU32(ImGuiCol_Border), 48);
    const ImU32 col[3] = {IM_COL32(120, 200, 120, 255), IM_COL32(90, 170, 240, 255), IM_COL32(240, 150, 70, 255)};
    for (int b = 0; b < dect2::SeenStore::kBands; b++) {
        const auto& o = G.st.outline(b);
        ImVec2 pts[dect2::SeenStore::kSectors];
        for (int i = 0; i < dect2::SeenStore::kSectors; i++) {
            const double ang = (i + 0.5) * 10.0 * 3.14159265358979 / 180.0;
            pts[i] = ImVec2(c.x + (float)std::sin(ang) * R * o[(size_t)i] / mx, c.y - (float)std::cos(ang) * R * o[(size_t)i] / mx);
        }
        dl->AddPolyline(pts, dect2::SeenStore::kSectors, col[b], ImDrawFlags_Closed, 1.6f * gUi);
    }
    char lg[96]; std::snprintf(lg, sizeof lg, "%.0f km  green < 10 000 ft, blue < 25 000, orange above", mx);
    dl->AddText(ImVec2(p0.x, p0.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), lg);
}

}  // namespace adsbui
