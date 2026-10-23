// The classic interface's root: drawUI() runs one receiver's per-frame work and draws either the new interface (drawShell2)
// or the classic layout (top rows, status bar, main tabs, list, constellations), then the floating windows.
// Split from main.cpp.
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "adsb/adsb_map.h"
#include "ui/contrast.h"
#include "ui/touch.h"
#include "ui/command_palette.h"
#include "clips/clips_ui.h"
#include "dect2/source/iq_file.h"
#include "panels/gallery.h"
#include "dect2/engine/crash_report.h"
#include "platform/discord_rpc.h"
#include "dect2/power/power.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include "../main_internal.h"

bool gIdentifyDev = false;   // --identify

// the per-frame work of a receiver: ingest, the mode's tick, scans, EPG, gain control, the video and subtitles (drawn or not)
static void tickReceiver(App& a) {
    bbfSyncPrefs();
    ingestSpectrum(a);
    ingestRx(a);
    dabHistory(a);
    fmHistory(a);
    if (const ModeUi* mu = modeUi(a.family)) if (mu->tick) mu->tick(a);
    syncAdsbReference(a);
    wideSync(a);
    secondModeTick(a);
    dabScanStep(a);
    fmScanStep(a);
    harvestScan(a);
    scanDbTick(a);
    if (a.engine.running() && glfwGetTime() - a.epgT > 1.0) {
        a.epg = a.engine.epg();
        if (a.fakeEpg) {
            const int64_t base = (int64_t)time(nullptr) / 1800 * 1800 - 1800;
            static const char* titles[] = {"Morning News", "The Garden Show", "Football: Derby", "Documentary: Oceans", "Quiz Night", "Late Film"};
            for (auto& sv : a.ts.services) for (int k = 0; k < 8; k++) {
                EpgEvent e; e.eventId = k + 1; e.start = base + k * 3600; e.duration = 3600; e.running = k == 1 ? 4 : 0; e.genre = 1 + k % 5;
                e.title = std::string(titles[(k + sv.id) % 6]); e.text = "Short synopsis of " + e.title; e.extended = "A longer description of the programme, with details about the presenters, guests and what to expect during the hour.";
                a.epg[sv.id].push_back(e);
            }
        }
        a.epgT = glfwGetTime();
    }
    if (!a.engine.running()) { a.epg.clear(); }
    tvRecTick(a);
    audioCleanupTick(a);
    audioControlTick(a);
    voiceInversionTick(a);
    voiceKeysTick(a);
    timeMachineTick(a);
    gainControl(a);
    followBandwidth(a);
    feedDirection(a);
    a.video.update(a.engine.player());
    subsUpdate(a);   // subtitle services, the teletext / subtitle decoders' PIDs, the overlay
    if (a.playReq >= 0) for (auto& sv : a.ts.services) if (sv.id == a.playReq && sv.havePmt) { a.engine.player().select(sv.id); a.engine.player().setVolume(a.volume); a.playReq = -1; break; }
}

// F toggles the video-only view; Escape leaves it
static void videoOnlyKeys(App& a) {
    clipsui::tick(a);   // radio clips: settings, the audio ring, the C key and the dialog
    ImGuiIO& io = ImGui::GetIO();
    // I toggles the Identify tool (a click or a drag on the spectrum identifies the signal), Escape leaves it
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false)) a.identifyTool = !a.identifyTool;
    if (a.identifyTool && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) a.identifyTool = false;
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (a.videoOnly) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        else if (a.video.has()) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    }
    if (a.videoOnly && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
}

// the video over the whole window (video-only view)
static void videoOnlyWindow(App& a, ImVec2 disp) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::Begin("##vonly", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (a.video.has()) a.video.draw(disp); else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("no picture - press Esc")); ImGui::PopTextWrapPos(); }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    ImGui::End();
    ImGui::PopStyleColor();
}

// the classic interface in TX view: the guided transmit panel fills the window
static void classicTx(App& a, ImVec2 disp) {
    (void)disp;
    standardSwitch(a);
    ImGui::Separator();
    txPanel(a);
    ImGui::End();
}

// the classic top rows: the toolbar, source options, the mode selector and the right-aligned buttons
static void classicTopBar(App& a, ImVec2 disp) {
    const ImVec2 tb0 = ImGui::GetCursorScreenPos();
    toolbar(a);
        if (sourceOptionsOpen(a)) sourceOptions(a);
    a.tgMin[TgToolbar] = ImVec2(tb0.x - 4, tb0.y - 3); a.tgMax[TgToolbar] = ImVec2(tb0.x + ImGui::GetContentRegionAvail().x + 4, ImGui::GetCursorScreenPos().y);
    const ImVec2 sw0 = ImGui::GetCursorScreenPos();
    standardSwitch(a);
    if (a.devices[a.devIdx].isRadio()) { ImGui::SameLine(disp.x - 64 - 410 * gUi); radioSettingsUi(a, false); }   // the top bar is full
    ImGui::SameLine(disp.x - 64 - 370 * gUi);
    supportButton();
    ImGui::SameLine(disp.x - 64 - 290 * gUi);
    updateButton(a);
    clearAllButton(a);
    {   // the language, the new interface and the tour, right-aligned (their width changes with the language)
        int nl = 0;
        const i18n::Language* L = i18n::languages(nl);
        const char* langName = L[0].name;
        for (int i = 0; i < nl; i++) if (!strcmp(i18n::current(), L[i].code)) langName = L[i].name;
        const float pad = ImGui::GetStyle().FramePadding.x * 2, sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = ImGui::CalcTextSize(langName).x + ImGui::CalcTextSize(tr("Server")).x + ImGui::CalcTextSize(tr("New interface")).x + ImGui::CalcTextSize(tr("Tour")).x + 4 * pad + 3 * sp;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), disp.x - 12 * gUi - w));
    }
    languageButton(a);
    ImGui::SameLine();
    mapSendButton(a);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("New interface"))) { a.newUi = true; savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Try the new interface. You can switch back at any time."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Touch"))) { a.touchUi = true; savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Switch to the touch interface (big buttons, on-screen keypad). You can switch back at any time."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Tour"))) tourStart(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Take the guided tour"));
    a.tgMin[TgSwitch] = ImVec2(sw0.x - 2, sw0.y - 2); a.tgMax[TgSwitch] = ImVec2(sw0.x + gSwitchWidth + 20, ImGui::GetCursorScreenPos().y);
}

// the classic interface in RX view: top rows, status bar, main tabs, the list on the right and the constellations
static void classicRx(App& a, ImVec2 disp) {

    classicTopBar(a, disp);
    ImGui::Separator();
    statusBar(a);
    ImGui::Separator();

    float logH = (a.atsc3Mode ? 0 : std::min(250.f * gUi, std::max(170.f * gUi, ImGui::GetContentRegionAvail().y * 0.32f))), rightW = 350 * gUi;   // a short window gives the main area more   // ATSC 3.0 has no constellation panels yet
    float mainH = ImGui::GetContentRegionAvail().y - logH - 6;
    ImGui::BeginChild("main", ImVec2(disp.x - rightW - 16 * gUi, mainH));
    a.tgMin[TgMain] = ImGui::GetWindowPos(); a.tgMax[TgMain] = ImVec2(a.tgMin[TgMain].x + ImGui::GetWindowSize().x, a.tgMin[TgMain].y + ImGui::GetWindowSize().y);
    mainTabs(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, mainH));
    a.tgMin[TgRight] = ImGui::GetWindowPos(); a.tgMax[TgRight] = ImVec2(a.tgMin[TgRight].x + ImGui::GetWindowSize().x, a.tgMin[TgRight].y + ImGui::GetWindowSize().y);
    rightPanel(a);
    ImGui::EndChild();

    if (!a.atsc3Mode) {
        ImGui::Separator();
        ImGui::BeginChild("constellations", ImVec2(0, 0));
        a.tgMin[TgConst] = ImGui::GetWindowPos(); a.tgMax[TgConst] = ImVec2(a.tgMin[TgConst].x + ImGui::GetWindowSize().x, a.tgMin[TgConst].y + ImGui::GetWindowSize().y);
        constellationsTab(a);
        ImGui::EndChild();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::End();
}

// the classic interface's root window
static void classicShell(App& a, ImVec2 disp) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    txViewSwitch(a);   // RX / TX, top left
    if (hamViewFlag()) { ImGui::BeginChild("##hambody", ImVec2(0, 0)); hamPanel(a); ImGui::EndChild(); }
    else if (txViewFlag()) classicTx(a, disp);
    else classicRx(a, disp);
}

// the windows over the interface (passes, images, scanner, tours, feeds, map, rotator) and the per-frame ticks of the shared services
static void floatingWindows(App& a, ImVec2 disp) {
    passesWindow(a);
    schedulerWindow(a);
    recmgrWindow(a);
    satnogsWindow(a);
    dfWindow(a);
    passiveRadarWindow(a);
    rfiWindow(a);
    galleryWindow(a);
    scannerWindow(a);
    scanViewWindow(a);
    sweepWindow(a);
    stationHealthWindow(a);
    calibrateFrequencyDraw(a);
    memoryWindow(a);
    freqdbWindow(a);
    packsWindow(a);
    datasetsWindow(a);
    // the "add a radio" windows: here, not with the source controls, so they open whichever layout is up and from the setup wizard too
    netRadioWindow(a);
    synthGroupWindow(a);
    kiwiFindWindow(a);
    hamlibRigWindow(a);
    setupWizard(a, disp);
    listenWizard(a, disp);
    wizard(a, disp);
    lessonsUi(a, disp);
    if (gIdentifyDev && a.engine.running() && ImGui::GetFrameCount() > 120) { gIdentifyDev = false; identifyStart(a, radioCenterMhz(a) * 1e6, 0, 0, radioCenterMhz(a) * 1e6); }
    identifyWindow(a);
    dect2::PskReporter::get().setDialHz(a.freqMhz * 1e6);
    dect2::WsprNet::get().setDialHz(a.freqMhz * 1e6);
    a.engine.setWideDecode(a.wideDecode && modeSupportsWideHf(a.family));
    a.engine.setMultiChannel(a.mtvOn && a.family == 0, a.mtvMax, a.mtvGrid);
    feedsWindow(a);
    swarmWindow(a);
    automationWindow(a);
    protolabWindow(a);
    signallabWindow(a);
    modeBuilderFrame(a);
    modeBuilderWindow(a);
    txStudioWindow(a);
    pluginFrame(a);
    pluginManagerWindow(a);
    msgdbWindow(a);
    watchlistWindow(a);
    txTick(a);
    autodetTick(a);
    shareTick(a);
    remoteApiTick(a);
    mapWindow(a);
    propagationWindow(a);
    antennaDesignerWindow(a);
    rotatorTick(a);
    rotatorWindow(a);
}

// a modal note once per start when the last run crashed or the startup guard acted
static void crashNotePopup(ImVec2 disp) {
    if (!gCrashNote.empty()) {   // once per start; the same text is in the log panel
        // asked again until OK is pressed: another popup opened at the same level (the data-sharing consent) closes this one unanswered,
        // so it only opens while nothing else is up and the two dialogs take turns
        if (!ImGui::IsPopupOpen(trw("OnAir crashed")) &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::OpenPopup(trw("OnAir crashed"));
        ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(420 * gUi, 0), ImVec2(560 * gUi, 400 * gUi));
        if (ImGui::BeginPopupModal(trw("OnAir crashed"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(520 * gUi);
            ImGui::TextUnformatted(gCrashNote.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("OK"))) { gCrashNote.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
}

// --hidden: the pop-out video is a panel inside the main window (popout.cpp makes the real window otherwise)
static void hiddenPopOut(App& a) {
    if (a.popOut && gNoOsWindows) {   // --hidden: no OS window, the pop-out is a panel (popout.cpp makes the real window)
        firstWindowSize(640 * gUi, 380 * gUi);
        ImGui::SetNextWindowPos(ImVec2(200, 160), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        if (ImGui::Begin(trw("Video"), &a.popOut)) {
            if (a.video.has()) a.video.draw(ImGui::GetContentRegionAvail()); else ImGui::TextDisabled("%s", tr("no picture"));
            if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0)) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

void drawUI(App& a, ImVec2 disp, bool draw) {
    {   // the user's power choices (View > Power) to the policy the engine and the background threads read
        dect2::power::Settings ps;
        ps.lowPower = (dect2::power::LowPowerMode)std::clamp(a.powerLowMode, 0, 2);
        ps.saveWhenHidden = (dect2::power::HiddenPolicy)std::clamp(a.powerHidden, 0, 2);
        dect2::power::setSettings(ps);
    }
    tickReceiver(a);
    if (!draw) return;   // a receiver whose tab is not shown: ticked above, not drawn
    dockFrameBegin(a);   // View > Docking: the placement hook (ui2_dock.cpp)
    videoOnlyKeys(a);
    if (a.videoOnly) { videoOnlyWindow(a, disp); return; }
    applyUiTheme(a);
    if (modeacFullMap(a, disp) || adsbFullMap(a, disp)) return;   // the ADS-B map popped out over the whole window (in Mode A/C too, #235)
    if (adsbmap::fullDraw(&a, a.family, disp)) return;   // any other map screen: the tab it is on over the whole window (the "full screen" button on the map)
    if (a.touchUi) touch::shell(a, disp);   // the touch interface (ui/touch_shell.cpp) over either interface's panels
    else if (a.newUi) drawShell2(a, disp);
    else classicShell(a, disp);
    floatingWindows(a, disp);
    commandPalette(a);   // Ctrl+K (ui/command_palette.h)
    crashNotePopup(disp);
    hiddenPopOut(a);
    dockFrameEnd(a);
}
