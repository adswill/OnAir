// The new interface: the Faceplate, Cinema, Meters, Dock and Panel layouts of the body (the "Scope: Cinema" and
// "Faceplate" variants are instruments in a chassis; Dock and Panel are the two-column ones).
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include "ui2_internal.h"
#include "dect2/dsp/baseband_filter.h"
#include "typography.h"

using namespace ui2;

namespace ui2 {

void bodyFace(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##face", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 8 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 3 * gUi));
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float anaH = (a.atsc3Mode ? 0 : 190) * gUi;
    const float topH = H - (anaH > 0 ? anaH + gap : 0);
    const float leftW = std::floor(W * 0.66f), rightW = W - leftW - gap;
    ImGui::BeginGroup();
    {   // the display: run, frequency, what is being received
        const float dispH = 142 * gUi;
        beginModule("##f_disp", N_("DISPLAY"), ImVec2(leftW, dispH));
        tgRect(a, TgToolbar);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const bool running = a.engine.running();
        if (runButton(running, 54 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        ImGui::SetCursorScreenPos(ImVec2(o.x + 72 * gUi, o.y - 2 * gUi));
        const float end = freqDigits(a, 52.f);
        (void)end;
        summary(a, o.x + 74 * gUi, o.y + 66 * gUi, 1.5f, ImGui::GetContentRegionAvail().x - 74 * gUi);
        const float mw = 250 * gUi;
        levelMeter(a, o.x + ImGui::GetContentRegionAvail().x - mw, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + ImGui::GetContentRegionAvail().x - mw, o.y + 44 * gUi));
        volumeControl(a, mw);
        endModule();
    }
    {
        beginModule("##f_scope", N_("SPECTRUM"), ImVec2(leftW, topH - 142 * gUi - 3 * gUi));
        tgRect(a, TgMain);
        mainTabs(a);
        endModule();
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        const float cH = 188 * gUi;
        beginModule("##f_src", N_("SOURCE / GAIN"), ImVec2(rightW, cH + 38 * gUi));
        toolbarParts(a, TbSource, true);
        toolbarParts(a, TbGain, true);
        endModule();
        const float modeModH = (kNumModes * 22 + 3 * 20 + 50) * gUi;   // twelve modes in three groups, then the tuner
        beginModule("##f_mode", N_("MODE / TUNER"), ImVec2(rightW, modeModH));
        toolbarParts(a, TbTuner, true);
        a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
        modeList(a);
        if (a.family == 0 || a.family == 3 || captureRowShown(a) || ((modeUi(a.family) && modeUi(a.family)->decoder) || *dect2::bbf::keyForFamily(a.family))) toolbarParts(a, TbDecoder, true);
        endModule();
        const float rest = topH - (cH + 38 * gUi) - modeModH - 9 * gUi;          // what the first two modules leave
        const float listH = std::max(150.f * gUi, rest * 0.64f), logH = std::max(60.f * gUi, rest - listH);
        beginModule("##f_list", listTitle(a, true), ImVec2(rightW, listH));
        tgRect(a, TgRight);
        rightPanel(a);
        endModule();
        beginModule("##f_log", N_("LOG"), ImVec2(rightW, logH));
        logConsole(a, 0);
        endModule();
    }
    ImGui::EndGroup();
    if (anaH > 0) {
        beginModule("##f_ana", N_("ANALYSIS"), ImVec2(W, anaH));
        tgRect(a, TgConst);
        constellationsTab(a);
        endModule();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void bodyCinema(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    static bool open = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##cinema", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    beginPane("##cin_head", ImVec2(0, 0), true);
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    modeCombo(a, 120 * gUi);
    a.tgMax[TgSwitch] = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::SameLine(0, 10 * gUi);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - 96 * gUi;
    bool any = false;
    auto drawChip = [&](const char* label, double mhz, bool fm) {
        const float w = ImGui::CalcTextSize(label).x + 14 * gUi;
        if (ImGui::GetCursorScreenPos().x + w > right) return;
        bool clicked = false;
        chip(label, std::fabs(mhz - a.freqMhz) < 0.05, clicked);
        ImGui::SameLine(0, 4 * gUi);
        any = true;
        if (clicked && !a.fmScan.running) { if (fm) fmTune(a, mhz); else tuneFreq(a, mhz); }
    };
    if (a.fmMode) {
        for (const auto& r : a.fmScan.results) if (r.found) { char b[64]; snprintf(b, sizeof b, "%.1f %s", r.mhz, r.name.c_str()); drawChip(b, r.mhz, true); }
    } else if (!a.dabMode && a.family < 6) {
        for (const auto& c : a.channels) { char b[64]; snprintf(b, sizeof b, "%.0f %s", c.freqMhz, c.name.empty() ? "mux" : c.name.c_str()); drawChip(b, c.freqMhz, false); }
    }
    if (!any) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr(a.fmMode ? "no stations yet: run the scan" : a.dabMode || a.family >= 6 ? "" : "no saved channels yet: run the scan")); ImGui::SameLine(); }
    ImGui::SetCursorScreenPos(ImVec2(right + 8 * gUi, ImGui::GetItemRectMin().y));
    if (ImGui::SmallButton(tr(open ? "Controls  \xe2\x96\xb4" : "Controls  \xe2\x96\xbe"))) open = !open;
    endPane();
    if (open) ribbonPane(a);
    const float botH = (a.atsc3Mode ? 0 : 118) * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - (botH > 0 ? botH + gap : 0);
    beginPane("##cin_main", ImVec2(0, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    if (!a.atsc3Mode) {
        beginPane("##cin_ana", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 5: scope / meters (the dock is a bank of big instrument meters)
struct Meter { std::string label, value; float frac; int level; };   // level: 0 neutral, 1 good, 2 warn, 3 bad

void drawMeters(const std::vector<Meter>& ms, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Meter& m : ms) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bh = 9 * gUi;
        dl->AddText(p, u32(T.dim), m.label.c_str());
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.45f);
        const ImVec2 ts = ImGui::CalcTextSize(m.value.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + w - ts.x, p.y - 4 * gUi), u32(T.bright), m.value.c_str());
        ImGui::PopFont();
        const float y = p.y + ImGui::GetTextLineHeight() + 6 * gUi;
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + bh), u32(ImVec4(0.07f, 0.075f, 0.08f, 1)));
        const ImVec4 col = m.level == 3 ? pal::badRed() : m.level == 2 ? pal::warnAmber() : m.level == 1 ? ImVec4(0.62f, 0.78f, 0.66f, 1) : ImVec4(0.72f, 0.74f, 0.77f, 1);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * std::min(1.f, std::max(0.f, m.frac)), y + bh), u32(col));
        for (int k = 1; k < 10; k++) dl->AddLine(ImVec2(p.x + w * k / 10.f, y), ImVec2(p.x + w * k / 10.f, y + bh * (k == 5 ? 1.f : 0.45f)), u32(ImVec4(0.04f, 0.04f, 0.045f, 1)));
        dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + w, y + bh), u32(T.edge));
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + bh + 12 * gUi));
    }
}

void meterBank(App& a, float w) {
    std::vector<Meter> ms;
    auto add = [&](const char* label, const char* fmt, double v, double lo, double hi, int level) {
        char b[32]; snprintf(b, sizeof b, fmt, v);
        ms.push_back({label, b, (float)((v - lo) / (hi - lo)), level});
    };
    const RxTelemetry& rx = a.rx;
    const bool run = a.engine.running();
    if (const ModeUi* mu = modeUi(a.family); run && mu && mu->meters) {
        std::vector<ModeMeter> mm;
        mu->meters(a, mm);
        for (const ModeMeter& m : mm) add(m.label, m.fmt, m.v, m.lo, m.hi, m.level);
    } else if (run && a.fmMode && rx.standard == 6) {
        const FmTelemetry& f = rx.fm;
        add("AUDIO SNR  dB", "%.1f", f.snrDb, 0, 50, f.snrDb >= 30 ? 1 : f.snrDb >= 18 ? 2 : 3);
        add("DEVIATION  kHz", "%.0f", f.devKhz, 0, 100, f.devKhz > 90 ? 3 : f.devKhz > 75 ? 2 : 1);
        add("PILOT  %", "%.1f", f.pilotPct, 0, 12, f.stereo ? 1 : 0);
        add("RDS BLOCKS OK  %", "%.0f", f.rdsBlockOkPct, 0, 100, f.rdsBlockOkPct >= 70 ? 1 : f.rdsSync ? 2 : 0);
    } else if (run && a.dabMode && rx.standard == 3) {
        add("SNR  dB", "%.1f", rx.dab.snrDb, 0, 30, rx.dab.snrDb >= 12 ? 1 : 2);
        add("FIC BLOCKS OK  /12", "%.0f", rx.dab.ficRecentOk, 0, 12, rx.dab.ficRecentOk >= 11 ? 1 : rx.dab.ficRecentOk > 0 ? 2 : 3);
    } else if (run) {
        add("SNR  dB", "%.1f", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb, 0, 40, 0);
        if (rx.plpMerDb > 0) add("MER  dB", "%.1f", rx.plpMerDb, 0, 40, 0);
        const double tot = (double)(rx.blocksOk + rx.blocksBad);
        if (tot > 0) add("FEC BLOCKS OK  %", "%.0f", 100.0 * (double)rx.blocksOk / tot, 0, 100, 0);
    }
    const double db = run ? a.spec.stats.rmsDbfs : -60.0;
    add("ADC LEVEL  dBFS", "%.1f", db, -60, 0, !run ? 0 : db > -3 ? 3 : db > -12 ? 2 : db < -45 ? 2 : 1);
    drawMeters(ms, w);
}

void bodyMeters(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##meters", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    const float botH = (a.atsc3Mode ? 0 : 150) * gUi, dockW = 320 * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - (botH > 0 ? botH + gap : 0);
    beginPane("##main2", ImVec2(ImGui::GetContentRegionAvail().x - dockW - gap, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    ImGui::TextDisabled("%s", tr("INSTRUMENTS"));
    ImGui::Separator();
    meterBank(a, ImGui::GetContentRegionAvail().x - 4 * gUi);
    ImGui::Separator();
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        beginPane("##const2", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 6: scope / dock (an IDE-style tabbed panel along the bottom)
void bodyDock(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##dockbody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    const float dockH = 230 * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - dockH - gap;
    beginPane("##main2", ImVec2(0, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    beginPane("##dock", ImVec2(0, 0));
    tgRect(a, TgConst);
    if (ImGui::BeginTabBar("##docktabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (!a.atsc3Mode && typo::beginTabItem(trw("Analysis"))) { constellationsTab(a); ImGui::EndTabItem(); }
        if (typo::beginTabItem(listTitle(a, false))) { tgRect(a, TgRight); rightPanel(a); ImGui::EndTabItem(); }
        if (typo::beginTabItem(trw("Receiver"))) { signalRow(a); ImGui::Spacing(); statusBar(a); ImGui::Spacing(); receiverGlance(a, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y); ImGui::EndTabItem(); }
        if (typo::beginTabItem(trw("Log"))) { logConsole(a, 0); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    endPane();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 7: panel (the mode keys on the right; SDR settings, spectrum and waterfall, LEDs and constellations down the left)
const ImVec4 kPanelChassis(0.735f, 0.728f, 0.706f, 1);

// the mode keys: grouped, each with a lamp that lights when the mode is selected
void modeKeys(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const ImVec2 first = ImGui::GetCursorScreenPos();
    for (int g = 0; g < kNumGroups; g++) {
        if (!modeGroupListed(a, g)) continue;
        ImGui::TextDisabled("%s", tr(kGroupNames[g]));
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.group != g || !modeListed(a, m)) continue;
            const float h = 26 * gUi, w = ImGui::GetContentRegionAvail().x - 2 * gUi;   // twelve keys have to fit: the name on the left, the short description on the right
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##key", ImVec2(w, h));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const bool sel = modeSelected(a, m);
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            const ImU32 plate = sel ? IM_COL32(18, 19, 20, 255) : hov ? IM_COL32(58, 59, 60, 255) : IM_COL32(48, 49, 50, 255);
            dl->AddRectFilled(p0, p1, plate, 2.f);
            if (sel) {   // pressed: a dark inset
                dl->AddRect(p0, p1, IM_COL32(8, 8, 8, 255), 2.f);
                dl->AddLine(ImVec2(p0.x + 1, p1.y), ImVec2(p1.x, p1.y), IM_COL32(95, 95, 92, 255));
            } else {     // raised: a light top edge, a dark bottom edge
                dl->AddLine(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 1), IM_COL32(110, 111, 112, 255));
                dl->AddLine(ImVec2(p0.x + 1, p1.y - 1), ImVec2(p1.x - 1, p1.y - 1), IM_COL32(14, 14, 15, 255));
                dl->AddRect(p0, p1, IM_COL32(20, 20, 21, 255), 2.f);
            }
            const ImVec2 c(p0.x + 15 * gUi, (p0.y + p1.y) * 0.5f);
            dl->AddCircleFilled(c, 6.f * gUi, IM_COL32(8, 8, 8, 255));
            dl->AddCircle(c, 6.f * gUi, IM_COL32(70, 70, 68, 255), 0, 1.f);
            if (sel) {
                dl->AddCircleFilled(c, 4.4f * gUi, u32(m.accent));
                dl->AddCircleFilled(ImVec2(c.x - 1.4f * gUi, c.y - 1.6f * gUi), 1.5f * gUi, IM_COL32(255, 255, 255, 120));
            } else dl->AddCircleFilled(c, 4.4f * gUi, IM_COL32(30, 31, 31, 255));
            const float fs = ImGui::GetFontSize();
            const char* sub = tr(m.sub);
            const ImVec2 ns = ImGui::CalcTextSize(m.name), ss = ImGui::CalcTextSize(sub);
            dl->AddText(ImVec2(p0.x + 30 * gUi, p0.y + (h - ns.y) * 0.5f), u32(sel ? T.bright : (running ? T.dim : T.text)), m.name);
            const float sw = ss.x * 0.84f;
            if (p0.x + 30 * gUi + ns.x + 14 * gUi < p1.x - sw - 8 * gUi) dl->AddText(ImGui::GetFont(), fs * 0.84f, ImVec2(p1.x - sw - 8 * gUi, p0.y + (h - ss.y * 0.84f) * 0.5f), u32(T.dim), sub);
            if (clicked) selectMode(a, m.family, m.subId);
            if (hov) ImGui::SetTooltip("%s", running && !sel ? tr("Click to switch mode and restart receiver automatically") : m.tip);
        }
        ImGui::Dummy(ImVec2(1, 2 * gUi));
    }
    a.tgMin[TgSwitch] = first; a.tgMax[TgSwitch] = ImVec2(first.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
}

void bodyPanel(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##panel", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 8 * gUi, vgap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, vgap));
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rightW = 236 * gUi, leftW = W - rightW - gap;
    const bool synth = sourceOptionsOpen(a);
    const float sdrH = (synth ? 196 : 148) * gUi + (modeUi(a.family) && modeUi(a.family)->tuner ? 70 * gUi : 0), ledH = 80 * gUi, anaH = (a.atsc3Mode ? 0 : 224) * gUi;
    const float wfH = H - sdrH - ledH - anaH - (anaH > 0 ? 3 : 2) * vgap;
    ImGui::BeginGroup();
    {   // SDR settings: run, frequency, what is received, level and volume; then the source, tuner and gain controls
        beginModule("##p_sdr", N_("SDR"), ImVec2(leftW, sdrH));
        tgRect(a, TgToolbar);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const bool running = a.engine.running();
        if (runButton(running, 44 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        ImGui::SetCursorScreenPos(ImVec2(o.x + 62 * gUi, o.y - 1 * gUi));
        const float digitsEnd = freqDigits(a, 38.f);
        const float vw = 210 * gUi, mw = 250 * gUi;
        summary(a, digitsEnd + 30 * gUi, o.y + 5 * gUi, 1.f, o.x + ImGui::GetContentRegionAvail().x - vw - mw - 36 * gUi - (digitsEnd + 30 * gUi));
        levelMeter(a, o.x + ImGui::GetContentRegionAvail().x - vw - mw - 20 * gUi, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + ImGui::GetContentRegionAvail().x - vw + 4 * gUi, o.y + 8 * gUi));
        volumeControl(a, vw);
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + 52 * gUi));
        toolbarParts(a, TbSource | TbTuner | TbGain | TbDecoder, false);
        if (a.devices[a.devIdx].isRadio()) { ImGui::SameLine(0, 14 * gUi); radioSettingsUi(a, false); }
        if (synth) sourceOptions(a);
        endModule();
    }
    {   // the picture
        beginModule("##p_wf", N_("SPECTRUM / WATERFALL"), ImVec2(leftW, wfH));
        tgRect(a, TgMain);
        mainTabs(a);
        endModule();
    }
    {   // the LEDs
        beginModule("##p_led", N_("STATUS"), ImVec2(leftW, ledH));
        statusBar(a);
        endModule();
    }
    if (anaH > 0) {
        beginModule("##p_ana", N_("CONSTELLATIONS"), ImVec2(leftW, anaH));
        tgRect(a, TgConst);
        constellationsTab(a);
        endModule();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        const float modeH = (kNumModes * 30 + 3 * 26 + 14) * gUi;
        beginModule("##p_mode", N_("MODE"), ImVec2(rightW, modeH));
        modeKeys(a);
        endModule();
        beginModule("##p_list", listTitle(a, true), ImVec2(rightW, H - modeH - vgap));
        tgRect(a, TgRight);
        rightPanel(a);
        endModule();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

}  // namespace ui2
