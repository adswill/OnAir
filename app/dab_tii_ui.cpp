// DAB: the Map tab (the transmitters). The receiver names the transmitters of the network it hears by their TII codes (MainId / SubId, EN 300 401
// clause 14.8); with a list of where each transmitter of an ensemble stands they go on the map, with the distance and bearing from the
// antenna. The list is a CSV file in OnAir's data folder (dab-transmitters.csv), one line per transmitter:
//   eid,main,sub,lat,lon,name,country,power_kw      (eid in hex, e.g. CE15; a line starting with # is a comment)
// TII codes are unique within an ensemble, not worldwide: a transmitter is found by (ensemble id, MainId, SubId).
#include "app.h"
#include "adsb_map.h"
#include "dect2/updater.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {

struct TxSite { double lat = 0, lon = 0, powerKw = 0; std::string name, country; };

struct State {
    bool loaded = false;
    std::map<uint64_t, TxSite> sites;   // key: eid << 16 | main << 8 | sub
    std::string file, loadMsg;
    adsbmap::View map;
    bool fitted = false;
    double refLat = 0, refLon = 0;
    bool haveRef = false;
};
State S;

uint64_t key(int eid, int mainId, int subId) { return ((uint64_t)(eid & 0xFFFF) << 16) | ((uint64_t)(mainId & 0xFF) << 8) | (uint64_t)(subId & 0xFF); }

std::vector<std::string> splitCsv(const std::string& line) {   // commas; a field in double quotes may hold commas
    std::vector<std::string> f;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == ',' && !q) { f.push_back(cur); cur.clear(); }
        else cur += c;
    }
    f.push_back(cur);
    for (auto& s : f) { while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back(); while (!s.empty() && s.front() == ' ') s.erase(0, 1); }
    return f;
}

void load() {
    S.loaded = true;
    S.sites.clear();
    S.file = plat::dataDir() + "/dab-transmitters.csv";
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(S.file.c_str())));
    if (!in) { S.loadMsg = "no transmitter list"; return; }
    std::string line;
    int lines = 0, bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = splitCsv(line);
        if (f.size() < 5 || f[0] == "eid") { if (f.size() < 5) bad++; continue; }
        char* e = nullptr;
        const long eid = std::strtol(f[0].c_str(), &e, 16);
        const int m = std::atoi(f[1].c_str()), sb = std::atoi(f[2].c_str());
        TxSite s;
        s.lat = std::atof(f[3].c_str()); s.lon = std::atof(f[4].c_str());
        if (f.size() > 5) s.name = f[5];
        if (f.size() > 6) s.country = f[6];
        if (f.size() > 7) s.powerKw = std::atof(f[7].c_str());
        if (e == f[0].c_str() || m < 0 || m > 69 || sb < 0 || sb > 23 || std::fabs(s.lat) > 90 || std::fabs(s.lon) > 180) { bad++; continue; }
        S.sites[key((int)eid, m, sb)] = s;
        lines++;
    }
    char b[160];
    snprintf(b, sizeof b, "%d transmitter(s) in the list%s", lines, bad ? ", some lines not understood" : "");
    S.loadMsg = b;
}

// great-circle distance (km) and initial bearing (degrees from north)
void distBearing(double la1, double lo1, double la2, double lo2, double& km, double& deg) {
    const double r = M_PI / 180.0, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    deg = std::fmod(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r + 360.0, 360.0);
}

} // namespace

void dabTransmittersTab(App& a) {
    if (!S.loaded) load();
    // the antenna position: the one the map modes share (set in the ADS-B tuner, or below)
    if (!S.haveRef) {
        S.refLat = plat::prefs().getD("adsbLat", 0); S.refLon = plat::prefs().getD("adsbLon", 0);
        S.haveRef = S.refLat != 0 || S.refLon != 0;
    }
    const DabTelemetry& d = a.rx.dab;
    const DabEnsemble ens = a.engine.dabEnsemble();
    const bool on = a.engine.running() && d.state == 2;

    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Every transmitter of a DAB network sends its own identification (TII: MainId / SubId) in the null symbol of every other frame. "
                          "OnAir lists the ones it hears; with a transmitter list it shows where they stand.");
      ImGui::PopTextWrapPos(); }

    // which of the heard ones are in the list
    struct Row { DabTelemetry::Tii t; const TxSite* site; double km = -1, deg = 0; };
    std::vector<Row> rows;
    if (on) for (const auto& t : d.tii) {
        Row r{t, nullptr};
        auto it = S.sites.find(key(ens.eid, t.mainId, t.subId));
        if (it != S.sites.end()) { r.site = &it->second; if (S.haveRef) distBearing(S.refLat, S.refLon, it->second.lat, it->second.lon, r.km, r.deg); }
        rows.push_back(r);
    }
    const float strongest = rows.empty() ? 0.f : rows.front().t.levelDb;

    // ---- the map (over the table), when there is something to put on it
    int located = 0;
    for (const auto& r : rows) if (r.site) located++;
    const float avail = ImGui::GetContentRegionAvail().y;
    if (located > 0 || S.haveRef) {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(160.f * gUi, avail * 0.55f));
        if (!S.fitted && located > 0) {   // centre on what was found, once
            double la = 0, lo = 0; int n = 0;
            for (const auto& r : rows) if (r.site) { la += r.site->lat; lo += r.site->lon; n++; }
            if (S.haveRef) { la += S.refLat; lo += S.refLon; n++; }
            S.map.lat = la / n; S.map.lon = lo / n; S.map.zoom = 8; S.fitted = true;
        } else if (!S.fitted && S.haveRef) { S.map.lat = S.refLat; S.map.lon = S.refLon; S.map.zoom = 8; }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        adsbmap::draw(S.map, size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
        const ImVec2 me = adsbmap::project(S.refLat, S.refLon);
        for (const auto& r : rows) {
            if (!r.site) continue;
            const ImVec2 q = adsbmap::project(r.site->lat, r.site->lon);
            // brighter and larger for a stronger transmitter (within 20 dB of the strongest)
            const float rel = std::max(0.f, std::min(1.f, 1.f + (r.t.levelDb - strongest) / 20.f));
            if (S.haveRef) dl->AddLine(me, q, IM_COL32(120, 200, 255, (int)(60 + 120 * rel)), 1.5f);
            dl->AddCircleFilled(q, (4.f + 4.f * rel) * gUi, IM_COL32(255, 170, 60, (int)(140 + 115 * rel)));
            char lab[96];
            snprintf(lab, sizeof lab, "%s  %d/%d", r.site->name.empty() ? "?" : r.site->name.c_str(), r.t.mainId, r.t.subId);
            dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), lab);
        }
        if (S.haveRef) {   // the antenna: a cross
            const float c = 6.f * gUi;
            dl->AddLine(ImVec2(me.x - c, me.y), ImVec2(me.x + c, me.y), IM_COL32(120, 200, 255, 255), 2.f);
            dl->AddLine(ImVec2(me.x, me.y - c), ImVec2(me.x, me.y + c), IM_COL32(120, 200, 255, 255), 2.f);
        }
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
        if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::Button("fit")) S.fitted = false;
        ImGui::SameLine(0, 8 * gUi);
        adsbmap::tileControls("never the position itself");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y - ImGui::GetTextLineHeightWithSpacing()));
        if (!on) adsbmap::legend(size.x, "start the receiver");
        else if (located == 0) adsbmap::legend(size.x, "no transmitter heard is in the list");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y + 4 * gUi));
    }

    // ---- the table
    if (!on) ImGui::TextDisabled(a.engine.running() ? "waiting for the ensemble..." : "start the receiver on a DAB channel");
    else if (rows.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(d.tiiFrames < 12 ? "listening for transmitter identification..." : "no transmitter identification in this signal (TII is optional; not every network sends it)"); ImGui::PopTextWrapPos(); }
    else if (ImGui::BeginTable("##dabtii", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                               ImVec2(0, std::max(5 * ImGui::GetTextLineHeightWithSpacing(), ImGui::GetContentRegionAvail().y - 3 * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("TII"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Transmitter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Bearing"); ImGui::TableSetupColumn("Power");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%02d / %02d", r.t.mainId, r.t.subId); ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("MainId %d (pattern), SubId %d (comb)\n%.1f dB above the noise of the null symbol", r.t.mainId, r.t.subId, r.t.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f dB", r.t.levelDb - strongest);
            ImGui::TableNextColumn();
            if (r.site) ImGui::Text("%s%s%s", r.site->name.c_str(), r.site->country.empty() ? "" : ", ", r.site->country.c_str());
            else ImGui::TextDisabled("not in the list");
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f km", r.km); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.km >= 0) ImGui::Text("%.0f°", r.deg); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (r.site && r.site->powerKw > 0) ImGui::Text("%.1f kW", r.site->powerKw); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    // ---- the list and the antenna position
    ImGui::Spacing();
    ImGui::TextDisabled("%s", S.loadMsg.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\none line per transmitter: eid,main,sub,lat,lon,name,country,power_kw\n(eid in hex, e.g. CE15; # starts a comment)", S.file.c_str());
    flowNext(10 * gUi);   // the row wraps in a narrow window
    if (ImGui::SmallButton("reload")) { load(); S.fitted = false; }
    flowNext(6 * gUi);
    if (ImGui::SmallButton("open folder")) dect2::openUrl(plat::dataDir());
    if (on) { flowNext(10 * gUi); ImGui::TextDisabled("ensemble id %04X", ens.eid); }
    flowEnd();
    if (!S.haveRef) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Distances and bearings: set the antenna position in the ADS-B tuner."); ImGui::PopTextWrapPos(); }
}
