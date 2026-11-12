// AIS: the channel plan (AIS 1 + 2 and the long-range channels 75 + 76: one wide capture, or a hop plan for a narrow radio) and the Satellite AIS
// panel: satellite mode, the receiving satellite's elements, its passes over the area of interest with auto-arming, the map of ships heard per
// pass (the "eavesdropper" diagram), and the statistics of Li, Chen, Chen (Acta Astronautica 2017, doi:10.1016/j.actaastro.2017.02.014):
// detections per pass, per-vessel last seen, new-type (message 27) vessels and their ratio, the message-type mix, SNR against the satellite's
// elevation, detection probability per month; every table exports as CSV. docs/SATELLITE_AIS.md.
#include "app.h"
#include "ais_sat_ui.h"
#include "state/results_export.h"
#include "../adsb/adsb_map.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_sat.h"
#include "dect2/geo/passes.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

struct SatState {
    bool loaded = false;
    bool sat = false, applied = false, appliedSat = false;   // satellite mode wanted / pushed to the receiver
    bool hop = false; int hopStep = 0; double hopAt = 0;      // the retune plan of a narrow radio
    int view = 0;                                             // 0 map, 1 passes, 2 statistics, 3 vessels
    // the receiving satellite
    int norad = 0;
    dect2::Tle tle; bool haveTle = false; std::string tleMsg;
    std::vector<dect2::Tle> store;                            // what the TLE files hold (Passes window downloads, pasted elements)
    double storeAt = -1;
    char filter[32] = "AIS";
    // passes over the area of interest (the receiver position)
    std::vector<dect2::Pass> passes; double passesAt = 0; float minEl = 0;
    bool autoArm = false, inPass = false, weStarted = false; double passAos = 0, passLos = 0;
    double fileStartUnix = 0;                                  // a recording's start time (0 = the computer's clock)
    double appliedBase = -1;
    // the analysis
    dect2::AisSatStats stats;
    uint64_t statsSeq = 0;
    std::vector<dect2::AisMonthStat> months; std::vector<dect2::AisSnrBin> snr; double cacheAt = -1; uint64_t cacheN = ~0ull;
    adsbmap::View map;
    int selPass = -1;                                          // the pass the map shows (-1 all)
    std::string note;
};
SatState S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.sat = d.getI("aisSatMode", 0) != 0;
    S.norad = (int)d.getI("aisSatNorad", 0);
    S.autoArm = d.getI("aisSatArm", 0) != 0;
    S.hop = d.getI("aisHop", 0) != 0;
    S.minEl = (float)d.getD("aisSatMinEl", 0.0);
    S.map.zoom = 2; S.map.lat = 20; S.map.lon = 0;
}

std::string readText(const std::string& p) {
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// every TLE the pass planner keeps (plat::dataDir()/tle: the downloaded groups, custom sources and the pasted ones)
void loadStore() {
    if (S.storeAt >= 0 && ImGui::GetTime() - S.storeAt < 30) return;
    S.storeAt = ImGui::GetTime();
    S.store.clear();
    std::error_code ec;
    const std::filesystem::path dir(reinterpret_cast<const char8_t*>((plat::dataDir() + "/tle").c_str()));
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".txt" && ext != ".tle") continue;
        for (const dect2::Tle& t : dect2::parseTleText(readText(e.path().string()))) S.store.push_back(t);
    }
    std::sort(S.store.begin(), S.store.end(), [](const dect2::Tle& a, const dect2::Tle& b) { return a.name < b.name; });
    S.store.erase(std::unique(S.store.begin(), S.store.end(), [](const dect2::Tle& a, const dect2::Tle& b) { return a.norad == b.norad; }), S.store.end());
}

void pickSatellite(App& a, int norad) {
    S.norad = norad;
    plat::prefs().setI("aisSatNorad", norad);
    S.haveTle = false;
    S.passesAt = 0;
    for (const dect2::Tle& t : S.store) if (t.norad == norad) { S.tle = t; S.haveTle = true; }
    S.tleMsg = S.haveTle ? "" : "no elements for this NORAD number in the TLE store (View > Passes: download, or paste them)";
    if (S.haveTle) { S.stats.setSatellite(S.tle); a.engine.ais().setSatelliteTle(&S.tle); }
    else { S.stats.clearSatellite(); a.engine.ais().setSatelliteTle(nullptr); }
}

std::string utcText(double t) {
    if (t <= 0) return "-";
    const time_t s = (time_t)t;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &g);
    return b;
}

double nowUnix() { return (double)time(nullptr); }

// a CSV text (as AisSatStats writes it) into the shared export table
resexp::Table tableOf(const std::string& csv) {
    resexp::Table t;
    std::vector<std::string> row; std::string cell; bool q = false, first = true;
    auto endRow = [&] { row.push_back(cell); cell.clear(); if (first) { t.cols = row; first = false; } else t.add(row); row.clear(); };
    for (size_t i = 0; i < csv.size(); i++) {
        const char c = csv[i];
        if (q) { if (c == '"') { if (i + 1 < csv.size() && csv[i + 1] == '"') { cell += '"'; i++; } else q = false; } else cell += c; continue; }
        if (c == '"') q = true;
        else if (c == ',') { row.push_back(cell); cell.clear(); }
        else if (c == '\n') endRow();
        else if (c != '\r') cell += c;
    }
    if (!cell.empty() || !row.empty()) endRow();
    return t;
}

void refreshCache() {
    if (S.cacheN == S.stats.receptions() && ImGui::GetTime() - S.cacheAt < 10) return;
    S.cacheN = S.stats.receptions(); S.cacheAt = ImGui::GetTime();
    S.months = S.stats.months();
    S.snr = S.stats.snrVsElevation(10);
}

// ---------------------------------------------------------------- the eavesdropper map

void mapView(App& a, ImVec2 size) {
    (void)a;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    // the satellite's ground track during the chosen pass (or the last one)
    const auto& ps = S.stats.passes();
    const int pi = S.selPass >= 0 && S.selPass < (int)ps.size() ? S.selPass : (int)ps.size() - 1;
    if (S.haveTle && pi >= 0) {
        dect2::Sgp4 g;
        if (g.init(S.tle)) {
            const auto& p = ps[(size_t)pi];
            ImVec2 prev; bool have = false;
            for (double t = p.startUtc - 300; t <= std::max(p.endUtc, p.startUtc + 60) + 300; t += 20) {
                double la, lo, al;
                if (!dect2::subPoint(g, t, la, lo, al)) break;
                const ImVec2 q = adsbmap::project(la, lo);
                if (have && std::fabs(q.x - prev.x) < size.x * 0.5f) dl->AddLine(prev, q, IM_COL32(240, 200, 80, 200), 2.f);
                prev = q; have = true;
            }
        }
    }
    int shown = 0, lr = 0;
    for (const auto& kv : S.stats.vessels()) {
        const dect2::AisSatVessel& v = kv.second;
        if (!v.hasPos) continue;
        if (pi >= 0 && S.selPass >= 0 && !std::binary_search(ps[(size_t)pi].vessels.begin(), ps[(size_t)pi].vessels.end(), v.mmsi)) continue;
        const ImVec2 q = adsbmap::project(v.lat, v.lon);
        const bool nt = v.longRangeMsgs > 0;
        const float r = (2.5f + std::min(4.f, (float)v.passes)) * gUi;
        dl->AddCircleFilled(q, r, nt ? IM_COL32(245, 150, 60, 230) : IM_COL32(90, 160, 240, 220));
        if (nt) dl->AddCircle(q, r + 2.5f * gUi, IM_COL32(245, 150, 60, 200), 16, 1.2f);
        shown++; lr += nt;
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    adsbmap::tileControls("never the position itself");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(size.x, "%d ships (%d new-type, ringed orange, sent message 27); size: passes heard; yellow: the satellite's track", shown, lr);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

// ---------------------------------------------------------------- passes, auto-arming

void passesView(App& a) {
    loadStore();
    ImGui::TextDisabled("Receiving satellite");
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputText("##aissatf", S.filter, sizeof S.filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * gUi);
    const std::string cur = S.haveTle ? S.tle.name + " (" + std::to_string(S.tle.norad) + ")" : S.norad ? std::to_string(S.norad) : "none";
    if (ImGui::BeginCombo("##aissat", cur.c_str())) {
        if (ImGui::Selectable("none", S.norad == 0)) { S.norad = 0; S.haveTle = false; S.stats.clearSatellite(); a.engine.ais().setSatelliteTle(nullptr); plat::prefs().setI("aisSatNorad", 0); }
        std::string f = S.filter; for (char& c : f) c = (char)toupper((unsigned char)c);
        for (const dect2::Tle& t : S.store) {
            std::string n = t.name; for (char& c : n) c = (char)toupper((unsigned char)c);
            if (!f.empty() && n.find(f) == std::string::npos && std::to_string(t.norad) != S.filter) continue;
            if (ImGui::Selectable((t.name + "  " + std::to_string(t.norad)).c_str(), t.norad == S.norad)) pickSatellite(a, t.norad);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The satellite the IQ comes from (an AIS payload whose samples you have). Its elements come from the TLE store of the Passes window; type part of the name or the NORAD number above to filter.");
    if (S.norad && !S.haveTle) { pickSatellite(a, S.norad); }
    if (!S.tleMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.tleMsg.c_str()); ImGui::PopTextWrapPos(); }
    if (!S.haveTle) return;
    double lat, lon;
    receiverLocation(lat, lon);
    ImGui::TextDisabled("area of interest: %.3f, %.3f (the receiver position%s)", lat, lon, receiverLocationSet() ? "" : ", not set");
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::SliderFloat("minimum elevation##aissat", &S.minEl, 0.f, 40.f, "%.0f deg")) { plat::prefs().setD("aisSatMinEl", S.minEl); S.passesAt = 0; S.stats.setMinElevDeg(S.minEl); }
    const double now = nowUnix();
    if (now - S.passesAt > 600) {
        S.passesAt = now;
        dect2::Sgp4 g;
        S.passes.clear();
        if (g.init(S.tle)) {
            dect2::Observer o; o.latDeg = lat; o.lonDeg = lon;
            S.passes = dect2::findPasses(g, o, now - 1800, now + 86400, S.minEl);
        }
    }
    if (ImGui::Checkbox("Arm the passes (start AIS in satellite mode at AOS, stop at LOS)", &S.autoArm)) plat::prefs().setI("aisSatArm", S.autoArm);
    if (ImGui::BeginTable("##aispasses", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("AOS (UTC)", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("LOS"); ImGui::TableSetupColumn("max el"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const dect2::Pass& p : S.passes) {
            if (p.los < now) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(p.aos).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(p.los).substr(11).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.maxElDeg);
            ImGui::TableNextColumn();
            if (p.aos <= now) ImGui::TextColored(pal::okGreen(), "%s", S.inPass ? "receiving" : "now");
        }
        if (S.passes.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("no pass in the next 24 hours"); }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- statistics

void statsView() {
    refreshCache();
    const dect2::AisSatStats& st = S.stats;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(200 * gUi); ImGui::Text(fmt, v...); };
    kv("receptions", "%llu", (unsigned long long)st.receptions());
    kv("vessels heard", "%zu in %zu passes", st.vessels().size(), st.passes().size());
    kv("new-type vessels", "%u (%.1f %%) sent message 27", st.newTypeCount(), 100 * st.newTypeRatio());
    kv("still sending 1 - 3 / 18 / 19", "%.1f %% of the new-type vessels", 100 * st.newTypeStillCommonShare());
    ImGui::Spacing();
    const float half = ImGui::GetContentRegionAvail().x * 0.5f - 6 * gUi;
    ImGui::BeginChild("##aisst_l", ImVec2(half, ImGui::GetContentRegionAvail().y));
    ImGui::TextDisabled("Detections per pass");
    if (ImGui::BeginTable("##aisst_p", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pass (UTC)", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("msgs"); ImGui::TableSetupColumn("27");
        ImGui::TableSetupColumn("ships"); ImGui::TableSetupColumn("new"); ImGui::TableSetupColumn("SNR");
        ImGui::TableHeadersRow();
        for (int i = (int)st.passes().size() - 1; i >= 0; i--) {
            const dect2::AisSatPass& p = st.passes()[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(utcText(p.startUtc).c_str(), S.selPass == i, ImGuiSelectableFlags_SpanAllColumns)) S.selPass = S.selPass == i ? -1 : i;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%u", p.detections);
            ImGui::TableNextColumn(); ImGui::Text("%u", p.longRange);
            ImGui::TableNextColumn(); ImGui::Text("%zu", p.vessels.size());
            ImGui::TableNextColumn(); ImGui::Text("%u", p.newTypeVessels);
            ImGui::TableNextColumn(); if (p.detections) ImGui::Text("%.1f", p.snrSum / p.detections); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Per month (detection probability: ships heard / ships in the footprint with elements, else / passes x ships)");
    if (ImGui::BeginTable("##aisst_m", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Month", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("passes"); ImGui::TableSetupColumn("ships");
        ImGui::TableSetupColumn("new-type"); ImGui::TableSetupColumn("P(detect)");
        ImGui::TableHeadersRow();
        for (const dect2::AisMonthStat& m : S.months) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%04d-%02d", m.year, m.month);
            ImGui::TableNextColumn(); ImGui::Text("%u", m.passes);
            ImGui::TableNextColumn(); ImGui::Text("%u", m.vessels);
            ImGui::TableNextColumn(); ImGui::Text("%u (%.0f %%)", m.newType, m.vessels ? 100.0 * m.newType / m.vessels : 0.0);
            ImGui::TableNextColumn(); ImGui::Text("%.2f%s", m.detectionProb, m.fromFootprint ? "" : "*");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##aisst_r", ImVec2(0, ImGui::GetContentRegionAvail().y));
    ImGui::TextDisabled("Message types");
    const auto mix = st.typeMix();
    uint64_t tot = 0; for (uint64_t v : mix) tot += v;
    if (ImGui::BeginTable("##aisst_t", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("count"); ImGui::TableSetupColumn("share");
        ImGui::TableHeadersRow();
        for (int k = 1; k <= 28; k++) {
            const int ti = k == 28 ? 0 : k;
            if (!mix[(size_t)ti]) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); if (ti) ImGui::Text("%d", ti); else ImGui::TextUnformatted("28 and up");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)mix[(size_t)ti]);
            ImGui::TableNextColumn(); ImGui::Text("%.1f %%", tot ? 100.0 * (double)mix[(size_t)ti] / (double)tot : 0.0);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("SNR against the satellite's elevation seen from the ship (dB, 10 deg bins)");
    if (!S.haveTle) ImGui::TextDisabled("needs the satellite's elements (Passes)");
    else {
        float vals[9] = {}; int nb = 0; float lo = 1e9f, hi = -1e9f;
        for (const dect2::AisSnrBin& b : S.snr) { if (nb < 9) { vals[nb] = (float)b.snrMeanDb; if (b.n) { lo = std::min(lo, vals[nb]); hi = std::max(hi, vals[nb]); } nb++; } }
        if (hi < lo) { lo = 0; hi = 1; }
        ImGui::PlotHistogram("##aissnr", vals, nb, 0, "0 ... 90 deg", std::min(0.f, lo), hi + 2, ImVec2(ImGui::GetContentRegionAvail().x, 90 * gUi));
        for (const dect2::AisSnrBin& b : S.snr) if (b.n) ImGui::TextDisabled("%2.0f - %2.0f deg: %.1f dB (%u)", b.elevLo, b.elevHi, b.snrMeanDb, b.n);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Export (CSV or JSON by the file name)");
    { const resexp::Table t = tableOf(st.receptionsCsv()); resexp::saveButton(t, "ais_sat_receptions", S.note, "aisx1"); }
    ImGui::SameLine(); ImGui::TextUnformatted("receptions");
    { const resexp::Table t = tableOf(st.passesCsv()); resexp::saveButton(t, "ais_sat_passes", S.note, "aisx2"); }
    ImGui::SameLine(); ImGui::TextUnformatted("passes");
    { const resexp::Table t = tableOf(st.vesselsCsv()); resexp::saveButton(t, "ais_sat_vessels", S.note, "aisx3"); }
    ImGui::SameLine(); ImGui::TextUnformatted("vessels");
    { const resexp::Table t = tableOf(st.monthsCsv()); resexp::saveButton(t, "ais_sat_months", S.note, "aisx4"); }
    ImGui::SameLine(); ImGui::TextUnformatted("months");
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::SmallButton("clear the statistics")) { S.stats.clear(); S.selPass = -1; S.cacheN = ~0ull; }
    ImGui::EndChild();
}

void vesselsView() {
    std::vector<const dect2::AisSatVessel*> rows;
    for (const auto& kv : S.stats.vessels()) rows.push_back(&kv.second);
    std::sort(rows.begin(), rows.end(), [](const auto* x, const auto* y) { return x->lastUtc > y->lastUtc; });
    if (ImGui::BeginTable("##aissatv", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("MMSI"); ImGui::TableSetupColumn("Last seen (UTC)", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("msgs");
        ImGui::TableSetupColumn("27"); ImGui::TableSetupColumn("passes"); ImGui::TableSetupColumn("Pos"); ImGui::TableSetupColumn("SNR");
        ImGui::TableHeadersRow();
        for (const auto* v : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%09u", v->mmsi);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(v->lastUtc).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", v->msgs);
            ImGui::TableNextColumn(); if (v->longRangeMsgs) ImGui::TextColored(ImVec4(0.96f, 0.6f, 0.25f, 1), "%u", v->longRangeMsgs); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d", v->passes);
            ImGui::TableNextColumn(); if (v->hasPos) ImGui::Text("%.2f, %.2f", v->lat, v->lon); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", v->lastSnrDb);
        }
        ImGui::EndTable();
    }
}

bool liveAis(const App& a) { return a.rx.standard == 15; }

} // namespace

// ---------------------------------------------------------------- the hooks ais_ui.cpp calls

void aisChannelControls(App& a, bool& retune) {
    load();
    const DeviceInfo& dv = a.devices[a.devIdx];
    const double rate = a.tune.sampleRate;
    const int mask = dect2::ais::aisChannelsCovered(a.freqMhz * 1e6, rate);
    ImGui::TextDisabled("Channels in the capture: %s%s%s%s", mask & 1 ? "AIS 1 " : "", mask & 2 ? "AIS 2 " : "", mask & 4 ? "75 " : "", mask & 8 ? "76" : "");
    if (ImGui::SmallButton("AIS 1 + 2 (162.000)")) { a.freqMhz = 162.0; retune = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("75 + 76 (156.800)")) { a.freqMhz = 156.8; retune = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("all four (159.400)")) { a.freqMhz = dect2::ais::kAisWideCentreHz / 1e6; retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One capture of %.1f Msps or more holds AIS 1, AIS 2 and the long-range channels 75 and 76 (message 27)", dect2::ais::aisWideRateHz() / 1e6);
    const bool narrow = dv.maxRateHz > 0 && dv.maxRateHz < dect2::ais::aisWideRateHz();
    if (narrow || S.hop) {
        if (ImGui::Checkbox("Hop: AIS 1 + 2 for 1 min, 75 + 76 for 3 min 20 s", &S.hop)) { plat::prefs().setI("aisHop", S.hop); S.hopAt = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "For a radio that cannot hold all four channels: message 27 comes every 3 minutes, so a dwell a little longer than that hears every class A ship in range once");
    }
    if (ImGui::Checkbox("Satellite mode (Doppler +-4.5 kHz, collisions)", &S.sat)) plat::prefs().setI("aisSatMode", S.sat);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "For IQ received by a satellite in low earth orbit: wider channel filters, Doppler tracking, and decoding of bursts that land on top of each other (more CPU)");
}

void aisSatTick(App& a) {
    load();
    if (!S.applied || S.appliedSat != S.sat) { a.engine.ais().setSatelliteMode(S.sat); S.appliedSat = S.sat; S.applied = true; }
    const bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    const double base = isFile ? S.fileStartUnix : 0.0;
    if (base != S.appliedBase) { a.engine.ais().setTimeBase(base); S.appliedBase = base; }
    if (liveAis(a) && a.rx.ais.seq != S.statsSeq) { S.statsSeq = a.rx.ais.seq; S.stats.addAll(a.rx.ais.recent); }
    // the hop plan of a narrow radio
    if (S.hop && a.engine.running() && a.devices[a.devIdx].isRadio()) {
        const auto plan = dect2::ais::aisTunePlan(a.tune.sampleRate);
        if (plan.size() > 1) {
            const double t = ImGui::GetTime();
            if (S.hopAt == 0) S.hopAt = t;
            if (t - S.hopAt >= plan[(size_t)S.hopStep % plan.size()].dwellSec) {
                S.hopStep = (S.hopStep + 1) % (int)plan.size();
                S.hopAt = t;
                a.freqMhz = plan[(size_t)S.hopStep].centreHz / 1e6;
                a.tune.centerHz = plan[(size_t)S.hopStep].centreHz;
                a.engine.retune(a.tune);   // no reset: the stations stay
            }
        }
    }
    // auto-arming: the pass of the receiving satellite over the area of interest
    if (!S.autoArm || !S.haveTle) return;
    const double now = nowUnix();
    const dect2::Pass* cur = nullptr;
    for (const dect2::Pass& p : S.passes) if (now >= p.aos - 30 && now <= p.los) { cur = &p; break; }
    if (cur && !S.inPass) {
        S.inPass = true; S.passAos = cur->aos; S.passLos = cur->los; S.weStarted = false;
        S.stats.beginPass(S.tle.name, cur->aos, cur->los);
        a.engine.log("Satellite AIS: pass of " + S.tle.name + " begins");
        S.sat = true; plat::prefs().setI("aisSatMode", 1);
        if (!a.engine.running()) {
            if (a.family != 14) selectMode(a, 14);
            startReceiver(a);
            S.weStarted = a.engine.running();
        }
    } else if (S.inPass && now > S.passLos) {
        S.inPass = false;
        a.engine.log("Satellite AIS: pass of " + S.tle.name + " ends");
        if (S.weStarted && a.engine.running()) a.engine.stop();
        S.weStarted = false;
    }
}

void aisSatPanel(App& a) {
    load();
    subNav("aissatv", S.view, {"Map", "Passes", "Statistics", "Vessels"});
    {   // the state line
        const dect2::AisTelemetry& t = a.rx.ais;
        if (liveAis(a) && t.satellite)
            ImGui::TextDisabled("satellite mode: Doppler AIS 1 %+.0f Hz (%+.1f Hz/s), AIS 2 %+.0f Hz; %llu found on another frequency or part, %llu after cancelling", t.dopplerHz[0],
                                t.dopplerRateHzS[0], t.dopplerHz[1], (unsigned long long)t.recovered, (unsigned long long)t.cancelled);
        else ImGui::TextDisabled("%s", S.sat ? "satellite mode on (start the receiver)" : "satellite mode off: turn it on in the tuner panel for IQ from a satellite");
        if (a.devices[a.devIdx].kind == DeviceInfo::File) {
            ImGui::SetNextItemWidth(160 * gUi);
            ImGui::InputDouble("recording start, UNIX time (0 = now)##aisfs", &S.fileStartUnix, 0, 0, "%.0f");
        }
    }
    if (S.view == 0) mapView(a, ImGui::GetContentRegionAvail());
    else if (S.view == 1) passesView(a);
    else if (S.view == 2) statsView();
    else vesselsView();
}
