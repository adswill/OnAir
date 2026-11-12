// The classic interface's main tabs (Overview, Receiver, Stream and the mode's own tab) and the small helpers they share.
// Split from main.cpp; mainTabs() is called by drawUI() in draw_ui.cpp.
// The tab bodies are small; the larger panels they show live in analysis_tabs.cpp, tv.cpp and the mode folders.
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "ease/ease_ui.h"
#include "imgui_internal.h"
#include "ui/kit/kit.h"
#include "ui/kit/mode_panel.h"
#include "adsb/adsb_map.h"
#include "ui/contrast.h"
#include "dect2/source/iq_file.h"
#include "panels/gallery.h"
#include "dect2/engine/crash_report.h"
#include "platform/discord_rpc.h"
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include "main_internal.h"

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
    if (ImGui::DragFloatRange2(tr("spectrum dB##specdb"), &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f")) a.specAutoDb = false;
    ImGui::SameLine(0, 16 * gUi); ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::DragFloatRange2(tr("waterfall dB##wfdb"), &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f")) a.wf.autoTrack = a.specAutoDb = false;
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
        row("decoder load", "%.0f%% of real time%s", 100 * at.decodeLoad, at.fieldsDropped ? "" : "  (no field dropped)");
        if (at.fieldsDropped || at.packetsDropped)
            row("dropped", "%llu fields (decoder too slow), %llu packets (player or recorder too slow)", (unsigned long long)at.fieldsDropped, (unsigned long long)at.packetsDropped);
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
        {"Player", "TV", 0, -1, -1}, {"Guide", "TV", 1, -1, -1}, {"Recordings", "TV", 2, -1, -1}, {"Teletext", "TV", 3, -1, -1},
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

static void mainTabsInner(App& a, bool unified);
#include "ui/input_map.h"
#include "main_tabs_unified.inc"

void mainTabs(App& a) {
    // several receivers share this one tab bar, so ImGui kept whichever tab was open and fell back to the first one when the new receiver
    // had none of that name; each receiver now remembers its own tab and gets it back when its chip is selected again (#342)
    static int lastRx = 0;
    std::string mine;
    if (a.rxId != lastRx) {
        lastRx = a.rxId;
        if (!a.mainTabName.empty() && a.mainTabName != "Overview" && gForceTab.empty()) gForceTab = mine = a.mainTabName;
    }
    gTabOwner = &a;
    gMainTabBar = nullptr;
    if (a.layoutUnified && pal::dev()) unifiedCentre(a, [&a] { mainTabsInner(a, true); });
    else mainTabsInner(a, false);
    if (!mine.empty() && gForceTab == mine) gForceTab.clear();   // this receiver's mode has no such tab: do not jump to it later
    gTabOwner = nullptr;
    gMainTabBar = nullptr;
}

static void mainTabsInner(App& a, bool unified) {
    if (unified && gForceTab == "Overview") gForceTab.clear();
    // a narrow pane: the tabs get less padding (the bar takes it from the style when it begins, each tab when it is added), so that they keep
    // their whole names instead of being cut short or scrolled out of view
    // a mode with a Simple view: the switch above, then either the one Command view (Simple) or the tabs below (Advanced)
    if (const ModeUi* mu = modeUi(a.family); mu && modepanel::available(a)) {
        if (modepanel::simple(a)) {
            if (!unified) {   // the Overview tab (the spectrum) is not a tab here: the spectrum stays above the Command view
                const float h = std::floor(std::min(330 * gUi, std::max(210 * gUi, ImGui::GetContentRegionAvail().y * 0.32f)));
                ImGui::BeginChild("##simpleSpec", ImVec2(0, h), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                overviewTab(a);
                ImGui::EndChild();
            }
            adsbmap::FullScope fs(&a, [&a, mu]{ modepanel::commandView(a, *mu); });
            modepanel::commandView(a, *mu);   // in the window of the tabs (it scrolls like a tab body), not in a child of its own
            return;
        }
    }
    modeActionBar(a);   // 3.7: the same row (next step, Auto setup, Reset this mode) above the tabs of every mode; 4.0: one row, Simple / Advanced at its right end
    gTightTabs = ImGui::GetContentRegionAvail().x < 760 * gUi;
    if (gTightTabs) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * gUi, ImGui::GetStyle().FramePadding.y));
    const bool bar = pal::dev() ? kit::beginTabs("tabs") : ImGui::BeginTabBar("tabs", 0);
    if (gTightTabs) ImGui::PopStyleVar();
    if (bar) {
        gMainTabBar = ImGui::GetCurrentTabBar();
        if (!unified && tabItem(N_("Overview"), Ic::Grid)) { adsbmap::FullScope fs(&a, [&a]{ overviewTab(a); }); overviewTab(a); ImGui::EndTabItem(); }
        if (const ModeUi* mu = modeUi(a.family)) {   // a mode added after FM: its own tabs
            if (mu->tab && tabItem(mu->tabName ? mu->tabName : "Mode", mu->tabIcon)) { adsbmap::FullScope fs(&a, [&a, mu]{ mu->tab(a); }); easeui::codecNotice(a); mu->tab(a); ImGui::EndTabItem(); }
            else if (!mu->tab && mu->stream && tabItem(N_("TV"), Ic::Tv)) { adsbmap::FullScope fs(&a, [&a]{ tvTab(a); }); tvTab(a); ImGui::EndTabItem(); }
            if (mu->receiver && tabItem(N_("Receiver"), Ic::Antenna)) { adsbmap::FullScope fs(&a, [&a]{ receiverTab(a); }); receiverTab(a); ImGui::EndTabItem(); }
            if (mu->stream && tabItem(N_("Stream"), Ic::Layers)) { adsbmap::FullScope fs(&a, [&a]{ streamTab(a); }); streamTab(a); ImGui::EndTabItem(); }
            if (mu->scan && tabItem(N_("Scan"), Ic::Scan)) { adsbmap::FullScope fs(&a, [&a]{ scanTab(a); }); scanTab(a); ImGui::EndTabItem(); }
            if (unified && (!mu->panels ? false : true) && tabItem(N_("Analysis"), Ic::Pulse)) { adsbmap::FullScope fs(&a, [&a]{ constellationsTab(a); }); constellationsTab(a); ImGui::EndTabItem(); }
            if (tabItem(N_("History"), Ic::Chart)) { adsbmap::FullScope fs(&a, [&a]{ historyLogTab(a); }); historyLogTab(a); ImGui::EndTabItem(); }
            memoryTabItem(a); freqdbTabItem(a);
            secondModeTabs(a);
            ImGui::EndTabBar();
            return;
        }
        if (a.dabMode) { if (tabItem(N_("Radio"), Ic::Radio)) { adsbmap::FullScope fs(&a, [&a]{ dabRadioTab(a); }); dabRadioTab(a); ImGui::EndTabItem(); } }
        else if (a.fmMode) { if (tabItem(N_("Radio"), Ic::Radio)) { adsbmap::FullScope fs(&a, [&a]{ fmRadioTab(a); }); fmRadioTab(a); ImGui::EndTabItem(); } }
        else if (tabItem(N_("TV"), Ic::Tv)) { adsbmap::FullScope fs(&a, [&a]{ tvTab(a); }); tvTab(a); ImGui::EndTabItem(); }
        if (!a.fmMode && tabItem(a.dabMode ? N_("Ensemble") : N_("Receiver"), Ic::Antenna)) { adsbmap::FullScope fs(&a, [&a]{ receiverTab(a); }); receiverTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("Map"), Ic::Compass)) { adsbmap::FullScope fs(&a, [&a]{ dabTransmittersTab(a); }); dabTransmittersTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("Analyzer"), Ic::Grid)) { adsbmap::FullScope fs(&a, [&a]{ dabAnalyzerTab(a); }); dabAnalyzerTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && dabHasSpi(a.engine.dabEnsemble()) && tabItem(N_("Guide"), Ic::Clock)) { adsbmap::FullScope fs(&a, [&a]{ dabGuideTab(a); }); dabGuideTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && dabHasJournaline(a.engine.dabEnsemble()) && tabItem(N_("Journaline"), Ic::Doc)) { adsbmap::FullScope fs(&a, [&a]{ dabJournalineTab(a); }); dabJournalineTab(a); ImGui::EndTabItem(); }
        if (a.dabMode && tabItem(N_("FIC"), Ic::Doc)) { adsbmap::FullScope fs(&a, [&a]{ dabFicTab(a); }); dabFicTab(a); ImGui::EndTabItem(); }
        if (!a.dabMode && !a.fmMode && tabItem(N_("Stream"), Ic::Layers)) { adsbmap::FullScope fs(&a, [&a]{ streamTab(a); }); streamTab(a); ImGui::EndTabItem(); }
        if (tabItem(N_("Scan"), Ic::Scan)) { adsbmap::FullScope fs(&a, [&a]{ scanTab(a); }); scanTab(a); ImGui::EndTabItem(); }
        if (!a.fmMode && tabItem(N_("Antenna"), Ic::Compass)) { adsbmap::FullScope fs(&a, [&a]{ antennaTab(a); }); antennaTab(a); ImGui::EndTabItem(); }
        if (unified && !a.atsc3Mode && tabItem(N_("Analysis"), Ic::Pulse)) { adsbmap::FullScope fs(&a, [&a]{ constellationsTab(a); }); constellationsTab(a); ImGui::EndTabItem(); }
        if (tabItem(N_("History"), Ic::Chart)) { adsbmap::FullScope fs(&a, [&a]{ historyLogTab(a); }); historyLogTab(a); ImGui::EndTabItem(); }
        memoryTabItem(a); freqdbTabItem(a);
        secondModeTabs(a);
        ImGui::EndTabBar();
    }
}

bool anyRunning() { for (App* r : receiversAll()) if (r->engine.running()) return true; return false; }
