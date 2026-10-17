#ifdef __linux__
#include <unistd.h>
#endif
// OnAir: window, main loop and the layout of the panels.
#include "app.h"
#include "contrast.h"
#include "dect2/iq_file.h"
#include "gallery.h"
#include "dect2/crash_report.h"
#include "discord_rpc.h"
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif

// The processor check, before anything else runs. The receiver library is built for AVX2 and FMA on Windows and Intel Macs (no runtime
// dispatch there), and the start-up code of its files (global objects) already uses AVX: a check in main() came too late, and a Mac Pro 5,1
// (Xeon without AVX) crashed with an illegal instruction before any message (issue #14). This file is built without AVX and its start-up
// code runs before the library's (the linker places the program's own objects first), so the check here comes in time.
#if defined(DECT2_REQUIRES_AVX2) && (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
namespace {
struct CpuGate {
    CpuGate() {
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return;
        plat::showFatalError("OnAir", "This version of OnAir needs a processor with AVX2 and FMA instructions (Intel from the 2013 \"Haswell\" "
                                      "generation on, AMD from about 2015). This computer's processor does not have them.");
        std::_Exit(1);
    }
} gCpuGate;
}
#endif

std::string gForceTab;
static std::string gCrashNote;   // set when the last run crashed: shown once in a small window

gfx::Backend* gGfx = nullptr;

GLFWwindow* gWindow = nullptr;

int gWinX = 100, gWinY = 100, gWinW = 1500, gWinH = 900;

ImU32 ttxColour(int c, float alpha) {
    static const ImVec4 col[8] = {ImVec4(0, 0, 0, 1), ImVec4(1, 0.1f, 0.1f, 1), ImVec4(0.1f, 1, 0.1f, 1), ImVec4(1, 1, 0.1f, 1),
                                  ImVec4(0.2f, 0.3f, 1, 1), ImVec4(1, 0.2f, 1, 1), ImVec4(0.1f, 1, 1, 1), ImVec4(1, 1, 1, 1)};
    ImVec4 v = col[c & 7]; v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// ---- programme guide helpers
int64_t utcNowOf(const App& a) { return a.ts.utcNow ? a.ts.utcNow : (int64_t)time(nullptr); }

void overviewTab(App& a) {
    if (!pal::dev()) {   // the new interface has these in its sidebar (Display)
    if (ImGui::Checkbox(tr("peak hold"), &a.peakHold)) savePrefs(a);
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::DragFloatRange2(tr("spectrum dB##specdb"), &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f");
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::DragFloatRange2(tr("waterfall dB##wfdb"), &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f")) a.wf.autoTrack = false;
    ImGui::SameLine(0, 4 * gUi); wfAutoButton(a);
    }
    spectrumWaterfall(a, specWaterfallSplit(), pal::dev());   // the new interface: the boundary is a divider you can drag, as in SDR++
}

void receiverTab(App& a) {
    if (const ModeUi* mu = modeUi(a.family)) if (mu->receiver) { mu->receiver(a); return; }
    if (a.dabMode) { dabEnsembleTab(a); return; }
    if (a.atsc3Mode) { atsc3ReceiverTab(a); return; }
    if (a.isdbtMode) { isdbtReceiverTab(a); return; }
    if (a.rx.standard == 2 || a.atscMode) {
        const AtscTelemetry& at = a.rx.atsc;
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("ATSC 8-VSB receiver: matched filter, pilot loop, symbol clock, field sync, per-field equaliser, trellis, Reed-Solomon")); ImGui::PopTextWrapPos(); }
        ImGui::Spacing();
        auto row = [&](const char* k, const char* fmt, auto... v) {   // a long value wraps under itself in a narrow tab
        ImGui::TextDisabled("%s", k); ImGui::SameLine(std::min(190 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
        row("pilot carrier", "%s", at.pilot ? "locked" : "searching");
        row("carrier offset", "%+.1f Hz", at.cfoHz);
        row("symbol clock offset", "%+.2f ppm", at.sroPpm);
        row("segment sync", "%s  (correlation %.2f)", at.segSync ? "locked" : "searching", at.syncQuality);
        row("field sync", "%s  (field %d)", at.fieldSync ? "locked" : "searching", at.fieldParity);
        row("equaliser", "%s, SNR %.1f dB from the known sync symbols", at.eqTrained ? "trained" : "not trained", at.snrDb);
        row("data symbols", "%.1f dB from the nearest of the 8 levels", at.dataSnrDb);
        row("fields decoded", "%llu", (unsigned long long)at.fields);
        row("Reed-Solomon", "%llu clean, %llu corrected, %llu failed", (unsigned long long)at.rsClean, (unsigned long long)at.rsCorrected, (unsigned long long)at.rsFailed);
        row("lock losses", "%llu (field sync misses %llu)", (unsigned long long)at.lockLosses, (unsigned long long)at.fieldSyncMisses);
        return;
    }
    subNav("rx", a.subRx, {"Sync", "Signalling", "FEC", "Frame map", "Channel"});
    switch (a.subRx) {
    case 0: syncTab(a); break;
    case 1: signallingTab(a); break;
    case 2: fecTab(a); break;
    case 3: frameMapTab(a); break;
    default: channelDashboard(a); break;
    }
}

void streamTab(App& a) {
    subNav("st", a.subStream, {"Outputs", "Transport stream"});
    if (a.subStream == 0) outputsTab(a); else tsTab(a);
}

// the old per-view names (for --tab in screenshots) map onto the new tabs and sub-views
void routeTab(App& a, const std::string& name) {
    struct R { const char* old; const char* tab; int tv, rx, st; };
    static const R map[] = {
        {"Player", "TV", 0, -1, -1}, {"Guide", "TV", 1, -1, -1}, {"Teletext", "TV", 2, -1, -1},
        {"Overview", "Overview", -1, -1, -1}, {"Spectrum", "Overview", -1, -1, -1}, {"Waterfall", "Overview", -1, -1, -1}, {"Constellations", "Overview", -1, -1, -1},
        {"Sync", "Receiver", -1, 0, -1}, {"Signalling", "Receiver", -1, 1, -1}, {"FEC", "Receiver", -1, 2, -1}, {"Frame map", "Receiver", -1, 3, -1},
        {"Channel", "Receiver", -1, 4, -1}, {"Impulse response", "Receiver", -1, 4, -1}, {"SNR per carrier", "Receiver", -1, 4, -1},
        {"Outputs", "Stream", -1, -1, 0}, {"TS", "Stream", -1, -1, 1}, {"Scan", "Scan", -1, -1, -1}, {"History", "History", -1, -1, -1}, {"Log", "History", -1, -1, -1}};
    for (auto& r : map) if (name == r.old) {
        gForceTab = r.tab;
        if (r.tv >= 0) a.subTv = r.tv;
        if (r.rx >= 0) a.subRx = r.rx;
        if (r.st >= 0) a.subStream = r.st;
        return;
    }
    gForceTab = name;
}

ImU32 scoreColour(double sc) {
    const float t = (float)std::min(1.0, std::max(0.0, sc / 100.0));
    return IM_COL32((int)(230 * (1 - t) + 40 * t), (int)(70 + 150 * t), (int)(60 + 30 * t), 255);
}

void mainTabs(App& a) {
    if (txViewFlag()) { txSpectrum(a); return; }
    // a narrow pane: the tabs get less padding (the bar takes it from the style when it begins, each tab when it is added), so that they keep
    // their whole names instead of being cut short or scrolled out of view
    gTightTabs = ImGui::GetContentRegionAvail().x < 760 * gUi;
    if (gTightTabs) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * gUi, ImGui::GetStyle().FramePadding.y));
    const bool bar = ImGui::BeginTabBar("tabs", pal::dev() ? ImGuiTabBarFlags_DrawSelectedOverline : 0);
    if (gTightTabs) ImGui::PopStyleVar();
    if (bar) {
        if (tabItem(N_("Overview"), Ic::Grid)) { overviewTab(a); ImGui::EndTabItem(); }
        if (const ModeUi* mu = modeUi(a.family)) {   // a mode added after FM: its own tabs
            if (mu->tab && tabItem(mu->tabName ? mu->tabName : "Mode", mu->tabIcon)) { mu->tab(a); ImGui::EndTabItem(); }
            else if (!mu->tab && mu->stream && tabItem(N_("TV"), Ic::Tv)) { tvTab(a); ImGui::EndTabItem(); }
            if (mu->receiver && tabItem(N_("Receiver"), Ic::Antenna)) { receiverTab(a); ImGui::EndTabItem(); }
            if (mu->stream && tabItem(N_("Stream"), Ic::Layers)) { streamTab(a); ImGui::EndTabItem(); }
            if (mu->scan && tabItem(N_("Scan"), Ic::Scan)) { scanTab(a); ImGui::EndTabItem(); }
            if (tabItem(N_("History"), Ic::Chart)) { historyLogTab(a); ImGui::EndTabItem(); }
            secondModeTabs(a);
            ImGui::EndTabBar();
            return;
        }
        if (a.dabMode) { if (tabItem(N_("Radio"), Ic::Radio)) { dabRadioTab(a); ImGui::EndTabItem(); } }
        else if (a.fmMode) { if (tabItem(N_("Radio"), Ic::Radio)) { fmRadioTab(a); ImGui::EndTabItem(); } }
        else if (tabItem(N_("TV"), Ic::Tv)) { tvTab(a); ImGui::EndTabItem(); }
        if (!a.fmMode && tabItem(a.dabMode ? N_("Ensemble") : N_("Receiver"), Ic::Antenna)) { receiverTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("Map"), Ic::Compass)) { dabTransmittersTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("Analyzer"), Ic::Grid)) { dabAnalyzerTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && dabHasJournaline(a.engine.dabEnsemble()) && tabItem(N_("Journaline"), Ic::Doc)) { dabJournalineTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("FIC"), Ic::Doc)) { dabFicTab(a); ImGui::EndTabItem(); }
        if (!a.dabMode && !a.fmMode && tabItem(N_("Stream"), Ic::Layers)) { streamTab(a); ImGui::EndTabItem(); }
        if (tabItem(N_("Scan"), Ic::Scan)) { scanTab(a); ImGui::EndTabItem(); }
        if (!a.fmMode && tabItem(N_("Antenna"), Ic::Compass)) { antennaTab(a); ImGui::EndTabItem(); }
        if (tabItem(N_("History"), Ic::Chart)) { historyLogTab(a); ImGui::EndTabItem(); }
        secondModeTabs(a);
        ImGui::EndTabBar();
    }
}

static bool anyRunning() { for (App* r : receiversAll()) if (r->engine.running()) return true; return false; }
static bool gIdentifyDev = false;   // --identify

void drawUI(App& a, ImVec2 disp, bool draw) {
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
    gainControl(a);
    followBandwidth(a);
    feedDirection(a);
    a.video.update(a.engine.player());
    subsUpdate(a);   // subtitle services, the teletext / subtitle decoders' PIDs, the overlay
    if (a.playReq >= 0) for (auto& sv : a.ts.services) if (sv.id == a.playReq && sv.havePmt) { a.engine.player().select(sv.id); a.engine.player().setVolume(a.volume); a.playReq = -1; break; }
    if (!draw) return;   // a receiver whose tab is not shown: ticked above, not drawn
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            if (a.videoOnly) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
            else if (a.video.has()) { a.videoOnly = true; if (!glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        }
        if (a.videoOnly && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
    }
    if (a.videoOnly) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(disp);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::Begin("##vonly", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (a.video.has()) a.video.draw(disp); else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("no picture - press Esc")); ImGui::PopTextWrapPos(); }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0)) { a.videoOnly = false; if (glfwGetWindowMonitor(gWindow)) toggleFullscreen(); }
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    applyUiTheme(a);
    if (adsbFullMap(a, disp)) return;   // the ADS-B map popped out over the whole window
    if (a.newUi) drawShell2(a, disp);
    else {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    txViewSwitch(a);   // RX / TX, top left
    if (txViewFlag()) {
        txBar(a);
        standardSwitch(a);
        ImGui::Separator();
        float rightW = 350 * gUi;
        float mainH = ImGui::GetContentRegionAvail().y - 6;
        ImGui::BeginChild("main", ImVec2(disp.x - rightW - 16 * gUi, mainH));
        txSpectrum(a);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, mainH));
        txRight(a);
        ImGui::EndChild();
        ImGui::End();
    } else {

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
    if (ImGui::SmallButton(tr("Tour"))) { a.wizOpen = true; a.wizX = -1; a.wizStep = 0; a.wizStepT = ImGui::GetTime(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Take the guided tour with Onny"));
    a.tgMin[TgSwitch] = ImVec2(sw0.x - 2, sw0.y - 2); a.tgMax[TgSwitch] = ImVec2(sw0.x + gSwitchWidth + 20, ImGui::GetCursorScreenPos().y);
    ImGui::Separator();
    statusBar(a);
    ImGui::Separator();

    float logH = (a.atsc3Mode ? 0 : 250) * gUi, rightW = 350 * gUi;   // ATSC 3.0 has no constellation panels yet
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
    }
    passesWindow(a);
    galleryWindow(a);
    scannerWindow(a);
    // the "add a radio" windows: here, not with the source controls, so they open whichever layout is up and from the setup wizard too
    netRadioWindow(a);
    hamlibRigWindow(a);
    setupWizard(a, disp);
    wizard(a, disp);
    if (gIdentifyDev && a.engine.running() && ImGui::GetFrameCount() > 120) { gIdentifyDev = false; identifyStart(a, radioCenterMhz(a) * 1e6, 0, 0, radioCenterMhz(a) * 1e6); }
    identifyWindow(a);
    dect2::PskReporter::get().setDialHz(a.freqMhz * 1e6);
    dect2::WsprNet::get().setDialHz(a.freqMhz * 1e6);
    a.engine.setWideDecode(a.wideDecode && modeSupportsWideHf(a.family));
    a.engine.setMultiChannel(a.mtvOn && a.family == 0, a.mtvMax, a.mtvGrid);
    feedsWindow(a);
    txTick(a);
    shareTick(a);
    remoteApiTick(a);
    mapWindow(a);
    rotatorTick(a);
    rotatorWindow(a);
    if (!gCrashNote.empty()) {   // once per start; the same text is in the log panel
        static bool opened = false;
        if (!opened) { ImGui::OpenPopup(trw("OnAir crashed")); opened = true; }
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
    if (a.popOut && gNoOsWindows) {   // --hidden: no OS window, the pop-out is a panel (popout.cpp makes the real window)
        ImGui::SetNextWindowSize(ImVec2(640 * gUi, 380 * gUi), ImGuiCond_FirstUseEver);
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

// View > Light: each colour the interface drew gets its lightness turned over (HSL: L -> 1 - L) with hue and saturation kept, so dark
// panes turn white, light text dark, and red, amber and green stay themselves. Done on the finished draw lists, so every panel of every
// mode follows without colours of its own. Only what is drawn from the font atlas (text, shapes, plots) is turned: pictures, map tiles
// and other textures keep their colours. Per channel c' = c + (255 - max - min), which keeps max - min and the hue.
// Light and High contrast also check the text: a glyph's vertices (not the solid shapes, whose texture coordinate is the atlas' white pixel)
// get a colour that holds the contrast ratio of contrast.h on that theme's panes (Light: 5:1 on white after the turn-over, so the amber and
// green status words and the dim grey do not fade; High contrast: 7:1 on black).
static void lightenDrawData(ImDrawData* dd, bool light = true, bool highContrast = false) {
    if (!dd) return;
    static std::vector<unsigned char> keep;
    const ImVec2 wp = ImGui::GetFontTexUvWhitePixel();
    for (ImDrawList* dl : dd->CmdLists) {
        const int nv = dl->VtxBuffer.Size;
        keep.assign((size_t)nv, 0);
        for (const ImDrawCmd& c : dl->CmdBuffer) {
            if (c.UserCallback == nullptr && c.TexRef._TexData != nullptr) continue;   // the font atlas
            for (unsigned i = 0; i < c.ElemCount; i++) {
                const unsigned v = c.VtxOffset + dl->IdxBuffer[(int)(c.IdxOffset + i)];
                if (v < (unsigned)nv) keep[v] = 1;
            }
        }
        for (int i = 0; i < nv; i++) {
            if (keep[(size_t)i]) continue;
            ImU32& col = dl->VtxBuffer[i].col;
            const int r = (int)(col >> IM_COL32_R_SHIFT) & 255, g = (int)(col >> IM_COL32_G_SHIFT) & 255, b = (int)(col >> IM_COL32_B_SHIFT) & 255;
            const int d = light ? 255 - std::max(r, std::max(g, b)) - std::min(r, std::min(g, b)) : 0;
            auto cl = [](int x) { return (ImU32)std::clamp(x, 0, 255); };
            int nr = (int)cl(r + d), ng = (int)cl(g + d), nb = (int)cl(b + d);
            const ImDrawVert& vt = dl->VtxBuffer[i];
            const bool glyph = std::fabs(vt.uv.x - wp.x) > 1e-5f || std::fabs(vt.uv.y - wp.y) > 1e-5f;
            if (glyph) {   // text (and the textured lines): the legible colour, remembered for the next vertex of the same colour
                static int ir = -1, ig = -1, ib = -1, orr = 0, og = 0, ob = 0;
                static bool lastLight = false, lastHc = false;
                if (nr != ir || ng != ig || nb != ib || light != lastLight || highContrast != lastHc) {
                    const contrast::Rgb c{nr / 255.f, ng / 255.f, nb / 255.f};
                    const contrast::Rgb o = highContrast ? contrast::legibleOnDark(c) : contrast::legibleOnLight(c);
                    ir = nr; ig = ng; ib = nb; lastLight = light; lastHc = highContrast;
                    orr = (int)std::lround(o.r * 255); og = (int)std::lround(o.g * 255); ob = (int)std::lround(o.b * 255);
                }
                nr = orr; ng = og; nb = ob;
            }
            col = (col & IM_COL32_A_MASK) | (cl(nr) << IM_COL32_R_SHIFT) | (cl(ng) << IM_COL32_G_SHIFT) | (cl(nb) << IM_COL32_B_SHIFT);
        }
    }
}

bool gWayland = false;

#if defined(__linux__)
// Linux: X11 (XWayland on a Wayland desktop) unless asked for Wayland.
// Crash report from 0.2.2 (Ubuntu 26.04, GLFW 3.4 on Wayland, "after some time"): SIGSEGV reading 0x40 in wl_proxy_get_version(NULL), called
// from GLFW's event loop (glfwPollEvents / glfwWaitEventsTimeout -> handleEvents -> the cursor timer -> setCursorImage). 0x40 is where
// libwayland keeps a proxy's version: GLFW sent a request on a NULL wl_pointer. GLFW 3.4 animates every theme cursor that has a frame delay
// on a timer while the pointer is over the window; when the seat loses its pointer (the mouse sleeps or is unplugged, a KVM switch, the
// screen locks or turns off) it destroys the wl_pointer and sets it to NULL, but neither disarms the timer nor forgets the window under the
// pointer, so the next tick calls wl_pointer_set_cursor(NULL, ...). That is inside GLFW and cannot be caught from here, so:
// - by default OnAir asks a GLFW that has both (3.4 and later) for X11 when an X server is there (every GNOME / KDE Wayland session runs
//   XWayland); the X11 code has none of this
// - ONAIR_DISPLAY=wayland (or --wayland, or the "displayServer" setting) keeps native Wayland; ONAIR_DISPLAY=x11 / --x11 forces X11
// - on native Wayland ImGui does not change the cursor shape (ImGuiConfigFlags_NoMouseCursorChange): with the default cursor GLFW's timer
//   finds no cursor of OnAir's to animate, which avoids the crash at the cost of resize / text cursors
// The functions are looked up at run time: the build may have the GLFW 3.3 headers, the library on the user's system decides.
constexpr int kGlfwPlatform = 0x00050003, kGlfwAnyPlatform = 0x00060000, kGlfwPlatformWayland = 0x00060003, kGlfwPlatformX11 = 0x00060004;
static void chooseGlfwPlatform(int argc, char** argv) {
    std::string want = plat::prefs().getS("displayServer", "auto");
    if (const char* e = getenv("ONAIR_DISPLAY")) if (*e) want = e;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--wayland") want = "wayland";
        if (std::string(argv[i]) == "--x11") want = "x11";
    }
    auto supported = (int (*)(int))dlsym(RTLD_DEFAULT, "glfwPlatformSupported");
    if (!supported) return;   // GLFW 3.3: one platform per library
    const char* wd = getenv("WAYLAND_DISPLAY");
    const char* xd = getenv("DISPLAY");
    const bool wl = wd && *wd, x11 = xd && *xd;
    if (want == "wayland") {
        if (supported(kGlfwPlatformWayland)) glfwInitHint(kGlfwPlatform, kGlfwPlatformWayland);
    } else if ((want == "x11" || (wl && x11)) && supported(kGlfwPlatformX11)) {
        glfwInitHint(kGlfwPlatform, kGlfwPlatformX11);
        if (wl) fprintf(stderr, "display: X11 (XWayland) on a Wayland desktop; ONAIR_DISPLAY=wayland for native Wayland\n");
    }
}
static bool glfwOnWayland() {
    if (auto getPlatform = (int (*)())dlsym(RTLD_DEFAULT, "glfwGetPlatform")) return getPlatform() == kGlfwPlatformWayland;
    auto display = (void* (*)())dlsym(RTLD_DEFAULT, "glfwGetWaylandDisplay");   // GLFW 3.3 built for Wayland
    return display && display();
}
#endif

static discord::Rpc* gDiscord = nullptr;   // created on first use (View > Show in Discord)
static void discordTick(const App& a) {   // posts the wanted Rich Presence; the thread does the talking
    if (!a.discordOn || !*discord::appId()) { if (gDiscord) { discord::Presence off; gDiscord->set(off); } return; }
    if (!gDiscord) gDiscord = new discord::Rpc();
    discord::Presence p; p.enabled = true; p.running = a.engine.running();
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) { p.mode = kModes[i].name; break; }
    const int kind = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? (int)a.devices[a.devIdx].kind : -1;
    if (kind == (int)DeviceInfo::File) p.freq = "File playback";
    else if (kind == (int)DeviceInfo::Synthetic) p.freq = "Synthetic signal";
    else if (a.discordFreq) { char b[32]; snprintf(b, sizeof b, "%.3f MHz", a.freqMhz); p.freq = b; }
    static discord::Presence prev; static long long start = 0;
    if (!start || prev.running != p.running || prev.mode != p.mode || prev.freq != p.freq)
        start = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    prev = p; p.startUnix = start;
    gDiscord->set(p);
}

int main(int argc, char** argv) {
    dect2::crash::install();   // first: a crash from here on leaves a report
#ifdef __linux__
    if (geteuid() == 0) fprintf(stderr, "OnAir: running as root is not needed; see docs for udev rules\n");
#endif
#ifdef _WIN32
    dect2::crash::startLog();   // the program has no console window: keep its messages in a log file next to the settings (the old one is kept as onair-prev.log)
#endif
    {
        std::string report;
        if (dect2::crash::lastRunCrashed(report)) {
#ifdef _WIN32
            gCrashNote = "The last run of OnAir crashed; the report is in " + report + " - please send it with onair-prev.log";
#else
            gCrashNote = "The last run of OnAir crashed; the report is in " + report + " - please send it";
#endif
            fprintf(stderr, "%s\n", gCrashNote.c_str());
        }
    }
    if (!dect2::cpuSupportsBuild()) {
        plat::showFatalError("OnAir", "This version of OnAir needs a processor with AVX2 and FMA instructions (any Intel or AMD processor from about 2013 on).");
        return 1;
    }
#if defined(__linux__)
    chooseGlfwPlatform(argc, argv);
    if (!glfwInit()) {   // X11 asked for and no X server after all: whatever GLFW finds
        glfwInitHint(kGlfwPlatform, kGlfwAnyPlatform);
        glfwInit();
    }
    gWayland = glfwOnWayland();
    if (gWayland) fprintf(stderr, "display: native Wayland (window positions are left to the compositor; the cursor shape stays the default)\n");
#else
    glfwInit();
#endif
    gfx::windowHints();
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);   // a fullscreen main window stays up when another window or monitor is clicked (#71)
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--hidden") { glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); gNoOsWindows = true; }   // dev: with --shot, render without showing the window
    GLFWwindow* window = glfwCreateWindow(1500, 900, "OnAir", nullptr, nullptr);
    if (!window) return 1;
#ifndef __APPLE__
    {   // the default size is in pixels: make the window as large (in points) as on a 96 dpi display, but never larger than the screen
        // (a 1366 x 768 laptop at 100 % is common: a 1500 x 900 window would hang off its bottom and right edges, hiding the status bar)
        float xs = 1, ys = 1;
        glfwGetWindowContentScale(window, &xs, &ys);
        const float s = std::max(1.f, std::max(xs, ys));
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (GLFWmonitor* mon = primaryMonitor()) glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
        int fl = 0, ft = 0, fr = 0, fb = 0;   // the frame and title bar around the window's content
        glfwGetWindowFrameSize(window, &fl, &ft, &fr, &fb);
        int w = (int)(1500 * s), h = (int)(900 * s);
        if (ww > 0 && wh > 0) { w = std::max(640, std::min(w, ww - fl - fr)); h = std::max(480, std::min(h, wh - ft - fb)); }
        if (w != 1500 || h != 900) {
            glfwSetWindowSize(window, w, h);
            if (ww > 0 && wh > 0 && !gWayland) {   // and wholly on the screen (Wayland: the compositor places it)
                int x = 0, y = 0;
                glfwGetWindowPos(window, &x, &y);
                x = std::max(wx + fl, std::min(x, wx + ww - fr - w));
                y = std::max(wy + ft, std::min(y, wy + wh - fb - h));
                glfwSetWindowPos(window, x, y);
            }
        }
    }
#endif
    if (const char* e = getenv("DECT2_WINSIZE")) { int w = 0, h = 0; if (sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) glfwSetWindowSize(window, w, h); }   // dev: try a small screen
    gWindow = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
#ifdef DECT2_UI_LAYOUT_CHECK
    void layoutCheckInstall();   // layout_check.cpp (developer build)
    layoutCheckInstall();
#endif
    gfx::Backend* gfxBackend = gfx::create(window);
    if (!gfxBackend) { fprintf(stderr, "could not start the graphics back end\n"); return 1; }
    gGfx = gfxBackend;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyTheme();
    static ImGuiStyle baseStyle = ImGui::GetStyle();   // the style at scale 1

    App app;
    ImFont* ui = nullptr;
    app.mono = nullptr;
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (ui = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break; // the whole UI is monospaced
    for (const auto& f : plat::monoFontCandidates()) if (access(f.c_str(), R_OK) == 0 && (app.mono = io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f))) break;
    if (!ui) ui = io.Fonts->AddFontDefault();
    // CJK (Korean/Japanese/Chinese): merge every system font that loads (Menlo has no Hangul; DRM Journaline needs it).
    for (const auto& f : plat::cjkFontCandidates()) {
        if (access(f.c_str(), R_OK) != 0) continue;
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.DstFont = ui;
        if (!io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f, &mc)) continue;
        if (app.mono && app.mono != ui) {
            ImFontConfig mm;
            mm.MergeMode = true;
            mm.DstFont = app.mono;
            io.Fonts->AddFontFromFileTTF(f.c_str(), 13.0f, &mm);
        }
    }
    app.ui = ui;
    if (!app.mono) app.mono = ui;

    ImGui_ImplGlfw_InitForOther(window, true);
    glfwSetDropCallback(window, [](GLFWwindow*, int n, const char** paths) { if (n > 0) wbQueueDrop(paths[0]); });   // a recording dropped on the window: the Signal workbench opens it
    watchMonitors();
    if (gWayland) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // see chooseGlfwPlatform()

    app.wf.init(gfxBackend);
    // startup guard: a start that keeps crashing (a saved setting that breaks it, #59) must not repeat forever. After one unfinished
    // start the other interface is tried (the classic and the new one share little drawing code); after two in a row the saved
    // settings are set aside and OnAir starts with the defaults
    // a start with arguments (the dev and test runs: --shot, --hidden ...) is left out: it neither changes the settings nor counts
    const int failedStarts = argc > 1 ? 0 : dect2::crash::failedStarts();
    if (argc > 1) dect2::crash::startupDone();
    std::string guardNote;
    if (failedStarts >= 2) {
        const std::string kept = plat::prefs().setAside();
        guardNote = "OnAir did not finish starting " + std::to_string(failedStarts) + " times in a row, so it starts with the default settings this time. "
                  + (kept.empty() ? std::string("The old settings could not be kept.") : "The old settings are kept in " + kept + ".");
    }
    loadPrefs(app);
    if (failedStarts == 1) {
        app.newUi = !app.newUi;
        guardNote = std::string("OnAir did not finish starting last time, so it opens with the ") + (app.newUi ? "new" : "classic")
                  + " interface this time. The other one is under View (new interface) or the \"New interface\" button (classic).";
    }
    if (!guardNote.empty()) {
        fprintf(stderr, "%s\n", guardNote.c_str());
        gCrashNote = gCrashNote.empty() ? guardNote : gCrashNote + "\n\n" + guardNote;
    }
    app.wizOpen = argc == 1 && !plat::prefs().getB("wizardDone", false);   // the tour runs on the very first start only
    if (argc == 1 && !plat::prefs().getB("setupDone", false)) { app.wizOpen = false; setupWizardOpen(app); }   // the setup comes first; the tour follows it
    if (getenv("DECT2_WIZARD")) app.wizOpen = true;
    if (getenv("DECT2_SETUP")) { app.wizOpen = false; setupWizardOpen(app); if (const char* ss = getenv("DECT2_SETUPSTEP")) app.setupStep = atoi(ss); }
    app.wizStepT = ImGui::GetTime();
    app.engine.player().setHardwareDecode(plat::prefs().getB("hwVideo", true));
    app.tune.synth.demoTv = true;   // the synthetic DVB-T / ATSC signal carries a test-card programme
    if (const char* ws = getenv("DECT2_WIZSTEP")) wizEnter(app, atoi(ws));
    app.engine.log("OnAir started");
    if (!gCrashNote.empty()) app.engine.log(gCrashNote);
    refreshDevices(app);
    for (int i = 0; i < (int)app.devices.size(); i++)
        if (app.devices[i].isRadio()) { app.devIdx = i; break; }
    if (app.devices.size() == 2) {
#ifdef _WIN32
        app.engine.log("no radio found - plug it in and press the refresh button next to the source. A HackRF needs the WinUSB driver on Windows: install it once with Zadig (zadig.akeo.ie), choosing the HackRF and the driver WinUSB. Until then use the synthetic signal or an IQ file.");
#else
        app.engine.log("no radio connected - use the synthetic signal or an IQ file");
#endif
    }
    if (getenv("DECT2_DEMO")) { wizAction(app, 2); }   // test: start the demo programme straight away
    // the decoded results of the last run (each mode's lists, until its Clear), then saved every 30 s and at exit
    app.engine.setResultsDir(plat::dataDir() + "/results");
    appResultsLoad(app);
    receiversInit(app);   // RX 2 ... that were open at the last exit

    bool autostart = false, hackrfStart = false, fileStart = false, stress = false;
    std::string startDevice;   // exact source name: select an RSPduo input without relying on enumeration order
    int stressFrame = 0, stressStep = 0;
    const char* shotPath = nullptr;
    double shotAfter = 0;      // dev: take the --shot after this many seconds instead of after a number of frames
    bool fmScanAuto = false;   // dev: start the FM band scan as soon as the receiver runs
    int shotFrame = 0;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--autostart") autostart = true;
        if (std::string(argv[i]) == "--device" && i + 1 < argc) { startDevice = argv[++i]; autostart = true; }
        if (std::string(argv[i]) == "--stress") stress = true;   // dev: retune frequency and gain every second, like moving the controls
        if (std::string(argv[i]) == "--shot" && i + 1 < argc) shotPath = argv[++i];
        if (std::string(argv[i]) == "--shotdelay" && i + 1 < argc) shotAfter = atof(argv[++i]);
        if (std::string(argv[i]) == "--fmscan") fmScanAuto = true;
        if (std::string(argv[i]) == "--passes") app.passesOpen = true;
        if (std::string(argv[i]) == "--images") app.galleryOpen = true;   // dev: open View > Images
        if (std::string(argv[i]) == "--hamlib") app.hamlibAddOpen = true;   // dev: open the Add Hamlib rig window
        if (std::string(argv[i]) == "--map") app.mapOpen = true;   // dev: open View > Map
        if (std::string(argv[i]) == "--scanner") app.scannerOpen = true;   // dev: open View > Scanner   // dev: open View > Passes
        if (std::string(argv[i]) == "--tle" && i + 1 < argc) passesDevTle(argv[++i]);   // dev: TLEs from a file for the pass planner (no download)
        if (std::string(argv[i]) == "--rx2") addReceiver();   // dev: a second receiver at once (no warning dialog)
        if (std::string(argv[i]) == "--mute") { app.muted = true; app.volume = 0; }
        if (std::string(argv[i]) == "--wbfile" && i + 1 < argc) {   // dev: the Signal workbench opens this recording (--wbrate for a raw file, --wbtab, --wbsel, --wbkind, --wbbaud, --wbrun)
            double rate = 0; int tab = 0;
            for (int k = 1; k + 1 < argc; k++) { if (std::string(argv[k]) == "--wbrate") rate = atof(argv[k + 1]); if (std::string(argv[k]) == "--wbtab") tab = atoi(argv[k + 1]); }
            wbDevOpen(argv[i + 1], rate, tab);
            setFamily(app, 73);
            routeTab(app, "Workbench");
            i++;
        }
        if (std::string(argv[i]) == "--wbsel" && i + 4 < argc) { wbDevSelect(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4])); i += 4; }   // dev: time range (s) and band (Hz from the centre; 0 0: none)
        if (std::string(argv[i]) == "--wbdemod" && i + 3 < argc) { const std::string r = argv[i + 3]; wbDevDemod(atoi(argv[i + 1]), atof(argv[i + 2]), r.find('m') != std::string::npos, r.find('d') != std::string::npos); i += 3; }   // dev: demodulator number, baud, "m" measure and / or "d" demodulate
        if (std::string(argv[i]) == "--identify") gIdentifyDev = true;   // dev: identify the strongest signal once the receiver runs (with --shot)
        if (std::string(argv[i]) == "--tab" && i + 1 < argc) routeTab(app, argv[++i]);
        if (std::string(argv[i]) == "--second" && i + 2 < argc) {   // dev: a second mode (the mode-list row named so) at a frequency in MHz
            const std::string nm = argv[++i];
            const double mhz = atof(argv[++i]);
            for (int k = 0; k < kNumModes; k++) if (nm == kModes[k].name) { setSecondMode(app, k, mhz); break; }
        }
        if (std::string(argv[i]) == "--dvb") setFamily(app, 0);
        if (std::string(argv[i]) == "--atsc") setFamily(app, 1);
        if (std::string(argv[i]) == "--atsc3") setFamily(app, 3);
        if (std::string(argv[i]) == "--isdbt") setFamily(app, 4);
        if (std::string(argv[i]) == "--dab") setFamily(app, 2);
        if (std::string(argv[i]) == "--fm") setFamily(app, 5);
        if (std::string(argv[i]) == "--mode" && i + 1 < argc) { const char* id = argv[++i]; if (const SubMode* sm = subModeById(id)) setMode(app, sm->stdMode - 2, subIndexOf(id)); else if (const ModeTuning* mt = modeTuningById(id)) setFamily(app, mt->stdMode - 2); }   // dev: rtty, ft8, meshtastic, pocsag, navtex ... (the protocol entries), dvbs, dtmb, atv, dmr, drm, adsb, gnss, sonde, ais, marine, acars, inmc, aero, iridium, mesh, hdr, cdr, pager, packet, hfdig, airband, vdl2, ism, vor, ils, m17, dstar, cw, hfdl, nxdn, dpmr, tetra, psk, js8 or ale, psk or js8
        if (std::string(argv[i]) == "--sopt" && i + 2 < argc) { const int k = atoi(argv[i + 1]); if (k >= 0 && k < 8) app.tune.synth.modeOpt[k] = atoi(argv[i + 2]); i += 2; }       // dev: option k of the mode's test signal
        if (std::string(argv[i]) == "--sval" && i + 2 < argc) { const int k = atoi(argv[i + 1]); if (k >= 0 && k < 4) app.tune.synth.modeVal[k] = atof(argv[i + 2]); i += 2; }   // dev: value k of the mode's test signal
        if (std::string(argv[i]) == "--pick" && i + 1 < argc) {   // dev: pick the mode-list row named so, else the first row the search finds, as a click would
            const char* q = argv[++i];
            int pick = -1;
            auto same = [](const char* x, const char* y) { for (; *x && *y; x++, y++) if (tolower((unsigned char)*x) != tolower((unsigned char)*y)) return false; return *x == *y; };
            for (int m = 0; m < kNumModes && pick < 0; m++) if (same(kModes[m].name, q)) pick = m;
            for (int m = 0; m < kNumModes && pick < 0; m++) if (modeMatches(kModes[m], q)) pick = m;
            if (pick >= 0) selectMode(app, kModes[pick].family, kModes[pick].subId);
        }
        if (std::string(argv[i]) == "--ui2") app.newUi = true;
        if (std::string(argv[i]) == "--lang" && i + 1 < argc) i18n::set(argv[++i]);   // dev: the interface in this language (not saved)
        if (std::string(argv[i]) == "--theme" && i + 1 < argc) app.uiTheme = atoi(argv[++i]);
        if (std::string(argv[i]) == "--light") app.lightUi = true;   // dev: View > Theme > Light
        if (std::string(argv[i]) == "--hc") app.highContrast = true;   // dev: View > Theme > High contrast
        if (std::string(argv[i]) == "--variant" && i + 1 < argc) app.uiVariant = std::max(0, std::min(7, atoi(argv[++i])));
        if (std::string(argv[i]) == "--classic") app.newUi = false;
        if (std::string(argv[i]) == "--rate" && i + 1 < argc) app.file.sampleRate = atof(argv[++i]) * 1e6;
        if (std::string(argv[i]) == "--freq" && i + 1 < argc) app.freqMhz = atof(argv[++i]);
        if (std::string(argv[i]) == "--station" && i + 1 < argc) app.dabStation = atoi(argv[++i]);
        if (std::string(argv[i]) == "--constview" && i + 1 < argc) app.constView = atoi(argv[++i]);
        if (std::string(argv[i]) == "--synthgain") app.tune.synth.gainModel = true;
        if (std::string(argv[i]) == "--dvbt") { app.tune.synth.dvbt = true; app.tune.synth.snrDb = 28; }
        if (std::string(argv[i]) == "--t2only") app.stdMode = 1;
        if (std::string(argv[i]) == "--dvbtonly") app.stdMode = 2;
        if (std::string(argv[i]) == "--nodeint") app.video.deint = false;
        if (std::string(argv[i]) == "--fakeepg") app.fakeEpg = true;
        if (std::string(argv[i]) == "--agc") app.agcOn = true;
        if (std::string(argv[i]) == "--fft" && i + 1 < argc) app.tune.synth.tx.s2field1 = atoi(argv[++i]);
        if (std::string(argv[i]) == "--gi" && i + 1 < argc) app.tune.synth.tx.giIdx = atoi(argv[++i]);
        if (std::string(argv[i]) == "--cfo" && i + 1 < argc) app.tune.synth.cfoHz = atof(argv[++i]);
        if (std::string(argv[i]) == "--snr" && i + 1 < argc) app.tune.synth.snrDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echo" && i + 1 < argc) app.tune.synth.echoDb = atof(argv[++i]);
        if (std::string(argv[i]) == "--echodelay" && i + 1 < argc) app.tune.synth.echoDelay = atoi(argv[++i]);
        if (std::string(argv[i]) == "--pp" && i + 1 < argc) app.tune.synth.tx.pp = atoi(argv[++i]);
        if (std::string(argv[i]) == "--ext") app.tune.synth.tx.ext = true;
        if (std::string(argv[i]) == "--hackrf") hackrfStart = true;
        if (std::string(argv[i]) == "--file" && i + 1 < argc) { app.file.path = argv[++i]; app.file.sampleRate = 10e6; app.file.format = FileFormat::CS8;
            { IqFileInfo info; std::string perr;
              if (probeIqFile(app.file.path, info, perr)) { app.file.path = info.dataPath; app.file.format = info.format; app.file.bigEndian = info.bigEndian; app.file.offsetBytes = info.offsetBytes; app.file.endBytes = info.endBytes; if (info.sampleRate > 0) app.file.sampleRate = info.sampleRate; if (info.centerHz > 0) app.freqMhz = info.centerHz / 1e6; }
              else if (!perr.empty()) fprintf(stderr, "%s\n", perr.c_str()); }
            fileStart = true; hackrfStart = true; autostart = true; }
        if (std::string(argv[i]) == "--play" && i + 1 < argc) app.playReq = atoi(argv[++i]);
        if (std::string(argv[i]) == "--lna" && i + 1 < argc) app.tune.lnaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--vga" && i + 1 < argc) app.tune.vgaDb = atoi(argv[++i]);
        if (std::string(argv[i]) == "--amp") app.tune.ampOn = true;
    }
    double last = glfwGetTime();
    double frameNext = 0, lastInputT = 0;
    while (!glfwWindowShouldClose(window)) {
        appResultsSave(app);   // every 30 s: the lists the screens keep themselves (the engine saves its own on a thread)
        discordTick(app);
        {
            {   // frame pacing: the screen is only redrawn as often as it can change, otherwise the loop would run at the refresh rate of the display (120 Hz on
                // a ProMotion Mac) and draw the same spectrum three times over. Input wakes the loop at once; --shot keeps the unpaced loop its frame counts rely on.
                const double t = glfwGetTime();
                const bool shown = glfwGetWindowAttrib(window, GLFW_VISIBLE) && !glfwGetWindowAttrib(window, GLFW_ICONIFIED);
                const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
                double interval = 0;
                if (!shotPath) {
                    if (!shown && !popOutShown() && !gNoOsWindows) interval = app.reduceCpuHidden ? 0.1 : 0.25;                                                  // the pop-out window keeps playing
                    else if (t - lastInputT < 1.0 || app.engine.player().selected() >= 0) interval = 1.0 / 60;      // typing, dragging, video
                    else if (anyRunning()) interval = focused ? 1.0 / 30 : 1.0 / 15;                          // spectrum and waterfall
                    else interval = focused ? 0.1 : 0.25;                                                          // nothing is moving
                }
                if (interval > 0 && frameNext > t) {
                    glfwWaitEventsTimeout(frameNext - t);
                    if (glfwGetTime() < frameNext - 0.002) lastInputT = glfwGetTime();                               // woken early: an event
                }
                frameNext = glfwGetTime() + interval;
            }
            glfwPollEvents();
            wbDropTick(app);
            updateTick(app);
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            const bool skipDraw = !shotPath && !gNoOsWindows && app.reduceCpuHidden && !glfwGetWindowAttrib(window, GLFW_VISIBLE);
            if (skipDraw || w == 0 || h == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                // minimised: nothing to draw in the main window, but the pop-out video window stays and keeps playing
                if (!popOutShown()) { glfwWaitEventsTimeout(0.1); continue; }
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                app.video.update(app.engine.player());
                popOutFrame(app);
                ImGui::EndFrame();
                popOutPresent();
                continue;
            }
            static const float kClear[4] = {0.04f, 0.045f, 0.05f, 1.f}, kClearLight[4] = {0.955f, 0.95f, 0.96f, 1.f};
            static const float kClearHc[4] = {0.f, 0.f, 0.f, 1.f};
            gfxBackend->newFrame(w, h, app.highContrast ? kClearHc : app.lightUi ? kClearLight : kClear);
            ImGui_ImplGlfw_NewFrame();
            {   // display scaling (Windows / Linux): follow the scale factor of the monitor the window is on. macOS scales by itself;
                // there DECT2_UISCALE (dev) still sets it, to see how a Windows laptop at 125 or 150 % lays out
                float s = 1;
#ifndef __APPLE__
                float xs = 1, ys = 1;
                glfwGetWindowContentScale(window, &xs, &ys);
                s = std::max(1.f, std::max(xs, ys));
#endif
                if (const char* e = getenv("DECT2_UISCALE")) s = std::max(0.5f, (float)atof(e));
                s *= app.uiTextScale;   // View > Text size
                if (s != gUi) {
                    gUi = s;
                    ImGuiStyle& st = ImGui::GetStyle();
                    st = baseStyle;
                    st.ScaleAllSizes(s);
                    st.FontScaleDpi = s;
                    plt::GetStyle().Scale = s;
                }
            }
            {   // a style ImGui refuses (#53: WindowMinSize below 1 on a Windows 11 PC) is put back instead of stopping the app
                ImGuiStyle& st = ImGui::GetStyle();
                if (!(st.WindowMinSize.x >= 1)) st.WindowMinSize.x = 32 * gUi;
                if (!(st.WindowMinSize.y >= 1)) st.WindowMinSize.y = 32 * gUi;
            }
            ImGui::NewFrame();
            {   // text size: Ctrl +, Ctrl - and Ctrl 0 (Cmd on a Mac), in either interface
                handleFullscreenHotkeys();   // F11 / Alt+Enter: the whole window, either interface (not while typing)
                ImGuiIO& kio = ImGui::GetIO();
                if ((kio.KeyCtrl || kio.KeySuper) && !kio.WantTextInput) {
                    float t = app.uiTextScale;
                    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) t = std::min(2.f, std::round(t * 10 + 1) / 10.f);
                    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) t = std::max(0.8f, std::round(t * 10 - 1) / 10.f);
                    if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) t = 1.f;
                    if (t != app.uiTextScale) { app.uiTextScale = t; savePrefs(app); }
                }
            }
            if (static bool up = false; !up && ImGui::GetFrameCount() > 120 && glfwGetTime() > 5) { up = true; dect2::crash::startupDone(); }   // started: see the startup guard
            if (autostart) {
                autostart = false;
                app.devIdx = 0;
                if (hackrfStart) for (int i = 0; i < (int)app.devices.size(); i++) if (app.devices[i].kind == (fileStart ? DeviceInfo::File : DeviceInfo::HackRF) || (!fileStart && app.devices[i].isRadio())) app.devIdx = i;
                bool found = startDevice.empty();
                if (!startDevice.empty()) {
                    for (int i = 0; i < (int)app.devices.size(); i++) if (app.devices[i].name == startDevice) { app.devIdx = i; found = true; break; }
                    if (!found) {
                        const std::string msg = "Requested source not found: " + startDevice + ". Receiver was not started.";
                        app.engine.log(msg); fprintf(stderr, "%s\n", msg.c_str());
                    }
                }
                if (found) {
                    app.tune.centerHz = app.freqMhz * 1e6; applyBandwidth(app);
                    app.engine.setComputeMode(app.computeMode); app.engine.setStandard(engineStd(app));
                    app.engine.start(app.devices[app.devIdx], app.tune, app.file);
                }
            }
            if (fmScanAuto && app.fmMode && app.engine.running() && glfwGetTime() > 3.0) {
                fmScanAuto = false;
                app.fmScan.running = true; app.fmScan.phase = 0; app.fmScan.idx = -1; app.fmScan.t0 = 0; app.fmScan.results.clear(); app.fmScan.cand.clear(); app.fmScan.savedFreq = app.freqMhz;
            }
            if (stress && app.engine.running() && ++stressFrame % 100 == 0) {
                stressStep++;
                app.freqMhz = 522.0 + 0.5 * (stressStep % 4);
                if (app.devices[app.devIdx].isGeneric()) app.tune.gainDb = 20 + 5 * (stressStep % 5);
                else { app.tune.lnaDb = 16 + 8 * (stressStep % 3); app.tune.vgaDb = 20 + 4 * (stressStep % 4); app.tune.ampOn = stressStep % 2; }
                app.tune.centerHz = app.freqMhz * 1e6;
                app.engine.retune(app.tune);
                app.peak.clear();   // what the toolbar does after a retune
                fprintf(stderr, "stress: retune %d -> %.1f MHz\n", stressStep, app.freqMhz);
            }
            double now = glfwGetTime();
            last = now;
            const double tUi0 = glfwGetTime();
            receiversFrame(app, io.DisplaySize);
            popOutFrame(app);
            ImGui::Render();
            if (app.highContrast) lightenDrawData(ImGui::GetDrawData(), false, true);   // High contrast wins over Light
            else if (app.lightUi) lightenDrawData(ImGui::GetDrawData());
            static const bool perf = getenv("DECT2_FPS") != nullptr;   // frame statistics on stderr: DECT2_FPS=1
            static double pAcc = 0, pMax = 0, pT0 = glfwGetTime(); static int pN = 0;
            if (perf) {
                const double d = glfwGetTime() - tUi0;
                pAcc += d; pMax = std::max(pMax, d); pN++;
                if (glfwGetTime() - pT0 > 2.0) { fprintf(stderr, "ui: %.1f fps, build %.2f ms avg / %.1f ms max\n", pN / (glfwGetTime() - pT0), 1e3 * pAcc / pN, 1e3 * pMax); pAcc = pMax = 0; pN = 0; pT0 = glfwGetTime(); }
            }
            const bool shotNow = shotPath && (shotAfter > 0 ? glfwGetTime() > shotAfter : ++shotFrame == (hackrfStart ? (app.playReq >= 0 || app.engine.player().selected() >= 0 ? 2400 : 900) : 400));
            static const bool statlog = getenv("DECT2_STATLOG") != nullptr;   // a status line every 10 s on stderr (the log file on Windows): DECT2_STATLOG=1
            static double sT0 = glfwGetTime();
            if (statlog && app.engine.running() && glfwGetTime() - sT0 >= 10.0) {
                sT0 = glfwGetTime();
                const PlayerStats ps = app.engine.player().stats();
                fprintf(stderr, "stat: dropped %llu, PLP frames dropped %llu, decode %.0f ms on %s (mode %d), blocks ok %llu bad %llu, player %s hw=%d decoded %llu shown %llu late %llu buf %.0f ms underruns %d A/V %+.0f ms\n",
                        (unsigned long long)app.engine.droppedSamples(), (unsigned long long)app.rx.plpFramesDropped, app.rx.plpDecodeMs, app.rx.plpOnGpu ? "GPU" : "CPU", app.rx.computeMode,
                        (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, ps.status.c_str(), ps.hardware, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, ps.audioBufferMs, ps.underruns, ps.avOffsetMs);
                fprintf(stderr, "stat: %s\n", t2rxProfile().c_str());
                fprintf(stderr, "stat: %s\n", engineWaitProfile().c_str());
                fprintf(stderr, "stat: %s\n", app.engine.loadProfile().c_str());
            }
            gfxBackend->endFrame(ImGui::GetDrawData(), shotNow ? shotPath : nullptr);
            popOutPresent();
            if (shotNow) {
                { PlayerStats ps = app.engine.player().stats(); { BbStats bb = app.engine.bbStats(); fprintf(stderr, "stream: BB frames %llu lost %llu, PLP frames dropped by busy decoder %llu, FEC blocks ok %llu bad %llu, samples dropped %llu, CPU decode %.0f ms\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)app.rx.plpFramesDropped, (unsigned long long)app.rx.blocksOk, (unsigned long long)app.rx.blocksBad, (unsigned long long)app.engine.droppedSamples(), app.rx.plpDecodeMs); }
                fprintf(stderr, "player: %s hw=%d %s %dx%d decoded %llu shown %llu late %llu errors %llu audio %s ch %d buf %.0f ms underruns %d A/V %+.0f ms\n", ps.status.c_str(), ps.hardware, ps.videoCodec.c_str(), ps.width, ps.height, (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, (unsigned long long)ps.errors, ps.audioCodec.c_str(), ps.audioChannels, ps.audioBufferMs, ps.underruns, ps.avOffsetMs); }
                glfwSetWindowShouldClose(window, 1);
            }
        }
    }
    delete gDiscord;   // clears the status and joins its thread
    txShutdown();      // the transmitter goes quiet before anything else is closed
    receiversShutdown(app);
    app.engine.stop();
    app.engine.flushResults();
    appResultsSave(app, true);
    updateOnExit(app);   // a downloaded update replaces this program once it has ended
    popOutShutdown();   // its window and surface go before the back end
    gfxBackend->shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    dect2::crash::startupDone();   // a quick clean exit (the --shot runs) is not a failed start
    return 0;
}

