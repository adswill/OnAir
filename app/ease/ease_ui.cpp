#include "ease_ui.h"
#include "dect2/ease/ease.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/inmsat/inmsat_satid.h"
#include "i18n/i18n.h"

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

}
