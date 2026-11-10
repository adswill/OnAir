// One shared strip under the "what do I do next" line (modeActionBar): the next pass of a satellite mode, whether a geostationary satellite can be seen from
// this location, and later hints. Facts and one click, nothing is blocked. The logic is in ease_logic.h.
#include "../app.h"
#include "ease_logic.h"
#include "dect2/engine/mode_tuning.h"
#include "typography.h"
#include "imgui.h"
#include "dect2/vor/navaid_list.h"
#include "platform/platform.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

namespace {

int passFamilyOf(const App& a) {
    switch (a.family) {
        case 59: case 44: case 152: case 201: return a.family;   // Meteor LRPT, Orbcomm, Argos downlink, LoRa satellites
        case 25: if (a.sub == subIndexOf("sstv") && std::fabs(a.freqMhz - 145.8) < 0.1) return kPassFamilyIssSstv; return -1;   // SSTV on the ISS's frequency
        default: return -1;
    }
}

void passHint(App& a) {
    const int fam = passFamilyOf(a);
    if (fam < 0) return;
    static std::map<int, std::string> msg;
    bool armed = false;
    const std::string line = passesFamilyLine(a, fam, &armed);
    ImGui::TextDisabled("%s", line.c_str());
    ImGui::SameLine(0, 8 * gUi);
    ImGui::PushID("passhint");
    if (armed) ImGui::TextDisabled("%s", tr("armed"));
    else if (ImGui::SmallButton(tr("Follow the next pass"))) msg[fam] = passesArmFamily(a, fam);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Starts the receiver by itself at the rise, follows the Doppler and puts the receiver back at the set. Cancel it in View > Passes."));
    ImGui::PopID();
    if (auto it = msg.find(fam); it != msg.end() && !armed && !it->second.empty()) ImGui::TextDisabled("%s", it->second.c_str());
    if (fam == kPassFamilyIssSstv) ImGui::TextDisabled("%s", tr("The ISS sends SSTV only during announced events; check the ARISS news before you wait for a pass."));
}

// E14: near-me navaids (OurAirports navaids.csv, public domain; fetched once with a notice, cached). Community data, not for navigation.
struct NavS {
    bool tried = false;
    std::atomic<int> job{0};   // 0 idle, 1 working, 2 done
    std::atomic<bool> dlOk{false};
    std::thread th;
    std::vector<dect2::Navaid> db;
    std::string err;
};
NavS& nav() { static NavS n; return n; }
std::string navPath() { return plat::dataDir() + "/navaids.csv"; }
std::string readNavFile() { std::ifstream f(navPath(), std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

void navLoadFile() {
    NavS& N = nav();
    N.db.clear();
    const std::string text = readNavFile();
    if (!text.empty()) dect2::navaidParseCsv(text, N.db, N.err);
    if (!N.db.empty()) vorUseNavaidFile(navPath());
}

unsigned navKindOf(const App& a) {
    if (a.family == 30) { const char* n = curModeName(a); return n && !std::strcmp(n, "VOR") ? dect2::NavaidVor : 0; }
    if (a.family == 29) return dect2::NavaidNdb;
    if (a.family == 121) return dect2::NavaidDme;
    return 0;
}

void navHint(App& a) {
    const unsigned kind = navKindOf(a);
    if (!kind) return;
    NavS& N = nav();
    ImGui::PushID("navhint");
    if (N.job.load() == 2) { if (N.th.joinable()) N.th.join(); N.job = 0; navLoadFile(); if (!N.dlOk.load() && N.db.empty()) N.err = tr("Could not download the station list."); }
    if (!N.tried) {   // first opening: the cached file, or one download
        N.tried = true;
        plat::Prefs& p = plat::prefs();
        if (std::ifstream(navPath()).good()) navLoadFile();
        else if (!p.getB("navaidAutoFetched", false)) {
            p.setB("navaidAutoFetched", true); p.flush();
            N.job = 1;
            const std::string path = navPath();
            N.th = std::thread([&N, path] { N.dlOk = plat::fetchUrl("https://davidmegginson.github.io/ourairports-data/navaids.csv", path, "OnAir"); N.job = 2; });
        }
    }
    if (N.job.load() == 1) { ImGui::TextDisabled("%s", tr("Getting the station list from OurAirports (public domain), once.")); ImGui::PopID(); return; }
    if (N.db.empty()) { ImGui::TextDisabled("%s", N.err.empty() ? tr("No station list. Import navaids.csv on the Stations tab.") : N.err.c_str()); ImGui::PopID(); return; }
    if (!receiverLocationSet()) { ImGui::TextDisabled("%s", tr("Set your location to list the nearest stations (View > Map).")); ImGui::PopID(); return; }
    double lat, lon;
    receiverLocation(lat, lon);
    const auto nearList = dect2::navaidNearest(N.db, lat, lon, kind, 500, 8);
    if (nearList.empty()) { ImGui::TextDisabled("%s", tr("No station within 500 km in the list.")); ImGui::PopID(); return; }
    const dect2::Navaid& first = N.db[nearList[0].index];
    char head[96]; snprintf(head, sizeof head, "%s: %s %.3f MHz, %.0f km", tr("Nearest"), first.ident.c_str(), first.freqMhz, nearList[0].km);
    if (ImGui::TreeNode("##nav", "%s", head)) {
        for (size_t i = 0; i < nearList.size(); i++) {
            const dect2::Navaid& s = N.db[nearList[i].index];
            ImGui::PushID((int)i);
            if (ImGui::SmallButton(tr("Tune"))) tuneFreq(a, s.freqMhz);
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextDisabled("%s  %s  %.3f MHz  %.0f km  %.0f deg", s.ident.c_str(), s.name.c_str(), s.freqMhz, nearList[i].km, nearList[i].bearingDeg);
            ImGui::PopID();
        }
        if (kind == dect2::NavaidVor && nearList.size() >= 2) {
            int x = -1, y = -1;
            if (dect2::navaidBestCrossing(nearList, x, y) && ImGui::SmallButton(tr("Use the best pair for a fix"))) vorSetFixPair(a, N.db[nearList[(size_t)x].index].freqMhz, N.db[nearList[(size_t)y].index].freqMhz);
        }
        ImGui::TextDisabled("%s", tr("Community data (OurAirports), not for navigation."));
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void geoHint(App& a) {
    const std::vector<ease::GeoSat> sats = ease::geoSatsOf(a.family);
    if (sats.empty()) return;
    if (!receiverLocationSet()) { ImGui::TextDisabled("%s", tr("Set your location to see if this satellite is above your horizon (View > Map).")); return; }
    double lat, lon;
    receiverLocation(lat, lon);
    std::vector<ease::GeoView> vs;
    for (const ease::GeoSat& s : sats) vs.push_back(ease::geoView(lat, lon, s));
    const int best = ease::geoBest(vs);
    const bool skew = ease::geoNeedsSkew(a.family);
    ImGui::PushID("geohint");
    if (sats.size() == 1) { ImGui::TextDisabled("%s", tr(ease::geoLine(vs[0], skew).c_str())); ImGui::PopID(); return; }
    int vis = 0; for (const ease::GeoView& v : vs) if (v.above) vis++;
    const std::string head = vis ? ease::geoLine(vs[best], skew) : tr("None of this mode's satellites is above your horizon.");
    if (ImGui::TreeNode("##geo", "%s", head.c_str())) {
        for (const ease::GeoView& v : vs) ImGui::TextDisabled("%s", ease::geoLine(v, skew).c_str());
        ImGui::TextDisabled("%s", tr("Satellite positions are the published slots; operators move satellites."));
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// E11: stated once per mode, while there is nothing else on the air to explain the silence: stopped, or in its first minute
void antennaHint(App& a) {
    static const char* lastMode = nullptr;
    static double shownUntil = 0;
    const char* name = curModeName(a);
    const double now = ImGui::GetTime();
    if (name != lastMode) { lastMode = name; shownUntil = now + 60; }
    if (a.engine.running() && now > shownUntil) return;
    const ease::AntennaNeed n = ease::antennaNeed(name, a.freqMhz);
    if (n.text.empty()) return;
    const char* warn = ease::antennaWarn(a.setupAntenna, a.freqMhz);
    ImGui::TextDisabled("%s: %s.%s%s", tr("Antenna"), tr(n.text.c_str()), warn[0] ? " " : "", tr(warn));
}

}  // namespace

const char* curModeName(const App& a) {
    for (const ModeDef& m : allModes()) if (m.family == a.family && (!m.subId || !a.sub || subIndexOf(m.subId) == a.sub)) return m.name;
    return nullptr;
}

void modeHintsLine(App& a) {
    firstStartHint(a);
    passHint(a);
    geoHint(a);
    navHint(a);
    antennaHint(a);
}

// E11: the browser's antenna row (its wording, not a judgement)
std::string modeAntennaNote(const ModeDef& m) {
    const ModeTuning* mt = nullptr;
    if (m.family >= 6) mt = dect2::modeTuning(m.family + 2);
    const double mhz = m.bandMinMhz > 0 ? m.bandMinMhz : mt ? mt->defMhz : 0;
    const ease::AntennaNeed n = ease::antennaNeed(m.name, mhz);
    return n.text.empty() ? std::string() : std::string("Antenna: ") + n.text;
}

// E4: a note in the mode browser. Empty when the mode is not geostationary, no position is set, or a satellite is above the horizon.
std::string modeGeoNote(const ModeDef& m) {
    const std::vector<ease::GeoSat> sats = ease::geoSatsOf(m.family);
    if (sats.empty() || !receiverLocationSet()) return {};
    double lat, lon;
    receiverLocation(lat, lon);
    for (const ease::GeoSat& s : sats) if (ease::geoView(lat, lon, s).above) return {};
    return N_("Below your horizon from your location");
}
