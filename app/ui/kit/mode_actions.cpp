// The action row every mode shows above its Simple view or its tabs: the "what do I do next" line, [Auto setup] and [Reset this mode].
// Auto setup: Auto gain, the mode's own bandwidth and sample rate, and the fine frequency (a narrow capture only: the strongest peak near the tuned
// frequency). Reset this mode: frequency, bandwidth and sample rate back to the mode's defaults, custom width and rate cleared. Both say what they did
// and keep an Undo for 12 seconds. Pure parts: mode_actions_logic.h.
#include "app.h"
#include "mode_actions_logic.h"
#include "state/presets_app.h"
#include <cstdio>
#include <string>

namespace {

struct Undo {
    bool have = false;
    double until = 0;
    std::string what, family;         // the note ("Auto setup: ...") and the mode it belongs to
    dect2::presets::Preset preset;    // frequency, rate, bandwidth, gain
    RxWidthSnap width;
    bool agcOn = false;
};
Undo gUndo;

bool hasRadioRunning(const App& a) { return a.devIdx >= 0 && a.devIdx < (int)a.devices.size(); }

void snapshot(App& a, const char* what) {
    gUndo = Undo{};
    gUndo.have = true;
    gUndo.until = ImGui::GetTime() + 12;
    gUndo.what = what;
    gUndo.family = presetsapp::modeKey(a);
    gUndo.preset = presetsapp::capture(a, "undo");
    gUndo.width = rxWidthSnapshot(a);
    gUndo.agcOn = a.agcOn;
}

void doUndo(App& a) {
    if (!gUndo.have || gUndo.family != presetsapp::modeKey(a)) { gUndo.have = false; return; }
    a.agcOn = gUndo.agcOn;
    rxWidthRestore(a, gUndo.width);
    presetsapp::apply(a, gUndo.preset);
    gUndo.have = false;
}

// Auto gain, the mode's own bandwidth and rate, the fine frequency. Returns what it did, in plain words.
std::string autoSetup(App& a) {
    std::string done;
    auto add = [&done](const std::string& s) { if (!done.empty()) done += ", "; done += s; };
    snapshot(a, "");
    const bool radio = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio();
    if (radio && a.engine.running() && !a.agcOn && autogainRun(a, 1)) add(tr("Auto gain on"));
    else if (radio && a.agcOn) add(tr("Auto gain already on"));
    if (rxWidthIsCustom(a)) { rxWidthReset(a); add(tr("bandwidth and rate reset")); }
    if (a.engine.running()) {
        const double win = modeactions::snapWindowHz(a.tune.bandwidthMhz);
        double found = 0;
        const double off = (a.freqMhz - radioCenterMhz(a)) * 1e6;
        if (win > 0 && modeactions::refineOffsetHz(a.spec.dbfs, a.engine.sampleRate(), off, win, &found) && std::fabs(found - off) >= 1.0) {
            const double newMhz = a.freqMhz + (found - off) / 1e6;
            tuneFreq(a, newMhz);
            char b[96];
            std::snprintf(b, sizeof b, tr("frequency %+.1f kHz"), (found - off) / 1e3);
            add(b);
        } else if (win <= 0) add(tr("wide signal, frequency kept"));
        else add(tr("frequency kept (no clear peak)"));
    } else add(tr("start the receiver first"));
    return done;
}

void resetThisMode(App& a) {
    snapshot(a, tr("Mode reset to its defaults"));
    if (rxWidthIsCustom(a)) rxWidthReset(a);
    presetsapp::resetModeToDefaults(a);
}

}  // namespace

void modeActionBar(App& a) {
    ImGui::PushID("##modeactions");
    nextStepLine(a);
    const bool radio = hasRadioRunning(a);
    if (!radio) ImGui::BeginDisabled();
    if (ImGui::SmallButton(tr("Auto setup"))) { const std::string s = autoSetup(a); gUndo.what = std::string(tr("Auto setup: ")) + s; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("One click: Auto gain, this mode's own bandwidth and sample rate, and the frequency nudged onto the signal when it is a narrow one. Every step can be undone."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Reset this mode"))) resetThisMode(a);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Put this mode's frequency, bandwidth and sample rate back to its defaults. Other modes and your other settings stay as they are."));
    if (!radio) ImGui::EndDisabled();
    if (gUndo.have && ImGui::GetTime() < gUndo.until && gUndo.family == presetsapp::modeKey(a)) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", gUndo.what.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Undo"))) doUndo(a);
    } else if (gUndo.have && ImGui::GetTime() >= gUndo.until) gUndo.have = false;
    ImGui::PopID();
    ImGui::Spacing();
}
