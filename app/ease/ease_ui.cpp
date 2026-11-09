#include "ease_ui.h"
#include "dect2/ease/ease.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/inmsat/inmsat_satid.h"
#include "i18n/i18n.h"
#include <cmath>
#include <map>
#include <memory>

namespace easeui {

int preselectSat(App& a, const char* prefKey, const double* slotLon, int count, SatPick& st) {
    const ModeTuning* mt = modeTuning(a.family + 2);
    plat::Prefs& p = plat::prefs();
    const auto act = dect2::ease::preselectAction(p.has(prefKey), receiverLocationSet(), a.engine.running(), a.freqMhz, mt ? mt->defMhz : a.freqMhz);
    if (act == dect2::ease::Preselect::LeaveAlone) { p.setI(prefKey, 0); return -1; }
    if (act != dect2::ease::Preselect::Pick) return -1;
    double la, lo;
    receiverLocation(la, lo);
    const dect2::ease::GeoPick g = dect2::ease::highestGeo(la, lo, slotLon, count);
    if (g.index < 0) return -1;
    st.prevMhz = a.freqMhz; st.undoable = true;
    p.setI(prefKey, g.index + 1);
    savePrefs(a);
    return g.index;
}

bool satPointing(App& a, double slotLon, SatPick& st) {
    if (!receiverLocationSet()) return false;
    double la, lo, az = 0, el = 0;
    receiverLocation(la, lo);
    dect2::inmsat::geoLook(la, lo, slotLon, az, el);
    ImGui::PushTextWrapPos(0);
    if (el < 0) ImGui::TextDisabled("%s", tr("This satellite is below your horizon."));
    else ImGui::TextDisabled(tr("Point the antenna %s (%.0f deg), %.1f deg up."), tr(dect2::ease::compass8(az)), az, el);
    ImGui::PopTextWrapPos();
    bool undo = false;
    if (st.undoable) {
        ImGui::TextDisabled("%s", tr("Chosen from your position as the highest satellite."));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Undo##easesat"))) { undo = true; st.undoable = false; }
    }
    return undo;
}

bool autoBandControl(App& a, const char* prefKey, uint64_t decoded) {
    struct St { bool loaded = false, on = false; std::unique_ptr<dect2::ease::AutoBand> ab; double lastMhz = 0; std::string note; };
    static std::map<std::string, St> all;
    St& s = all[prefKey];
    if (!s.loaded) { s.loaded = true; s.on = plat::prefs().getB(prefKey, false); }
    if (ImGui::Checkbox(tr("Auto band"), &s.on)) {
        plat::prefs().setB(prefKey, s.on);
        s.ab.reset(); s.lastMhz = 0; s.note.clear();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Listens 30 s on each band used in your region, settles where devices are heard and keeps sampling the others. One radio can still miss a device that sends rarely. Tuning by hand turns it off."));
    if (!s.on) {
        if (!s.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", s.note.c_str()); }
        return true;
    }
    if (!s.ab) s.ab = std::make_unique<dect2::ease::AutoBand>(dect2::ease::ismBandsFor(plat::prefs().getS("locCountry", "")));
    if (!a.engine.running()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Starts when the receiver runs.")); return false; }
    if (s.lastMhz > 0 && std::fabs(a.freqMhz - s.lastMhz) > 0.0001) {   // someone tuned: that choice wins
        s.on = false; plat::prefs().setB(prefKey, false); s.ab.reset(); s.lastMhz = 0;
        s.note = tr("Auto band off: you tuned by hand.");
        return true;
    }
    const int idx = s.ab->update(ImGui::GetTime(), decoded);
    const double mhz = s.ab->bandMhz(idx);
    if (std::fabs(a.freqMhz - mhz) > 0.0001) tuneFreq(a, mhz);
    s.lastMhz = a.freqMhz;
    ImGui::SameLine();
    ImGui::TextDisabled(s.ab->settled() ? tr("On %.2f MHz, where decodes arrive.") : tr("Looking for devices, now on %.2f MHz."), mhz);
    return false;
}

void codecNotice(App& a) {
    for (const ModeDef& m : allModes()) {
        if (!modeSelected(a, m)) continue;
        if (!m.name || !modeCodecMissing(m)) return;
        const std::string key = std::string("codecNoteHidden.") + m.name;
        if (plat::prefs().getB(key.c_str(), false)) return;
        const std::string n = m.name;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("The voice codec for this mode is not installed. Calls and data still decode, the sound stays off."));
        ImGui::PopTextWrapPos();
        const CodecHelp c = n == "NXDN" ? CodecHelp::NxdnAmbe : n.rfind("dPMR", 0) == 0 ? CodecHelp::DpmrAmbe : n == "D-STAR" ? CodecHelp::DstarAmbe : n.rfind("P25", 0) == 0 ? CodecHelp::P25Imbe : CodecHelp::DmrAmbe;
        if (n != "TETRA") { codecHelpButton(c, tr("How to install")); ImGui::SameLine(); }
        if (ImGui::SmallButton(tr("Hide##codecnote"))) plat::prefs().setB(key.c_str(), true);
        return;
    }
}

}
