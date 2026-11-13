// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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

// Applies a changed width / rate choice of this mode: applyBandwidth(), a restart when the radio's rate or filter changed, and when the driver
// refuses the new setting its own message and the previous setting back. Shared by the Advanced row and the Simple view's chip.
static void applyWidthChange(App& a, bool custom, bool custom0, double saved, bool changedWidth) {
    const std::string pk = key(a);
    const double oldRate = a.tune.sampleRate, oldBw = a.tune.bandwidthMhz;
    a.tune.centerHz = a.freqMhz * 1e6;
    a.rateRefusal.clear();
    applyBandwidth(a);
    if (a.engine.running() && (a.tune.sampleRate != oldRate || a.tune.bandwidthMhz != oldBw)) {
        startReceiver(a);
        if (!a.engine.running() && custom && changedWidth) {   // the driver refused the width's rate: its own words, the previous setting is back
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

// The warning beside a custom width: the rate that carries it against this radio's list and the decoder's need. Only a warning, never a refusal.
static std::string widthWarning(const App& a) {
    if (!(rxWidthMhz(a) > 0) || a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!(d.isRadio() || d.kind == DeviceInfo::Synthetic)) return "";
    return rxWidthWarning(a.tune.sampleRate, dect2::rateLimitsOf(d), modeMinSampleRate(a));
}

// "Reset to default": the mode's own bandwidth and sample rate again (the custom values stay saved for the next tick)
static void resetToDefault(App& a) {
    const std::string pk = key(a);
    const bool custom0 = customOf(a);
    const double saved = savedWidth(a);
    plat::prefs().setI((pk + ".custom").c_str(), 0);
    plat::prefs().flush();
    if (a.chosenRateHz > 0) chooseSampleRate(a, 0);   // restarts the receiver itself
    applyWidthChange(a, false, custom0, saved, false);
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
    static const double kPre[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 50.0, 100.0};
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
        if (const std::string w = widthWarning(a); !w.empty()) {   // a warning, never a refusal
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "!");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", w.c_str());
        }
    }
    // the sample rate, saved per mode and radio, in the same row
    sampleRateAdvanced(a);
    if (customOf(a) || a.chosenRateHz > 0) {   // back to the mode's own bandwidth and rate in one click
        flowNext(8 * gUi);
        if (ImGui::SmallButton(tr("Reset to default##rxbwreset"))) { resetToDefault(a); return; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Back to this mode's own bandwidth and sample rate. Your custom values stay saved for the next time you tick Custom."));
    }
    if (pick >= 0) {
        plat::prefs().setD(pk.c_str(), pick);
        plat::prefs().flush();
    } else if (!ticked) return;
    applyWidthChange(a, custom, custom0, saved, pick > 0 || ticked);
}

// The compact chip of the Simple view (the Command view): "Bandwidth: default 1.8 MHz" or "Bandwidth: custom 5 MHz", with a popup that has the same
// choices as the Advanced row (a tick, any typed width, Reset to default). Shown wherever the Advanced row would be (a radio or the test signal).
void rxWidthChip(App& a) {
    if (!captureRowShown(a)) return;
    const bool custom = customOf(a);
    const double saved = savedWidth(a);
    const double stdBw = a.autoBwMhz > 0 ? a.autoBwMhz : a.tune.bandwidthMhz;
    char b[128];
    if (custom) std::snprintf(b, sizeof b, "%s: %s %s##rxbwchip", tr("Bandwidth"), tr("custom"), bwText(saved).c_str());
    else std::snprintf(b, sizeof b, "%s: %s %s##rxbwchip", tr("Bandwidth"), tr("default"), bwText(stdBw).c_str());
    if (ImGui::SmallButton(b)) ImGui::OpenPopup("##rxbwchippop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How much spectrum the radio captures. Default is the mode's own width; click to set any width you like."));
    const std::string w = widthWarning(a);
    if (!w.empty()) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "!");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", w.c_str());
    }
    if (!ImGui::BeginPopup("##rxbwchippop")) return;
    static char typed[32] = "1";
    static bool fresh = true;
    if (fresh) { std::snprintf(typed, sizeof typed, "%g", saved > 0 ? saved : (stdBw > 0 ? stdBw : 1.0)); fresh = false; }
    ImGui::TextDisabled("%s", tr("Bandwidth: a number in MHz, or with k for kHz (250k)"));
    ImGui::SetNextItemWidth(120 * gUi);
    const bool enter = ImGui::InputText("##rxbwchipv", typed, sizeof typed, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const std::string pk = key(a);
    if (ImGui::Button(tr("Apply##rxbwchipapply")) || enter) {
        const double v = parseWidthMhz(typed);
        if (v > 0) {
            const bool custom0 = customOf(a);
            plat::prefs().setI((pk + ".custom").c_str(), 1);
            plat::prefs().setD(pk.c_str(), v);
            plat::prefs().flush();
            applyWidthChange(a, true, custom0, saved, true);
            ImGui::CloseCurrentPopup();
        }
    }
    if (custom) {
        if (ImGui::Button(tr("Reset to default##rxbwchipreset"))) { resetToDefault(a); ImGui::CloseCurrentPopup(); fresh = true; }
    }
    if (!w.empty()) { ImGui::PushTextWrapPos(360 * gUi); ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "%s", w.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ImGui::CloseCurrentPopup(); fresh = true; }
    ImGui::EndPopup();
}

// The remote API and the MCP tool: the same as ticking Custom and typing a width (0: the mode's own again).
void rxWidthSetFromApi(App& a, double widthMhz) {
    if (a.family < 0 || a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    const std::string pk = key(a);
    const bool custom0 = customOf(a);
    const double saved = savedWidth(a);
    a.rateRefusal.clear();
    if (widthMhz > 0 && std::isfinite(widthMhz)) {
        plat::prefs().setI((pk + ".custom").c_str(), 1);
        plat::prefs().setD(pk.c_str(), widthMhz);
    } else {
        plat::prefs().setI((pk + ".custom").c_str(), 0);
    }
    plat::prefs().flush();
    applyWidthChange(a, widthMhz > 0, custom0, saved, true);
}

// "Reset this mode" and its Undo (app/ui/kit/mode_actions.cpp): the saved Custom tick, the width and the chosen rate of this mode
RxWidthSnap rxWidthSnapshot(const App& a) {
    RxWidthSnap s;
    s.custom = plat::prefs().getI((key(a) + ".custom").c_str(), -1);
    s.widthMhz = plat::prefs().getD(key(a).c_str(), 0);
    s.chosenRateHz = a.chosenRateHz;
    return s;
}
bool rxWidthIsCustom(const App& a) { return customOf(a) || a.chosenRateHz > 0; }
void rxWidthReset(App& a) { resetToDefault(a); }
void rxWidthRestore(App& a, const RxWidthSnap& s) {
    const bool custom0 = customOf(a);
    const double saved = savedWidth(a);
    if (s.custom >= 0) plat::prefs().setI((key(a) + ".custom").c_str(), s.custom);
    plat::prefs().setD(key(a).c_str(), s.widthMhz);
    plat::prefs().flush();
    if (s.chosenRateHz > 0 && a.chosenRateHz != s.chosenRateHz) restoreSampleRate(a, s.chosenRateHz);   // restarts the receiver itself
    applyWidthChange(a, customOf(a), custom0, saved, true);
}
