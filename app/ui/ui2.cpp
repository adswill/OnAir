// The new interface, in a plain developer-tool style: flat, dense and monochrome. A text list of modes with keyboard shortcuts
// on the left, square bordered panes and hairlines instead of cards, one muted accent. It reuses every panel of the classic
// interface (main.cpp). This file holds the entry point drawShell2() and the Sidebar / Scope / Tiles layouts; the others
// are in ui2_faces.cpp, the shell in ui2_shell.cpp, the theme in ui2_theme.cpp.
#include "ui2_internal.h"
#include "input_map.h"
#include "dect2/dsp/baseband_filter.h"

using namespace ui2;

namespace ui2 {

// ---------------------------------------------------------------- variant 0: sidebar (the parent)
void bodyParent(App& a, float bodyH, ImVec2 disp) {
    // a narrow window (a laptop at 125 or 150 %): the side panel and the list on the right give up width before the spectrum does
    float sideW = std::floor(std::max(244.f * gUi, std::min(296.f * gUi, disp.x * 0.24f)));
    if (a.splitSide > 0) sideW = std::floor(clampPx(a.splitSide * gUi, 170 * gUi, disp.x * 0.5f));
    const bool rail = a.layoutUnified && a.sideCollapsed;   // Unified layout: the controls panel folded to a rail of icons
    if (rail) sideW = std::floor(34 * gUi);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (rail) {
        auto iconBtn = [&](const char* id, Ic ic, const char* tip, bool on) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 1 * gUi);
            return kit::iconButton(id, ic, tr(tip), on);
        };
        if (iconBtn("##railExpand", Ic::Right, "Show the controls panel", false)) { a.sideCollapsed = false; savePrefs(a); }
        const bool run = a.engine.running();
        if (iconBtn("##railRun", run ? Ic::Stop : Ic::Play, run ? "Stop (Space)" : "Start (Space)", run)) { if (run) a.engine.stop(); else startReceiver(a); }
        if (iconBtn("##railTune", Ic::Sliders, "Source, gain and tuner: show the panel", false)) { a.sideCollapsed = false; savePrefs(a); }
    } else {
        if (a.layoutUnified) {   // the fold button, a thin row on top
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 18 * gUi);
            if (ImGui::SmallButton("<<##railFold")) { a.sideCollapsed = true; savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Fold the controls panel to icons"));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2 * gUi);
        }
        sidebar(a);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    {
        const ImVec2 p = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - bodyH), ImVec2(p.x, p.y), u32(kEdge));
    }
    ImGui::SameLine(0, 0);
    splitter("##splitSide", true, 5 * gUi, bodyH, a.splitSide, sideW, 170 * gUi, disp.x * 0.5f, 1, a);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    // scrolls only when even the smallest panes do not fit (a 1366 x 768 screen at 150 %)
    ImGui::BeginChild("##content", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const bool tx = txViewFlag();
    if (!tx && sourceOptionsOpen(a)) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    if (!tx) {
        beginPane("##status", ImVec2(0, 0), true);
        statusBar(a);
        endPane();
    }
    // the analysis row gives up height (down to 150) before the tabs above it go below 240; the list on the right narrows with the window
    const float availH = ImGui::GetContentRegionAvail().y;
    const bool uni = a.layoutUnified && !tx && !a.atsc3Mode;   // Unified: nothing in the central area stays empty (docs/UI-LAYOUT.md)
    ImGuiStorage* stg = ImGui::GetStateStorage();
    const ImGuiID kRightEmpty = ImGui::GetID("##uniRightEmpty"), kAnaFold = ImGui::GetID("##uniAnaFold");
    const bool running = a.engine.running();
    const float anaHdr = std::floor(kit::px(kit::tokens().headerH));
    const ImGuiID kAnaNone = ImGui::GetID(("##uniAnaNone" + std::to_string(a.family)).c_str());   // measured: this mode has no charts to show
    const bool anaOpen = uni && running && !stg->GetBool(kAnaFold, false) && !stg->GetBool(kAnaNone, false);
    float logH = (a.atsc3Mode || tx) ? 0 : std::max(144.f * gUi, std::min(215.f * gUi, availH - gap - 240.f * gUi));
    if (uni) logH = 0;   // the charts are the "Analysis" tab of the decoded views (main_tabs.cpp)
    if (!a.atsc3Mode && !tx && a.splitBottom > 0 && !uni) logH = clampPx(a.splitBottom * gUi, 90 * gUi, availH - gap - 150 * gUi);
    float rightW = std::floor(std::max(236.f * gUi, std::min(330.f * gUi, ImGui::GetContentRegionAvail().x * 0.30f)));
    if (a.splitRight > 0) rightW = std::floor(clampPx(a.splitRight * gUi, 150 * gUi, ImGui::GetContentRegionAvail().x - gap - 260 * gUi));
    const float mainH = std::max(186.f * gUi, availH - (logH > 0 ? logH + gap : 0));
    const bool rightEmpty = uni && stg->GetBool(kRightEmpty, false);   // measured last frame: the list is only a note
    const float emptyLimit = ImGui::GetTextLineHeightWithSpacing() * 3.2f + 10 * gUi;
    if (rightEmpty) {   // drawn once more out of sight (clipped away) to learn when it has something to show
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(rowPos, rowPos, false);
        ImGui::BeginChild("##right2m", ImVec2(rightW, mainH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);
        const float y0 = ImGui::GetCursorPosY();
        rightPanel(a);
        const float used = ImGui::GetCursorPosY() - y0;
        ImGui::EndChild();
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(rowPos);
        if (used > emptyLimit) stg->SetBool(kRightEmpty, false);
    }
    const float mainW = rightEmpty ? ImGui::GetContentRegionAvail().x : ImGui::GetContentRegionAvail().x - rightW - gap;
    beginPane("##main2", ImVec2(mainW, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    if (rightEmpty) a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    else {
        ImGui::SameLine(0, 0);
        splitter("##splitRight", true, gap, mainH, a.splitRight, rightW, 150 * gUi, mainW + rightW - 260 * gUi, -1, a);
        ImGui::SameLine(0, 0);
        beginPane("##right2", ImVec2(0, mainH));
        tgRect(a, TgRight);
        const float y0 = ImGui::GetCursorPosY();
        rightPanel(a);
        if (uni && ImGui::GetCursorPosY() - y0 <= emptyLimit) stg->SetBool(kRightEmpty, true);
        endPane();
    }
    if (uni) {
        a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    } else if (!a.atsc3Mode && !tx) {
        {   // the bar between the row above and the lower row, in the gap that is there anyway
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
            splitter("##splitBottom", false, gap, ImGui::GetContentRegionAvail().x, a.splitBottom, logH, 90 * gUi, availH - gap - 150 * gUi, -1, a);
            ImGui::PopStyleVar();
        }
        beginPane("##const2", ImVec2(0, std::max(logH, ImGui::GetContentRegionAvail().y)));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void ribbonPane(App& a) {
    beginPane("##ribbon", ImVec2(0, 0), true);
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("MODE")); ImGui::SameLine(0, 6 * gUi); modeCombo(a, 130 * gUi);
    a.tgMax[TgSwitch] = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::SameLine(0, 18 * gUi);
    displayControls(a, false);
    toolbarParts(a, TbSource | TbTuner | TbGain | TbDecoder, false);
    if (a.devices[a.devIdx].isRadio()) { ImGui::SameLine(0, 14 * gUi); radioSettingsUi(a, false); }
    if (sourceOptionsOpen(a)) sourceOptions(a);
    endPane();
}

// ---------------------------------------------------------------- variant 1: scope (the picture first: a control ribbon, a huge spectrum and waterfall)
void bodyScope(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##scope", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float availH = ImGui::GetContentRegionAvail().y, availW = ImGui::GetContentRegionAvail().x;
    float botH = (a.atsc3Mode ? 0 : 150) * gUi, dockW = 310 * gUi;
    if (!a.atsc3Mode && a.splitBot2 > 0) botH = clampPx(a.splitBot2 * gUi, 90 * gUi, availH - gap - 150 * gUi);
    if (a.splitDock > 0) dockW = clampPx(a.splitDock * gUi, 150 * gUi, availW - gap - 260 * gUi);
    const float mainH = availH - (botH > 0 ? botH + gap : 0);
    const float mainW = availW - dockW - gap;
    beginPane("##main2", ImVec2(mainW, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine(0, 0);
    splitter("##splitDock", true, gap, mainH, a.splitDock, dockW, 150 * gUi, mainW + dockW - 260 * gUi, -1, a);
    ImGui::SameLine(0, 0);
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
        splitter("##splitBot2", false, gap, availW, a.splitBot2, botH, 90 * gUi, availH - gap - 150 * gUi, -1, a);
        ImGui::PopStyleVar();
        beginPane("##const2", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void bodyTiles(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##tiles", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (sourceOptionsOpen(a)) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float row1 = std::floor(H * 0.60f) - gap;
    const float wA = std::floor(W * 0.46f), wB = std::floor(W * 0.24f), wC = W - wA - wB - 2 * gap;
    beginTile("##t_scope", N_("SPECTRUM / WATERFALL"), ImVec2(wA, row1));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_list", listTitle(a, true), ImVec2(wB, row1));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_ctl", N_("CONTROLS"), ImVec2(wC, row1));
    tgRect(a, TgToolbar);
    ImGui::TextDisabled("%s", tr("source")); toolbarParts(a, TbSource, true);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("gain")); toolbarParts(a, TbGain, true);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("tuner")); toolbarParts(a, TbTuner, true);
    ImGui::Spacing();
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::TextDisabled("%s", tr("mode")); modeList(a);
    if (a.family == 0 || a.family == 3 || captureRowShown(a) || ((modeUi(a.family) && modeUi(a.family)->decoder) || *dect2::bbf::keyForFamily(a.family))) { ImGui::Spacing(); ImGui::TextDisabled("%s", tr("decoder")); toolbarParts(a, TbDecoder, true); }
    endPane();
    const float row2 = ImGui::GetContentRegionAvail().y;
    const float wD = std::floor(W * 0.70f);
    if (!a.atsc3Mode) {
        beginTile("##t_plots", N_("ANALYSIS"), ImVec2(wD, row2));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
        ImGui::SameLine();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    beginTile("##t_log", N_("LOG"), ImVec2(0, row2));
    ImGui::BeginChild("##logc", ImVec2(0, 0));
    size_t n = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(n);
    for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    endPane();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

}  // namespace ui2

void drawShell2(App& a, ImVec2 disp) {
    {   // keyboard: 1-9 pick a mode, space starts and stops (not while typing)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {   // the keys are View > Controls (ui/input_map.h): Space, 1-9 by default, with exactly their modifiers
            if (!gFreqHovered) for (int k = 0; k < 9; k++) if (inputmap::pressed((inputmap::Action)((int)inputmap::Action::Mode1 + k))) {
                int n = 0;
                for (int g = 0; g < kNumGroups; g++) for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i]) && n++ == k) selectMode(a, kModes[i].family, kModes[i].subId);
            }
            const bool spaceTaken = gSpaceClaimFrame >= ImGui::GetFrameCount() - 1 || txViewFlag();   // the TV picture or the transmitter has it
            if (!spaceTaken && inputmap::pressed(inputmap::Action::StartStop)) { if (a.engine.running()) a.engine.stop(); else startReceiver(a); }
            if (inputmap::pressed(inputmap::Action::Mute)) { a.muted = !a.muted; a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted); }
        }
    }
    if (a.kitGallery) { kit::gallery(a, disp); return; }
    {   // Ctrl/Cmd+Shift+L: Classic <-> Unified (the receiver keeps running)
        ImGuiIO& io = ImGui::GetIO();
        if ((io.KeyCtrl || io.KeySuper) && io.KeyShift && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_L, false)) { a.layoutUnified = !a.layoutUnified; savePrefs(a); }
    }
    if (a.devViewMenu > 0) {   // dev: --viewmenu opens the View menu by a click, for a hidden screenshot
        static int n = 0; ImGuiIO& io = ImGui::GetIO();
        if (++n == 60) io.AddMousePosEvent(gViewMenuCentre.x, gViewMenuCentre.y);
        if (n == 62) io.AddMouseButtonEvent(0, true);
        if (n == 64) io.AddMouseButtonEvent(0, false);
        if (n == 140 && a.devViewMenu == 2) io.AddMousePosEvent(gViewMenuCentre.x + 40 * gUi, gViewMenuCentre.y + 22 * gUi);   // on Layout: its submenu opens
    }
    const float menuH = menuBar(a);
    {   // the waterfall range is set automatically once, two seconds after the receiver starts
        static bool wasRunning = false, done = false;
        static double t0 = 0;
        const bool run = a.engine.running();
        if (run && !wasRunning) { t0 = ImGui::GetTime(); done = false; }
        wasRunning = run;
        if (run && !done && ImGui::GetTime() - t0 > 2.5) { autoRange(a); done = true; }
    }
    pal::panel() = a.uiVariant == 7;
    const bool face = a.uiVariant == 3 || a.uiVariant == 7;
    ImGui::SetNextWindowPos(ImVec2(0, menuH));
    ImGui::SetNextWindowSize(ImVec2(disp.x, disp.y - menuH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (face) ImGui::PushStyleColor(ImGuiCol_WindowBg, a.uiVariant == 7 ? kPanelChassis : kChassis);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    if (face) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(6 * gUi + ImGui::GetFrameHeight() * 4.8f, ImGui::GetFrameHeight() + 8 * gUi));
    ImGui::SetCursorPos(ImVec2(6 * gUi, 4 * gUi));
    txViewSwitch(a);   // RX / TX, top left
    ImGui::SetCursorPosX(0);
    const float sbH = statusStripBlockHeight() + 8 * gUi;
    if (hamViewFlag()) {}   // the Ham view has no top bar of its own: the receiver keeps its
    else if (txViewFlag()) txTopBar(a, disp);   // the transmit view has its own top bar (the same frequency digits) and no side list
    else if (!face) topBar(a, disp);
    const float bodyH = ImGui::GetContentRegionAvail().y - sbH;
    if (hamViewFlag()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 6 * gUi));
        ImGui::BeginChild("##hambody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        hamPanel(a);
        ImGui::EndChild();
    } else if (txViewFlag()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 6 * gUi));
        ImGui::BeginChild("##txbody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        // the TX panel greys out its settings while it transmits and is mostly dim text: on the Light skin that was grey on grey (#320)
        if (a.lightUi) ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 0.85f);
        txPanel(a);
        if (a.lightUi) ImGui::PopStyleVar();
        ImGui::EndChild();
    } else if (dockActive(a)) dockBody(a, bodyH);   // View > Docking: the panels are windows in a dock space
    else switch (a.uiVariant) {
    case 1: bodyScope(a, bodyH, disp); break;
    case 2: bodyTiles(a, bodyH, disp); break;
    case 3: bodyFace(a, bodyH, disp); break;
    case 4: bodyCinema(a, bodyH, disp); break;
    case 5: bodyMeters(a, bodyH, disp); break;
    case 6: bodyDock(a, bodyH, disp); break;
    case 7: bodyPanel(a, bodyH, disp); break;
    default: bodyParent(a, bodyH, disp); break;
    }
    bottomBar(a, disp);
    ImGui::End();
    if (dockActive(a) && !hamViewFlag() && !txViewFlag()) dockPanes(a);
}
