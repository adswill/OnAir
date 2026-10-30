// The custom capture bandwidth control and its saved value (rx_width.h): under Advanced in every mode.
#include "rx_width.h"
#include "../app.h"
#include <cstdio>
#include <string>

// saved per mode (the same id as the sample rate: each protocol entry of a shared receiver has its own); VDL2, TETRA and TETRAPOL kept their keys
static std::string key(const App& a) { return rxWidthKey(rateModeId(a)); }

static double savedWidth(const App& a) {
    const double w = plat::prefs().getD(key(a).c_str(), 0);
    return w > 0 && std::isfinite(w) ? w : 0;
}
static bool customOf(const App& a) { return bwCustomTicked((int)plat::prefs().getI((key(a) + ".custom").c_str(), -1), savedWidth(a)); }

// 0 (the mode's own standard bandwidth) unless the user ticked Custom
double rxWidthMhz(const App& a) { return rxWidthLoad(plat::prefs(), rateModeId(a)); }

static std::string bwText(double mhz) {
    char b[32];
    if (mhz < 1) std::snprintf(b, sizeof b, "%g kHz", mhz * 1e3);
    else std::snprintf(b, sizeof b, "%g MHz", mhz);
    return b;
}

bool captureRowShown(const App& a) {
    if (a.family < 0 || a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return false;
    const DeviceInfo& d = a.devices[a.devIdx];
    return d.isRadio() || d.kind == DeviceInfo::Synthetic;
}

// The Advanced row of every mode: the bandwidth and the sample rate as text with their standard values, and a Custom tick that opens the
// choice. Nothing is limited here: any width goes to the plan, any rate to the driver, and a limit only warns.
void rxWidthControl(App& a, bool& retune) {
    (void)retune;
    if (a.family < 0 || a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    bool custom = customOf(a);
    const bool custom0 = custom;
    const double saved = savedWidth(a), cur = custom ? saved : 0;
    const double stdBw = a.autoBwMhz > 0 ? a.autoBwMhz : a.tune.bandwidthMhz;
    static const double kPre[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 20.0};
    double pick = -1;
    bool ticked = false, openCustom = false;
    flowNext(10 * gUi);   // wraps with the rest of the row when the bar is narrow
    ImGui::TextDisabled("%s", tr("Advanced"));
    flowNext(8 * gUi);
    char sb[96];
    std::snprintf(sb, sizeof sb, "%s: %s (%s)", tr("Bandwidth"), bwText(stdBw).c_str(), tr("standard"));
    ImGui::TextDisabled("%s", custom ? tr("Bandwidth") : sb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How much spectrum the radio captures around the frequency. The standard bandwidth is the mode's own width; tick Custom to set any width you like (a decoder that searches for carriers then finds every one inside it). A wider capture needs a higher sample rate and more processor time; a limit here only warns."));
    flowNext(8 * gUi);
    const std::string pk = key(a);
    if (ImGui::Checkbox(tr("Custom##rxbwcustom"), &custom)) {
        plat::prefs().setI((pk + ".custom").c_str(), custom ? 1 : 0);
        if (custom && saved <= 0) plat::prefs().setD(pk.c_str(), stdBw > 0 ? stdBw : 1.0);   // a first tick starts at the standard width
        plat::prefs().flush();
        ticked = true;
    }
    if (custom) {
        const double shown = cur > 0 ? cur : (saved > 0 ? saved : (stdBw > 0 ? stdBw : 1.0));
        const std::string pv = bwText(shown);
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::BeginCombo("##rxbw", pv.c_str())) {
            for (double p : kPre) { const std::string l = bwText(p); if (ImGui::Selectable(l.c_str(), shown == p)) pick = p; }
            if (ImGui::Selectable(tr("Other…"))) openCustom = true;
            ImGui::EndCombo();
        }
        static char typed[32] = "1";
        if (openCustom) { std::snprintf(typed, sizeof typed, "%g", shown); ImGui::OpenPopup("##rxbwc"); }
        if (ImGui::BeginPopup("##rxbwc")) {
            ImGui::TextDisabled("%s", tr("Bandwidth: a number in MHz, or with k for kHz (250k)"));
            ImGui::SetNextItemWidth(120 * gUi);
            if (openCustom) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rxbwv", typed, sizeof typed, ImGuiInputTextFlags_EnterReturnsTrue)) { pick = parseWidthMhz(typed); ImGui::CloseCurrentPopup(); }
            if (ImGui::Button(tr("Apply"))) { pick = parseWidthMhz(typed); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        if (pick == 0) pick = -1;   // not a width: nothing changes
    }
    // the sample rate, saved per mode and radio, in the same row
    sampleRateAdvanced(a);
    if (pick >= 0) {
        plat::prefs().setD(pk.c_str(), pick);
        plat::prefs().flush();
    } else if (!ticked) return;
    const double oldRate = a.tune.sampleRate, oldBw = a.tune.bandwidthMhz;
    a.tune.centerHz = a.freqMhz * 1e6;
    a.rateRefusal.clear();
    applyBandwidth(a);
    if (a.engine.running() && (a.tune.sampleRate != oldRate || a.tune.bandwidthMhz != oldBw)) {
        startReceiver(a);
        if (!a.engine.running() && custom && (pick > 0 || ticked)) {   // the driver refused the width's rate: its own words, the previous setting is back
            const std::string why = a.engine.startError();
            a.rateRefusal = bwText(a.tune.bandwidthMhz) + " refused by the radio" + (why.empty() ? std::string() : ": " + why) + "; back to the previous setting";
            a.engine.log(a.rateRefusal);
            plat::prefs().setI((pk + ".custom").c_str(), custom0 ? 1 : 0);   // the setting from before
            plat::prefs().setD(pk.c_str(), saved);
            plat::prefs().flush();
            applyBandwidth(a);
            startReceiver(a);
        }
    }
}
