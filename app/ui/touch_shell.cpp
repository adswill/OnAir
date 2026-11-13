// The touch interface's shell: a top bar (menu, the mode chip, status), the body (the main tabs of the classic / new interface, or the
// transmit panel) that scrolls by drag and swipes between tabs, and a bottom dock with the primary controls (start / stop, tuning,
// gain, volume, tuner, more). Panels are the existing ones: the shell only arranges them and sizes the controls.
#include "touch.h"
#include "../app.h"
#include "theme.h"
#include "scale.h"
#include "toolbar_internal.h"

namespace touch {

namespace {
bool gOpenGain = false, gOpenVol = false, gOpenTuner = false, gOpenMore = false;
bool gDevDone = false;
int gMorePage = 0;   // 0 menu of pages, 1 status, 2 results list, 3 analysis
ShellLayout gL;      // this frame's bars and dock (touch_logic.h shellLayout: control size, short screens)

struct Item { const char* label; float weight; };

// One row of the dock: the items share the width by weight. Returns the index pressed (-1 none). Items in repeatMask repeat while held (+ and -
// keep tuning); the item at startIdx wears the start / stop colour.
int dockRow(const Item* it, int n, float h, unsigned repeatMask, int startIdx, bool running) {
    float total = 0;
    for (int i = 0; i < n; i++) total += it[i].weight;
    const float sp = ImGui::GetStyle().ItemSpacing.x, w = ImGui::GetContentRegionAvail().x - sp * (n - 1);
    int hit = -1;
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        const bool rep = (repeatMask >> i) & 1, col = i == startIdx;
        if (rep) ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
        if (col) {
            ImGui::PushStyleColor(ImGuiCol_Button, running ? ImVec4(0.55f, 0.16f, 0.16f, 1) : ImVec4(0.10f, 0.42f, 0.28f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, running ? ImVec4(0.70f, 0.22f, 0.22f, 1) : ImVec4(0.14f, 0.56f, 0.36f, 1));
        }
        if (ImGui::Button(it[i].label, ImVec2(std::floor(w * it[i].weight / total), h))) hit = i;
        if (col) ImGui::PopStyleColor(2);
        if (rep) ImGui::PopItemFlag();
        ImGui::PopID();
    }
    return hit;
}

void topBar(App& a, ImVec2 disp) {
    const float h = gL.topH;
    if (ImGui::Button("=", ImVec2(h, h))) openMenu();   // the menu bar, folded into one button
    ImGui::SameLine();
    const ModeDef* cur = &kModes[0];
    for (const auto& m : allModes()) if (modeSelected(a, m)) cur = &m;
    char chip[96]; snprintf(chip, sizeof chip, "%s  v##modechip", cur->name);
    const float chipW = std::min(disp.x * 0.5f, 260.f * dpi() * std::max(0.85f, sizeFactor()));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4((cur->col & 0x00FFFFFF) | 0xFF000000));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4((cur->col & 0x00FFFFFF) | 0xFF000000));
    if (ImGui::Button(chip, ImVec2(chipW, h))) openModePicker();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    const bool run = a.engine.running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(run ? pal::okGreen() : pal::grey(), "%s", run ? tr("Running") : tr("Stopped"));
}

// the dock: tuning on the first row (- frequency + step), the actions on the second; one row when the display is wide, and on a short screen
// one row without Step (it is in More then: shellLayout)
void dock(App& a, ImVec2 disp, float dockH) {
    (void)disp;
    ImGui::BeginChild("##tdock", ImVec2(0, dockH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float h = gL.rowH;
    const bool wide = gL.dockRows == 1;
    const bool running = a.engine.running();
    const std::string fq = mhzText(a.freqMhz) + " MHz";
    char step[32]; stepLabel(stepHz(), step, sizeof step);
    char stepBtn[48]; snprintf(stepBtn, sizeof stepBtn, "%s: %s", tr("Step"), step);
    const char* startLbl = running ? tr("Stop") : tr("Start");
    int hit = -1;
    Item tune[4] = {{"-", 0.8f}, {fq.c_str(), 3.f}, {"+", 0.8f}, {stepBtn, 1.6f}};
    Item act[5] = {{startLbl, 1.4f}, {tr("Gain"), 1.f}, {tr("Volume"), 1.f}, {tr("Tuner"), 1.f}, {tr("More"), 1.f}};
    // dock ids: 0 -, 1 frequency, 2 +, 3 step, 10 start / stop, 11 gain, 12 volume, 13 tuner, 14 more
    if (wide && gL.stepInMore) {   // a short screen: start, - frequency +, gain, volume, tuner, more
        const Item all[8] = {act[0], tune[0], tune[1], tune[2], act[1], act[2], act[3], act[4]};
        static const int map[8] = {10, 0, 1, 2, 11, 12, 13, 14};
        const int r = dockRow(all, 8, h, (1u << 1) | (1u << 3), 0, running);
        if (r >= 0) hit = map[r];
    } else if (wide) {   // one row: start, - frequency + step, gain, volume, tuner, more
        const Item all[9] = {act[0], tune[0], tune[1], tune[2], tune[3], act[1], act[2], act[3], act[4]};
        static const int map[9] = {10, 0, 1, 2, 3, 11, 12, 13, 14};
        const int r = dockRow(all, 9, h, (1u << 1) | (1u << 3), 0, running);
        if (r >= 0) hit = map[r];
    } else {
        const int r1 = dockRow(tune, 4, h, (1u << 0) | (1u << 2), -1, running);
        if (r1 >= 0) hit = r1;
        const int r2 = dockRow(act, 5, h, 0, 0, running);
        if (r2 >= 0) hit = 10 + r2;
    }
    const double stepMhz = stepHz() / 1e6;
    switch (hit) {
    case 0: tuneFreq(a, a.freqMhz - stepMhz); break;
    case 1: openKeypad(a.freqMhz * 1e6); break;
    case 2: tuneFreq(a, a.freqMhz + stepMhz); break;
    case 3: openStepPicker(); break;
    case 10: if (running) a.engine.stop(); else startReceiver(a); break;
    case 11: gOpenGain = true; break;
    case 12: gOpenVol = true; break;
    case 13: gOpenTuner = true; break;
    case 14: gOpenMore = true; gMorePage = 0; break;
    default: break;
    }
    ImGui::EndChild();
}

// the sheets of the dock
void gainSheet(App& a) {
    if (gOpenGain) { ImGui::OpenPopup("tsheet_gain"); gOpenGain = false; }
    if (!beginSheet("gain", tr("Gain"))) return;
    toolbarParts(a, TbGain, true);
    endSheet();
}

void volumeSheet(App& a) {
    if (gOpenVol) { ImGui::OpenPopup("tsheet_vol"); gOpenVol = false; }
    if (!beginSheet("vol", tr("Volume"))) return;
    int pct = (int)std::lround(a.volume * 100);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##tvol", &pct, 0, 200, "%d%%")) a.volume = pct / 100.f;
    ImGui::Checkbox(tr("Mute"), &a.muted);
    endSheet();
}

void tunerSheet(App& a) {
    if (gOpenTuner) { ImGui::OpenPopup("tsheet_full_tuner"); gOpenTuner = false; }
    if (!beginSheet("tuner", tr("Tuner"), true)) return;
    ImGui::BeginChild("##ttuner", ImVec2(0, 0), 0);
    toolbarParts(a, TbSource | TbTuner | TbDecoder, true);
    if (sourceOptionsOpen(a)) sourceOptions(a);
    if (a.devices[a.devIdx].isRadio()) radioSettingsUi(a, true);
    ImGui::EndChild();
    endSheet();
}

void moreSheet(App& a) {
    if (gOpenMore) { ImGui::OpenPopup("tsheet_full_more"); gOpenMore = false; }
    if (!beginSheet("more", gMorePage == 0 ? tr("More") : gMorePage == 1 ? tr("Status") : gMorePage == 2 ? tr("List") : tr("Analysis"), true)) return;
    if (gMorePage == 0) {
        const float w = ImGui::GetContentRegionAvail().x;
        const char* names[3] = {tr("Status"), tr("List"), tr("Analysis")};
        for (int i = 0; i < 3; i++) if (bigButton(names[i], ImVec2(w, target() * 1.3f))) gMorePage = i + 1;
        if (gL.stepInMore) {   // the short one-row dock has no Step button
            char step[32]; stepLabel(stepHz(), step, sizeof step);
            char stepBtn[64]; snprintf(stepBtn, sizeof stepBtn, "%s: %s##tmorestep", tr("Step"), step);
            if (bigButton(stepBtn, ImVec2(w, target() * 1.3f))) { closeSheet(); openStepPicker(); }
        }
    } else {
        if (bigButton(tr("Back"), ImVec2(0, 0))) gMorePage = 0;
        ImGui::BeginChild("##tmore", ImVec2(0, 0), 0);
        if (gMorePage == 1) statusBar(a);
        else if (gMorePage == 2) rightPanel(a);
        else constellationsTab(a);
        ImGui::EndChild();
    }
    endSheet();
}

}   // namespace

void shell(App& a, ImVec2 disp) {
    frame();
    if (!gDevDone && !gDevOpen.empty() && ImGui::GetFrameCount() > 60) {   // --touchsheet gain | vol | tuner | more | more1 | more2 | more3
        gDevDone = true;
        gOpenGain = gDevOpen == "gain"; gOpenVol = gDevOpen == "vol"; gOpenTuner = gDevOpen == "tuner";
        if (gDevOpen.rfind("more", 0) == 0) { gOpenMore = true; gMorePage = gDevOpen.size() > 4 ? gDevOpen[4] - '0' : 0; }
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("##troot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const ImGuiStyle& sty = ImGui::GetStyle();
    gL = shellLayout(disp.x, disp.y, dpi(), sizeFactor(), 7 * sty.ItemSpacing.x + 2 * sty.WindowPadding.x);
    topBar(a, disp);
    const float sp = sty.ItemSpacing.y;
    const float dockH = gL.dockRows * gL.rowH + (gL.dockRows - 1) * sp + sty.WindowPadding.y * 2;
    const float bodyH = std::max(target(), ImGui::GetContentRegionAvail().y - dockH - sp);
    ImGui::BeginChild("##tbody", ImVec2(0, bodyH), 0);
    const ImVec2 bodyMin = ImGui::GetWindowPos(), bodyMax = ImVec2(bodyMin.x + ImGui::GetWindowSize().x, bodyMin.y + ImGui::GetWindowSize().y);
    if (hamViewFlag()) hamPanel(a);
    else if (txViewFlag()) txPanel(a);
    else {
        if (gL.statusOnTop) { statusBar(a); ImGui::Separator(); }   // portrait: the mode's status line on top; landscape keeps the height for the picture (More > Status)
        mainTabs(a);
    }
    ImGui::EndChild();
    const GestureFrame& g = gesture();
    if (g.swipe && !pressedOnItem() && g.cx >= bodyMin.x && g.cx <= bodyMax.x && g.cy >= bodyMin.y && g.cy <= bodyMax.y && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        swipeTabs(-g.swipe);   // swipe left = the next tab
    dock(a, disp, dockH);
    keypadSheet(a);
    stepSheet(a);
    gainSheet(a);
    volumeSheet(a);
    tunerSheet(a);
    moreSheet(a);
    modePickerSheet(a, disp);
    menuSheet(a, disp);
    ImGui::End();
    endFrame();
}

}   // namespace touch
