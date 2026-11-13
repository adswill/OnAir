// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Get me listening": a three-step wizard for a new user. Step 1 picks a source (a connected radio, the demo signal or an IQ file),
// step 2 the place (its band plan, from core's bandplan registry), step 3 one of five things to listen to. It only picks settings and
// calls the existing tune and start code (setFamily / setMode / startReceiver); it adds no DSP of its own.
#include "../app.h"
#include "tour_logic.h"
#include "dect2/bandplan/bandplan.h"
#include "dect2/engine/mode_tuning.h"

namespace bp = dect2::bandplan;

namespace {

constexpr int kListenSteps = 3;

bool gOpen = false;
int gStep = 0;
int gDev = 0;       // step 1: an index into a.devices
int gPlan = -1;     // step 2: an index into the plans of listenPlans()
int gPreset = 0;    // step 3: an index into kPresets
bool gDontShow = false;

// The places: the built-in band plans and the user's own (the same folder the band plan picker uses).
bp::Registry& listenPlans() {
    static bp::Registry r = [] { bp::Registry x; x.setUserDir(plat::dataDir() + "/bandplans"); return x; }();
    return r;
}

// The five things to listen to. modeId "" is the FM broadcast family (its frequency comes from the place's band plan unless given);
// otherwise the mode's own tuning gives the family and the frequency, unless mhz is given.
struct Preset { const char* label; const char* modeId; double mhz; };
const Preset kPresets[] = {
    {N_("FM broadcast"), "", 0},
    {N_("Airband (aircraft voice)"), "airband", 0},
    {N_("Marine VHF (ships and harbours)"), "marine", 0},
    {N_("Weather radio (NOAA, 162.550 MHz)"), "", 162.550},
    {N_("Ham radio, 2 m (SSB and FM, 144.200 MHz)"), "general", 144.200},
};
constexpr int kNumPresets = (int)(sizeof kPresets / sizeof *kPresets);

// The FM frequency of a plan (its first FM band), or 100 MHz when the plan has none.
double fmMhzOf(const bp::Plan* p) {
    if (p) if (const bp::Band* b = p->firstBand("fm")) { const double hz = b->firstCentreHz > 0 ? b->firstCentreHz : b->startHz; if (hz > 0) return hz / 1e6; }
    return 100.0;
}

// The radios, the demo signal and the IQ file: the sources a new user can pick (sound cards and network rigs are under the source list).
bool listenSource(const DeviceInfo& d) { return d.isRadio() || d.kind == DeviceInfo::Synthetic || d.kind == DeviceInfo::File; }

// Start on the place the app would pick anyway: the saved plan, else the one for the language and the system region.
int defaultPlanIndex(const App& a) {
    const auto& plans = listenPlans().plans();
    if (!a.bandPlan.empty() && a.bandPlan != "none")
        for (int i = 0; i < (int)plans.size(); i++) if (plans[i].id == a.bandPlan) return i;
    const std::string id = listenPlans().defaultFor(a.uiLang.empty() ? plat::uiLanguage() : a.uiLang, bp::Registry::localeFromEnv());
    for (int i = 0; i < (int)plans.size(); i++) if (plans[i].id == id) return i;
    return plans.empty() ? -1 : 0;
}

void rememberDontShow() {
    plat::prefs().setB("listenWizardHide", gDontShow);
    plat::prefs().flush();
}

void closeWizard() {
    gOpen = false;
    rememberDontShow();
}

// Source, place and the chosen thing to listen to: the settings, then the start (the same calls the toolbar makes).
void finishWizard(App& a) {
    if (gDev >= 0 && gDev < (int)a.devices.size()) a.devIdx = gDev;
    const auto& plans = listenPlans().plans();
    if (gPlan >= 0 && gPlan < (int)plans.size()) a.bandPlan = plans[gPlan].id;
    const Preset& s = kPresets[gPreset];
    if (s.modeId[0] == '\0') {
        setFamily(a, 5);   // FM broadcast family
        a.freqMhz = s.mhz > 0 ? s.mhz : fmMhzOf(gPlan >= 0 && gPlan < (int)plans.size() ? &plans[gPlan] : nullptr);
    } else if (const ModeTuning* mt = modeTuningById(s.modeId)) {
        setFamily(a, mt->stdMode - 2);
        a.freqMhz = s.mhz > 0 ? s.mhz : mt->defMhz;
    }
    savePrefs(a);
    if (!a.engine.running()) startReceiver(a);
    closeWizard();
}

}

void listenWizardOpen(App& a) {
    gOpen = true;
    gStep = 0;
    gDev = a.devIdx;
    gPlan = defaultPlanIndex(a);
    gPreset = 0;
    gDontShow = plat::prefs().getB("listenWizardHide", false);
}

bool listenWizardShown() { return gOpen; }

void listenWizard(App& a, ImVec2 disp) {
    if (!gOpen) return;
    if (tour::guideInFront(a.setupOpen, a.wizOpen, gOpen, false) != tour::Guide::Listen) return;   // after the setup and the tour (#491)
    const float u = gUi;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.f * u, disp.x - 24 * u), 0));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::Begin("##listen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::TextColored(pal::accent(), "%s  (%d / %d)", tr("Get me listening"), gStep + 1, kListenSteps);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(560 * u);
    switch (gStep) {
    case 0: {
        ImGui::TextUnformatted(tr("Where does the signal come from?"));
        ImGui::Spacing();
        int radios = 0;
        for (int i = 0; i < (int)a.devices.size(); i++) {
            if (!listenSource(a.devices[i])) continue;
            if (a.devices[i].isRadio()) radios++;
            if (ImGui::RadioButton((a.devices[i].name + "##s" + std::to_string(i)).c_str(), gDev == i)) gDev = i;
        }
        if (!radios) {
            ImGui::Spacing();
            ImGui::TextColored(pal::warnAmber(), "%s", tr("No radio found"));
            ImGui::TextUnformatted(tr("You can go on without one: the demo signal and IQ files work with no hardware."));
            ImGui::TextUnformatted(tr("A radio on the network is not found by looking: add it under Setup wizard or the source list."));
        }
        ImGui::Spacing();
        if (ImGui::Button(tr("Look again"))) { refreshDevices(a); if (gDev >= (int)a.devices.size()) gDev = 0; }
        break;
    }
    case 1: {
        ImGui::TextUnformatted(tr("Where are you? This picks the band plan: the TV channels, FM and airband of that country."));
        ImGui::Spacing();
        const auto& plans = listenPlans().plans();
        ImGui::SetNextItemWidth(260 * u);
        const char* cur = gPlan >= 0 && gPlan < (int)plans.size() ? plans[gPlan].name.c_str() : "";
        if (ImGui::BeginCombo("##listenplace", cur)) {
            for (int i = 0; i < (int)plans.size(); i++) if (ImGui::Selectable(plans[i].name.c_str(), i == gPlan)) gPlan = i;
            ImGui::EndCombo();
        }
        break;
    }
    default: {
        ImGui::TextUnformatted(tr("What do you want to listen to?"));
        ImGui::Spacing();
        for (int i = 0; i < kNumPresets; i++)
            if (ImGui::RadioButton(tr(kPresets[i].label), gPreset == i)) gPreset = i;
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Checkbox(tr("Don't show again"), &gDontShow)) rememberDontShow();
    ImGui::Spacing();
    if (gStep > 0 && ImGui::Button(tr("Back"), ImVec2(70 * u, 0))) gStep--;
    if (gStep > 0) ImGui::SameLine();
    const bool last = gStep == kListenSteps - 1;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.40f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.45f, 0.53f, 1));
    if (ImGui::Button(tr(last ? "Start listening" : "Next"), ImVec2(130 * u, 0))) { if (last) finishWizard(a); else gStep++; }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 18 * u);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("Cancel"))) closeWizard();
    ImGui::PopStyleColor();
    ImGui::End();
}
