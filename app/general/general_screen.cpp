// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The General receiver screen of 4.0 (docs/GENERAL-4.0.md): the spectrum and the waterfall take almost the whole window, every control is
// in one sidebar on the left with folding sections stacked top to bottom (no tabs), a thin bar over the spectrum has the step, Snap, Centre
// and the channel level with its SNR, and a drawer under the waterfall holds the rest (Receiver details, History, Memory, Frequencies, a
// second mode). The demodulator button by the frequency is in the shared top bar (ui2_shell.cpp). OnAir's own layout; the idea is the one
// SDR++ made popular (one menu on the left, the plots everywhere else).
// The layout state (and its maths) is general_layout.h; the pieces of the General receiver it draws are in general_ui.cpp (general_panels.h).
#include "ui/ui2_internal.h"
#include "general/general_layout.h"
#include "general/general_panels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui2;

namespace {

genlayout::Layout& lay() {
    static genlayout::Layout L;
    static bool loaded = false;
    if (!loaded) { loaded = true; genlayout::decode(plat::prefs().getS("gen40Layout", ""), L); }   // nothing saved (an older profile): the defaults
    return L;
}
void saveLay() { plat::prefs().setS("gen40Layout", genlayout::encode(lay())); }
bool gForced = false;   // the sidebar shown by its button although the window is narrow (not saved)

// a folding section whose open state is one bit of the layout (kept between runs)
bool sec(const char* title, int s) {
    genlayout::Layout& L = lay();
    const bool was = (L.open & genlayout::bit(s)) != 0;
    ImGui::SetNextItemOpen(was, ImGuiCond_Always);
    const bool open = section(title, was);
    if (open != was) { L.open = open ? (L.open | genlayout::bit(s)) : (L.open & ~genlayout::bit(s)); saveLay(); }
    return open;
}
struct Ind { Ind() { ImGui::Indent(6 * gUi); } ~Ind() { ImGui::Unindent(6 * gUi); ImGui::Spacing(); } };

// the most used spectrum and waterfall settings; the rest stay in the corner buttons of the plots (Spectrum, Waterfall)
void displayExtras(App& a) {
    static const int kFft[] = {0, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
    char cur[32];
    if (a.specFft == 0) snprintf(cur, sizeof cur, "%s (%zu)", tr("Auto"), a.spec.dbfs.size()); else snprintf(cur, sizeof cur, "%d", a.specFft);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##gfft", cur)) {
        for (int v : kFft) {
            char lb[24]; if (v == 0) snprintf(lb, sizeof lb, "%s", tr("Auto")); else snprintf(lb, sizeof lb, "%d", v);
            if (ImGui::Selectable(lb, a.specFft == v)) { a.specFft = v; a.engine.setSpectrumFft(v); savePrefs(a); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("FFT size: bins in the spectrum. More is finer but costs more CPU; Auto follows the sample rate."));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##gavg", &a.specAvgSec, 0.f, 2.f, a.specAvgSec <= 0.001f ? tr("averaging off") : tr("averaging %.2f s"), ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Time constant of the averaged spectrum line: longer is calmer"));
    static const float kRates[] = {0, 30, 15, 10, 5, 2, 1, 0.5f, 0.2f, 100};
    char rc[32];
    if (a.wf.rowsPerSec <= 0) snprintf(rc, sizeof rc, "%s", tr("waterfall: max speed")); else snprintf(rc, sizeof rc, tr("waterfall: %g rows/s"), a.wf.rowsPerSec);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##gwfrate", rc)) {
        for (float r : kRates) {
            char lb[32]; if (r <= 0) snprintf(lb, sizeof lb, "%s", tr("max (a row a frame)")); else snprintf(lb, sizeof lb, r >= 50 ? tr("%g rows/s (CW, fast)") : tr("%g rows/s"), r);
            if (ImGui::Selectable(lb, std::fabs(a.wf.rowsPerSec - r) < 1e-4f)) { a.wf.setRate(r); savePrefs(a); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Waterfall speed. Slower rows hold a longer time in the picture; the Waterfall corner button has every rate, the detail and the export."));
    bool on = plat::prefs().getB("gen40Screen", true);
    if (ImGui::Checkbox(tr("4.0 General layout"), &on)) plat::prefs().setB("gen40Screen", on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Off: the General receiver goes back to the layout of 3.9 (the Listen tab). The same switch is in its Listen tab."));
}

void sidebarBody(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * gUi, 4 * gUi));
    const float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowSize().x;
    using namespace genlayout;
    if (sec(N_("Status"), SecStatus)) { Ind i; statusBar(a); }
    const float srcY0 = ImGui::GetCursorScreenPos().y;
    if (sec(N_("Source"), SecSource)) {
        Ind i;
        toolbarParts(a, TbSource, true);
        if (sourceOptionsOpen(a)) sourceOptions(a);
        tourMark("source", ImVec2(x0, srcY0), ImVec2(x1, ImGui::GetCursorScreenPos().y));
        const float gY0 = ImGui::GetCursorScreenPos().y;
        ImGui::TextDisabled("%s", tr("Gain"));
        toolbarParts(a, TbGain, true);
        tourMark("gain", ImVec2(x0, gY0), ImVec2(x1, ImGui::GetCursorScreenPos().y));
        ImGui::TextDisabled("%s", tr("Tuner"));
        toolbarParts(a, TbTuner, true);
        tourMark("tuning", ImVec2(x0, gY0), ImVec2(x1, ImGui::GetCursorScreenPos().y));
        if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) radioSettingsUi(a, false);
    }
    if (sec(N_("Radio"), SecRadio)) { Ind i; generalSideRadio(a); }
    if (sec(N_("Audio"), SecAudio)) {
        Ind i;
        const float y0 = ImGui::GetCursorScreenPos().y;
        volumeControl(a, ImGui::GetContentRegionAvail().x);
        tourMark("audio", ImVec2(x0, y0), ImVec2(x1, ImGui::GetCursorScreenPos().y));
        ImGui::TextDisabled("%s", tr("Output device and levels: the speaker settings button above"));
        generalSideAudio(a);
    }
    if (sec(N_("Display"), SecDisplay)) { Ind i; sideDisplayBody(a); displayExtras(a); }
    if (sec(N_("Recording"), SecRecording)) { Ind i; generalSideRecording(a); }
    if (sec(N_("Bookmarks"), SecBookmarks)) { Ind i; generalSideBookmarks(a); }
    if (sec(N_("Signals"), SecSignals)) { Ind i; generalSideSignals(a); }
    if (sec(N_("Channels"), SecChannels)) { Ind i; generalSideChannels(a); }
    if (sec(N_("Mode"), SecMode)) {
        Ind i;
        a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
        modeList(a);
        a.tgMax[TgSwitch] = ImVec2(x1, ImGui::GetCursorScreenPos().y);
        if (sideDecoderShown(a)) { ImGui::TextDisabled("%s", tr("Decoder")); toolbarParts(a, TbDecoder, true); }
    }
    if (sec(N_("Tools"), SecTools)) { Ind i; sideToolsBody(a); }
    if (sec(N_("Log"), SecLog)) sideLogBody(a);
    ImGui::PopStyleVar();
}

// the drawer under the waterfall: what used to be the other tabs of the General receiver
void drawerTabs(App& a) {
    modeActionBar(a);
    gTabOwner = &a;
    gMainTabBar = nullptr;
    if (pal::dev() ? kit::beginTabs("##gdrawer") : ImGui::BeginTabBar("##gdrawer")) {
        gMainTabBar = ImGui::GetCurrentTabBar();
        const ModeUi* mu = modeUi(a.family);
        if (mu && mu->receiver && tabItem(N_("Receiver"), Ic::Antenna)) { mu->receiver(a); ImGui::EndTabItem(); }
        if (tabItem(N_("History"), Ic::Chart)) { historyLogTab(a); ImGui::EndTabItem(); }
        memoryTabItem(a);
        freqdbTabItem(a);
        secondModeTabs(a);
        ImGui::EndTabBar();
    }
    gTabOwner = nullptr;
    gMainTabBar = nullptr;
}

}  // namespace

bool generalScreenActive(const App& a) {
    return a.family == 55 && a.newUi && !a.touchUi && !txViewFlag() && !hamViewFlag() && !dockActive(a) && (a.uiVariant == 0 || a.uiVariant == 1) &&
           plat::prefs().getB("gen40Screen", true);
}

void generalScreen(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    genlayout::Layout& L = lay();
    ImGuiIO& io = ImGui::GetIO();
    generalShortcuts(a);
    const float availW = ImGui::GetContentRegionAvail().x;
    bool autoHid = false;
    float sideW = std::floor(genlayout::sideWidth(availW / gUi, L, gForced, &autoHid) * gUi);
    auto toggle = [&] { genlayout::toggleSide(L, gForced, autoHid); saveLay(); };
    if (!io.WantTextInput && (io.KeyCtrl || io.KeySuper) && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        toggle();
        sideW = std::floor(genlayout::sideWidth(availW / gUi, L, gForced, &autoHid) * gUi);
    }
    if (sideW > 0) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
        ImGui::BeginChild("##gside", ImVec2(sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        sidebarBody(a);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        const ImVec2 p = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - bodyH), ImVec2(p.x, p.y), u32(kEdge));
        ImGui::SameLine(0, 0);
        float w = L.sideW;
        if (splitter("##gsideSplit", true, 5 * gUi, bodyH, w, sideW, genlayout::kSideMin * gUi, std::max(genlayout::kSideMin, std::min(genlayout::kSideMax, availW / gUi - genlayout::kPlotMinW)) * gUi, 1, a) || w != L.sideW) {
            L.sideW = w > 0 ? w : genlayout::kSideDefault;   // a double click: the default width
            saveLay();
        }
        ImGui::SameLine(0, 0);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##gcentre", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    {   // the bar: the sidebar button, then the General receiver's step, Snap, Centre and level
        if (kit::iconButton("##gsidebtn", Ic::Sliders, tr(sideW > 0 ? "Hide the sidebar (Ctrl+B)" : "Show the sidebar (Ctrl+B)"), sideW > 0)) toggle();
        ImGui::SameLine(0, 8 * gUi);
        generalBar(a);
    }
    const float hdrH = std::floor(ImGui::GetFrameHeight() + 2 * gUi);
    const float restH = ImGui::GetContentRegionAvail().y;
    float drawerH = L.drawerOpen ? std::floor(std::min(L.drawerH * gUi, std::max(genlayout::kDrawerMin * gUi, restH - hdrH - 2 * gap - 200 * gUi))) : 0;
    const float specH = std::max(120 * gUi, restH - hdrH - gap - (drawerH > 0 ? drawerH + gap : 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##gspec", ImVec2(0, specH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    tgRect(a, TgMain);
    {
        const float before = L.specFrac;
        gSpectrumInShell = true;
        spectrumWaterfall(a, L.specFrac, true);   // the bar between the spectrum and the waterfall is dragged (kept in the layout)
        gSpectrumInShell = false;
        static bool pending = false;
        if (L.specFrac != before) pending = true;
        if (pending && !ImGui::IsMouseDown(0)) { pending = false; saveLay(); }
    }
    ImGui::EndChild();
    a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    {   // the drawer's header: a fold arrow and its tabs' names; the bar above it resizes it
        if (drawerH > 0) {
            float h = L.drawerH;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
            if (splitter("##gdrawerSplit", false, gap, ImGui::GetContentRegionAvail().x, h, drawerH, genlayout::kDrawerMin * gUi, restH - hdrH - 200 * gUi, -1, a)) {
                L.drawerH = h > 0 ? h : genlayout::kDrawerDefault;
                saveLay();
            }
            ImGui::PopStyleVar();
        }
        bool open = L.drawerOpen;
        char t[96];
        snprintf(t, sizeof t, "%s", tr("MORE  receiver details, history, memories, second mode"));
        if (kit::panelHeader(t, &open)) { L.drawerOpen = open; saveLay(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(L.drawerOpen ? "Fold the drawer: the spectrum and the waterfall take the room" : "Open the drawer: the receiver's details, the history, memories, frequencies and a second mode"));
    }
    if (L.drawerOpen) {
        ImGui::BeginChild("##gdrawer", ImVec2(0, 0), 0);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemSpacing);
        drawerTabs(a);
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
