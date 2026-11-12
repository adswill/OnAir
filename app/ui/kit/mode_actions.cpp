// The action row every mode shows above its Simple view or its tabs: the "what do I do next" line, [Auto setup] and [Reset this mode].
// Auto setup: Auto gain, the mode's own bandwidth and sample rate, and the fine frequency (a narrow capture only: the strongest peak near the tuned
// frequency). Reset this mode: frequency, bandwidth and sample rate back to the mode's defaults, custom width and rate cleared. Both say what they did
// and keep an Undo for 12 seconds. Pure parts: mode_actions_logic.h.
#include "app.h"
#include "mode_actions_logic.h"
#include "mode_panel.h"
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

// 4.0: one row instead of four or five. Left: the next step (dot, line, its fix button such as Start); middle: the antenna hint as short muted
// text (the whole of it in the tooltip); right: the Undo note, Auto setup, Reset this mode, Simple / Advanced. A window too narrow for that
// puts the next step on a row of its own and the rest on a second row. The other hints (first start, next pass, satellite, navigation) follow
// on their own rows only when they have something to say.
void modeActionBar(App& a) {
    ImGui::PushID("##modeactions");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float sp = st.ItemSpacing.x, gap = 14 * gUi;
    const bool radio = hasRadioRunning(a);
    const bool level = modepanel::available(a);
    const auto bw = [&](const char* t) { return ImGui::CalcTextSize(t).x + 2 * st.FramePadding.x; };
    const bool undo = gUndo.have && ImGui::GetTime() < gUndo.until && gUndo.family == presetsapp::modeKey(a);
    if (gUndo.have && ImGui::GetTime() >= gUndo.until) gUndo.have = false;
    float rightW = 0;
    if (radio) rightW += bw(tr("Auto setup")) + sp + bw(tr("Reset this mode"));
    if (level) rightW += (rightW > 0 ? gap : 0.f) + modepanel::levelBarWidth();
    const float undoW = undo ? std::min(ImGui::CalcTextSize(gUndo.what.c_str()).x, 260 * gUi) + sp + bw(tr("Undo")) + sp : 0.f;
    const std::string ant = modeAntennaHint(a);
    const float antFull = ant.empty() ? 0.f : ImGui::CalcTextSize(ant.c_str()).x, antMin = ant.empty() ? 0.f : ImGui::CalcTextSize(tr("Antenna")).x;
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    const float leftMin = 220 * gUi;   // the next step needs this much to say anything useful on one row
    const bool oneRow = avail >= leftMin + (antMin > 0 ? antMin + gap : 0.f) + undoW + rightW + gap;
    // the middle part (antenna) and the right part, drawn after the next step (or on the second row)
    const auto rest = [&](float startX, float endX) {
        float x = startX;
        if (!ant.empty()) {
            const float room = endX - x - undoW - rightW - gap;
            if (room >= antMin) {
                ImGui::SameLine(0, 0); ImGui::SetCursorPosX(x);
                const std::string t = antFull <= room ? ant : ellipsize(ant, std::max(antMin, std::min(room, antFull)));
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", (room < antMin * 3 ? std::string(tr("Antenna")) : t).c_str());
                if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(ant.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            }
        }
        x = std::max(x, endX - undoW - rightW);
        if (undo) {
            ImGui::SameLine(0, 0); ImGui::SetCursorPosX(x);
            ImGui::AlignTextToFramePadding();
            const std::string t = ellipsize(gUndo.what, 260 * gUi);
            ImGui::TextDisabled("%s", t.c_str());
            if (t != gUndo.what && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gUndo.what.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Undo"))) doUndo(a);
            x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + sp;
        }
        if (radio) {
            ImGui::SameLine(0, 0); ImGui::SetCursorPosX(std::max(x, endX - rightW));
            if (ImGui::SmallButton(tr("Auto setup"))) { const std::string s = autoSetup(a); gUndo.what = std::string(tr("Auto setup: ")) + s; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("One click: Auto gain, this mode's own bandwidth and sample rate, and the frequency nudged onto the signal when it is a narrow one. Every step can be undone."));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Reset this mode"))) resetThisMode(a);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Put this mode's frequency, bandwidth and sample rate back to its defaults. Other modes and your other settings stay as they are."));
        }
        if (level) {
            ImGui::SameLine(0, 0); ImGui::SetCursorPosX(std::max(radio ? ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + gap : x, endX - modepanel::levelBarWidth()));
            modepanel::levelBar(a, false);
        }
    };
    const float endX = x0 + avail;
    if (oneRow) {
        const float leftMax = avail - (antMin > 0 ? antMin + gap : 0.f) - undoW - rightW - gap;
        const bool said = nextStepLine(a, std::max(leftMin, std::min(leftMax, avail * 0.6f)));
        const float after = said ? ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + gap : x0;
        if (!said) ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));   // the row still holds the buttons
        rest(after, endX);
    } else {
        nextStepLine(a, avail);
        if (!ant.empty() || undo || radio || level) { ImGui::Dummy(ImVec2(0, 0)); rest(x0, endX); }
    }
    modeHintsLine(a, false);   // the other hints, each a row of its own when it has something to say
    ImGui::PopID();
    ImGui::Spacing();
}
