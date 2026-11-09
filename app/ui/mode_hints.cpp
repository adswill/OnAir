// One shared strip under the "what do I do next" line (modeActionBar): the next pass of a satellite mode, whether a geostationary satellite can be seen from
// this location, and later hints. Facts and one click, nothing is blocked. The logic is in ease_logic.h.
#include "../app.h"
#include "ease_logic.h"
#include "typography.h"
#include "imgui.h"
#include <cmath>
#include <map>
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

}  // namespace

void modeHintsLine(App& a) {
    passHint(a);
    geoHint(a);
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
