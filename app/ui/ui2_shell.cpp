// The new interface: the shell around the body (menu bar, top bar with the mode combo and summary, bottom bar, the
// mode list and side bar, the status line and the log console).
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include "transcribe/transcribe_app.h"
#include "input_map.h"
#include "dect2/dsp/baseband_filter.h"
#include "ui2_internal.h"
#include "../state/listen_freq.h"
#include "../codec/speech_codec_ui.h"
#include "../voice/voice_monitor_ui.h"
#include "ui_common.h"
#include "tour_logic.h"
#include "status_strip.h"
#include "../platform/proc_cpu.h"
#include <thread>
#include "../state/workspace.h"
#include "../state/layout.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include "dect2/engine/overload_hint.h"
#include "../platform/discord_rpc.h"
#include "../panels/report_problem.h"
#include "../panels/backup_ui.h"
#include "../gfx/render_caps.h"

// the byte length of the UTF-8 character that starts with `lead` (1 for ASCII and for a stray byte)
static size_t utf8Len(char lead) { const unsigned char c = (unsigned char)lead; return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }


void clearAllButtonAt(App& a, bool sameLine);   // update_ui.cpp: Clear all, without its own SameLine
using namespace ui2;

namespace ui2 {
ImVec2 gViewMenuCentre;   // where the View menu is in the menu bar (the dev --viewmenu shot clicks it)

// two lines about what is being received, in the top bar: the station or ensemble, the state and the main numbers
void summaryText(App& a, std::string& l1, std::string& l2) {
    const bool run = a.engine.running();
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    l1 = cur->name; l2 = tr(run ? "starting" : a.scanner.progress().running ? "channel scan has the radio" : "stopped");
    char b[96];
    if (run) {
        const RxTelemetry& rx = a.rx;
        const ModeUi* mu = modeUi(a.family);
        if (mu && mu->summary) {
            mu->summary(a, l1, l2);
        } else if (a.fmMode && rx.standard == 6) {
            const FmTelemetry& f = rx.fm;
            std::string ps = f.psName;
            while (!ps.empty() && (ps.back() == ' ' || ps.back() == '-')) ps.pop_back();
            while (!ps.empty() && (ps.front() == ' ' || ps.front() == '-')) ps.erase(ps.begin());
            l1 = f.carrier ? (ps.empty() ? "FM" : ps) : tr("FM: no station");
            snprintf(b, sizeof b, "%s  %s  SNR %.0f dB", tr(f.state == 2 ? "locked" : f.state == 1 ? "weak" : "searching"), tr(f.stereo ? "stereo" : "mono"), f.snrDb);
            l2 = f.carrier ? b : tr("searching");
        } else if (a.dabMode && rx.standard == 3) {
            l1 = rx.dab.ensembleLabel.empty() ? "DAB" : rx.dab.ensembleLabel;
            snprintf(b, sizeof b, tr("%s  SNR %.0f dB  %d services"), tr(rx.dab.state == 2 ? "locked" : "searching"), rx.dab.snrDb, rx.dab.services);
            l2 = b;
        } else {
            snprintf(b, sizeof b, "%s  SNR %.1f dB  CFO %+.0f Hz", tr(rx.state == 2 ? "locked" : "searching"), rx.dataSnrDb, rx.cfoHz);
            l2 = b;
        }
        if (mapFamilySends(a.family)) {
            const std::string m = mapSendStatusLine();
            if (!m.empty()) {
                if (!l2.empty()) l2 += "  ";
                l2 += m;
            }
        }
    }
}

float summaryWidth(App& a, float scale = 1.f) {
    std::string l1, l2;
    summaryText(a, l1, l2);
    return std::max(ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * scale, FLT_MAX, 0, l1.c_str()).x,
                    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * (scale > 1 ? 1.15f : 1.f), FLT_MAX, 0, l2.c_str()).x);
}

// maxW: the lines are cut short with "..." rather than run into what is to their right
void summary(App& a, float x, float y, float scale, float maxW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string l1, l2;
    summaryText(a, l1, l2);
    const float s1 = ImGui::GetFontSize() * scale, s2 = ImGui::GetFontSize() * (scale > 1 ? 1.15f : 1.f);
    // primary: the mode / station, semibold (drawn twice, a fraction of a pixel apart); secondary: the state, green when locked, amber while searching
    const std::string e1 = ellipsize(l1, maxW, s1);
    dl->AddText(ImGui::GetFont(), s1, ImVec2(x, y), u32(T.bright), e1.c_str());
    dl->AddText(ImGui::GetFont(), s1, ImVec2(x + 0.6f * gUi, y), u32(T.bright), e1.c_str());
    ImU32 c2 = u32(T.dim);
    if (a.engine.running()) {
        if (l2.find(tr("locked")) != std::string::npos) c2 = u32(pal::okGreen());
        else if (l2.find(tr("searching")) != std::string::npos || l2.find(tr("weak")) != std::string::npos) c2 = IM_COL32(230, 180, 70, 255);
    }
    dl->AddText(ImGui::GetFont(), s2, ImVec2(x, y + s1 + 3 * gUi), c2, ellipsize(l2, maxW, s2).c_str());
}

// the fold state of the mode list groups, kept in the prefs by group name (never by number, so regrouping cannot open the wrong ones)
static bool groupOpen(const App& a, int g) { return ("|" + a.modeGroupsOpen + "|").find("|" + std::string(kGroupNames[g]) + "|") != std::string::npos; }

static void setGroupOpen(App& a, int g, bool on) {
    std::string out;
    for (int k = 0; k < kNumGroups; k++) if (k == g ? on : groupOpen(a, k)) { if (!out.empty()) out += "|"; out += kGroupNames[k]; }
    a.modeGroupsOpen = out;
    savePrefs(a);
}

// the modes as a plain radio list, grouped like the menu
void modeList(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const ImVec2 first = ImGui::GetCursorScreenPos();
    static char q[32] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modeside", tr("search modes (FT8, LoRa, APRS...)"), q, sizeof q);
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) q[0] = 0;
    const bool searching = q[0] != 0;
    int shown = 0;
    for (int g = 0; g < kNumGroups; g++) {
        int matches = 0, members = 0;
        const ModeDef* cur = nullptr;
        for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g) {
            if (!modeListed(a, kModes[i], "") && !searching) continue;   // hidden by Modes shown (a search still counts them)
            members++;
            if (modeMatches(kModes[i], q)) matches++;
            if (modeSelected(a, kModes[i])) cur = &kModes[i];
        }
        if (!members || (searching && !matches)) continue;
        if (shown++) ImGui::Dummy(ImVec2(0, 1 * gUi));   // the same small gap above every heading but the first
        // the group header: a thin accent in the group's colour, a fold arrow, the name, the member count, and the selected mode when folded
        const float hh = ImGui::GetFrameHeight() - 4 * gUi, hw = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(1000 + g);
        if (ImGui::InvisibleButton("##grp", ImVec2(hw, hh)) && !searching) setGroupOpen(a, g, !groupOpen(a, g));
        ImGui::PopID();
        const bool unfolded = searching || groupOpen(a, g);
        const ImVec2 g0 = ImGui::GetItemRectMin();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(g0, ImVec2(g0.x + hw, g0.y + hh), IM_COL32(255, 255, 255, 10));
        dl->AddRectFilled(ImVec2(g0.x, g0.y + 3 * gUi), ImVec2(g0.x + 2.5f * gUi, g0.y + hh - 3 * gUi), kGroupCols[g], 1.f);
        const float ty = g0.y + (hh - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::RenderArrow(dl, ImVec2(g0.x + 7 * gUi, ty), u32(T.dim), unfolded ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
        const float nx = g0.x + 22 * gUi;
        dl->AddText(ImVec2(nx, ty), u32(T.dim), tr(kGroupNames[g]));
        char cnt[16];
        if (searching) snprintf(cnt, sizeof cnt, "%d/%d", matches, members); else snprintf(cnt, sizeof cnt, "%d", members);
        const float cx = nx + ImGui::CalcTextSize(tr(kGroupNames[g])).x + 7 * gUi;
        const ImVec2 cs0 = ImGui::CalcTextSize(cnt);
        dl->AddRectFilled(ImVec2(cx - 3 * gUi, ty), ImVec2(cx + cs0.x + 3 * gUi, ty + cs0.y), IM_COL32(255, 255, 255, 14), 3 * gUi);
        dl->AddText(ImVec2(cx, ty), (kGroupCols[g] & 0x00FFFFFF) | 0xC0000000, cnt);
        if (!unfolded && cur) {
            const ImVec2 cs = ImGui::CalcTextSize(cur->name);
            if (cx + cs0.x + 12 * gUi < g0.x + hw - cs.x - 6 * gUi) dl->AddText(ImVec2(g0.x + hw - cs.x - 6 * gUi, ty), u32(T.accent), cur->name);
        }
        if (!unfolded) continue;
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.group != g || !modeListed(a, m, q)) continue;
            const float h = ImGui::GetFrameHeight() - 2 * gUi, w = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##mode", ImVec2(w, h));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const bool sel = modeSelected(a, m);
            if (hov) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 12));
            const ImVec2 c(p0.x + 14 * gUi, p0.y + h * 0.5f);   // under the heading's arrow, the name under the heading's name
            dl->AddCircle(c, 5 * gUi, u32(sel ? T.bright : T.dim), 0, 1.f);
            if (sel) dl->AddCircleFilled(c, 2.6f * gUi, u32(T.accent));
            const bool hid = searching && modeHiddenNow(a, m);   // found by the search only: tagged
            dl->AddText(ImVec2(p0.x + 28 * gUi, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), u32(sel ? T.bright : (running || hid ? T.dim : T.text)), hid ? (std::string(m.name) + " (" + tr("hidden") + ")").c_str() : m.name);
            const char* sub = hid ? "" : tr(m.sub);
            const ImVec2 ss = ImGui::CalcTextSize(sub);
            if (p0.x + 28 * gUi + ImGui::CalcTextSize(m.name).x + 10 * gUi < p0.x + w - ss.x - 6 * gUi)   // no room: the name alone, not both on top of each other
                dl->AddText(ImVec2(p0.x + w - ss.x - 6 * gUi, p0.y + (h - ss.y) * 0.5f), u32(T.dim), sub);
            if (clicked) selectMode(a, m.family, m.subId);
            if (hov && running && !sel) ImGui::SetTooltip("%s", tr("Click to switch mode and restart receiver automatically"));
        }
    }
    if (!shown) ImGui::TextDisabled(tr("no mode matches \"%s\""), q);
    a.tgMin[TgSwitch] = first; a.tgMax[TgSwitch] = ImVec2(first.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
}

void sidebar(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * gUi, 4 * gUi));
    // the tour points at the sections: Source, and Gain with Tuner (next to each other)
    const float sideX0 = ImGui::GetWindowPos().x, sideX1 = sideX0 + ImGui::GetWindowSize().x;
    const float srcY0 = ImGui::GetCursorScreenPos().y;
    float gainY0 = srcY0;
    auto sectionDone = [&]() { gainY0 = ImGui::GetCursorScreenPos().y; };
    if (section(N_("Source"))) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbSource, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    sectionDone();
    tourMark("source", ImVec2(sideX0, srcY0), ImVec2(sideX1, gainY0));
    if (section(N_("Gain"))) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbGain, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    tourMark("gain", ImVec2(sideX0, gainY0), ImVec2(sideX1, ImGui::GetCursorScreenPos().y));   // the gain lesson
    if (section(a.fmMode ? N_("Tuner (FM)###Tuner") : a.dabMode ? N_("Tuner (DAB)###Tuner") : N_("Tuner"))) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbTuner, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    tourMark("tuning", ImVec2(sideX0, gainY0), ImVec2(sideX1, ImGui::GetCursorScreenPos().y));
    if (section(N_("Mode"))) { ImGui::Indent(6 * gUi); modeList(a); ImGui::Unindent(6 * gUi); ImGui::Spacing(); }
    if (section(N_("Display"))) {
        ImGui::Indent(6 * gUi);
        if (ImGui::Checkbox(tr("Peak hold"), &a.peakHold)) savePrefs(a);
        const float col = ImGui::GetCursorPosX() + ImGui::CalcTextSize(tr("Waterfall")).x + 8 * gUi;   // the controls line up after the longer label
        const float autoW = ImGui::CalcTextSize(tr("Auto")).x + ImGui::GetStyle().FramePadding.x * 2 + 4 * gUi;
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Spectrum")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::DragFloatRange2("##specdb", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f")) a.specAutoDb = false;
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Waterfall")); ImGui::SameLine(col);
        ImGui::SetNextItemWidth(std::min(104.f * gUi, ImGui::GetContentRegionAvail().x - autoW));
        if (ImGui::DragFloatRange2("##wfdb", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f")) a.wf.autoTrack = a.specAutoDb = false;
        ImGui::SameLine(0, 4 * gUi);
        wfAutoButton(a);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Layout")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        {
            const char* lay[2] = {tr("Classic"), tr("Unified")};
            int cur = a.layoutUnified ? 1 : 0;
            if (kit::combo("##layoutsel", &cur, lay, 2)) { a.layoutUnified = cur == 1; savePrefs(a); }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Unified: the spectrum and waterfall stay on screen above the decoded views (F6 folds them)"));
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Look")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        {
            static const char* lk[3] = {N_("Classic"), N_("Dev A"), N_("Dev B")};
            const char* names[3] = {tr(lk[0]), tr(lk[1]), tr(lk[2])};
            int cur = a.uiTheme == 3 ? 1 : a.uiTheme == 4 ? 2 : 0;
            if (kit::combo("##looksel", &cur, names, 3)) { a.uiTheme = cur == 0 ? 1 : 2 + cur; savePrefs(a); }
        }
        if (ImGui::SmallButton(tr("Reset spectrum"))) resetSpectrum(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Restore the default spectrum and waterfall levels and the usual half-and-half split (does not change panel layout)"));
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Palette")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##wfmap", tr(Waterfall::mapName(a.wf.effMap())))) {   // also in the Waterfall popup on the plot
            for (int i = 0; i < Waterfall::NumMaps; i++)
                if (ImGui::Selectable(tr(Waterfall::mapName(i)), a.wf.effMap() == i)) { a.wf.map = i; a.wf.palette(); savePrefs(a); }
            ImGui::EndCombo();
        }
        {
            const char* ts[] = {N_("Classic"), N_("Simple")};
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Tuner style")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##tunerstyle", tr(ts[std::max(0, std::min(1, a.tunerStyle))]))) {
                for (int i = 0; i < 2; i++) if (ImGui::Selectable(tr(ts[i]), a.tunerStyle == i)) { a.tunerStyle = i; savePrefs(a); }
                ImGui::EndCombo();
            }
        }
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if ((a.family == 0 || a.family == 3 || captureRowShown(a) || ((modeUi(a.family) && modeUi(a.family)->decoder) || *dect2::bbf::keyForFamily(a.family))) && section(N_("Decoder"))) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbDecoder, true);
        ImGui::Unindent(6 * gUi);
    }
    if (section(N_("Tools"), false)) {   // what used to be only in the View menu; folded until it is wanted
        ImGui::Indent(6 * gUi);
        auto tool = [&](const char* label, bool& on, const char* tipText) {
            if (ImGui::Checkbox(tr(label), &on)) savePrefs(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(tipText));
        };
        tool("Passes", a.passesOpen, "Satellite passes and automatic tuning");
        tool("Scheduler...", a.schedulerOpen, "Timed jobs and satellite passes: tune, record, then put everything back");
        tool("Recordings...", a.recmgrOpen, "Every recording in one list: search, play, trim, convert, tag, delete with undo");
        tool("Direction finder", a.dfOpen, N_("Bearing and position of a transmitter from an antenna array such as the KrakenSDR"));
        tool("Interference hunter", a.rfiOpen, N_("Watch a band for new interference, tell what kind it is, and walk to find it with a loud / quiet meter"));
        tool("Passive radar", a.passiveRadarOpen, N_("Aircraft from the echoes of a broadcast transmitter, with two coherent channels"));
        tool("Images", a.galleryOpen, "The pictures the receivers have decoded");
        tool("Data feeds...", a.feedsOpen, "Send decoded data to other programs and services");
        tool("LAN swarm...", a.swarmOpen, "See and cooperate with other OnAir programs on this network (opt-in)");
        tool("Messages...", a.msgdbOpen, N_("Every decoded message in one searchable database, with filters and export"));
        tool("Protocol lab...", a.protolabOpen, N_("Work out an unknown digital signal: modulation, baud rate, sync word, CRC and fields, with an export"));
        tool("Signal Lab...", a.signallabOpen, N_("Blind analysis of an unknown digital signal from the spectrum or a recording, stage by stage with confidences, a bit viewer and a Mode Builder export"));
        tool("Mode Builder...", a.modeBuilderOpen, N_("Build a decoder without code: describe the modulation, sync word, frame, FEC, CRC and fields, test it on a recording, add it to the mode list"));
        tool("TX studio...", a.txStudioOpen, N_("Make a transmit signal as a file: spectrum, waterfall, measurements and a decoder check. No radio, nothing is transmitted"));
        tool("Digital video generator...", a.videoGenOpen, N_("Encode a camera, a still picture or a video file with an overlay into an MPEG transport stream: a .ts file or a UDP / RTP stream (port 5000)"));
        tool("Integrations...", a.integrationsOpen, N_("Send decoded events (aircraft, ships, APRS, pagers, sondes ...) to an MQTT broker, Home Assistant, a webhook or a JSON-lines file. Off until you set it up"));
        tool("Watchlists...", a.watchlistOpen, N_("Alerts when listed aircraft, ships, call signs, frequencies or words are heard: notification, sound, ntfy, Telegram or webhook"));
        tool("Automations...", a.automationOpen, N_("Rules and scripts: when something is decoded, notify, record, tune or call a webhook"));
        tool("Plugins...", a.pluginsOpen, N_("Add decoders and actions without rebuilding OnAir: install, enable and set up plugins"));
        tool("Memory...", a.memoryOpen, "The frequencies you keep, with mode and settings (Ctrl+M)");
        tool("Datasets...", a.datasetsOpen, N_("Reference data (frequency lists, airports, band plans): source, licence, your own files over the bundled ones, opt-in updates"));
        tool("Data packs...", a.packsOpen, N_("Download map tiles, satellite elements and airports once, to work without internet"));
        tool("Frequencies...", a.freqOpen, "Known frequencies of every mode (Ctrl+Shift+F)");
        tool("Transmit...", txViewFlag(), N_("Send this mode's own signal through a radio that can transmit. Off until you start it; needs a licence or permission."));
        tool("Map", a.mapOpen, "A world map: find a position by coordinates or Maidenhead locator, distance and bearing from your location");
        tool("Propagation...", a.propOpen, "Where a transmitter can be received: Simple (a preset, power, a plain answer and a coverage map) or Advanced (link budget, receiver and SDR, sweeps, compare, terrain from your own elevation file)");
        tool("Antenna designer...", a.antdOpen, N_("Design and model wire antennas: templates, impedance and SWR across a band, radiation pattern, optimiser, calculators"));
        tool("Antenna rotator...", a.rotatorOpen, "Connect a GS-232-compatible rotator (serial or TCP), set azimuth and elevation");
        tool("Sweep scanner", a.sweepOpen, N_("Fast wideband sweep with a noise-floor measurement, signal log and waterfall"));
        tool("Reception doctor", a.doctorOpen, N_("Why is nothing decoding? Checks overload, level, offset, bandwidth, interferers, drops and the mode from the live signal, with one-click fixes"));
        tool("RF fingerprint", a.rfFpOpen, N_("Recognise an individual transmitter by its RF fingerprint (carrier offset, key-up transient, pilot, spurs), also when it sends only a carrier. Logs every transmission."));
        tool("Station health", a.stationHealthOpen, N_("Noise floor, ADC level and reference stations per band, with plain advice on gain, filters and antenna placement"));
        tool("Scanner (highly experimental)", a.scanViewOpen, "HIGHLY EXPERIMENTAL: memories, scan list and tones in one view; steps through them and stops on activity");
        ImGui::Separator();
        themeChoice(a, false);
        accentMenu();
        typographyChoice(false);
        dataFolderChoice();
        timeDisplayChoice(false);
        int pct = (int)std::lround(a.uiTextScale * 100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##sidetxt", &pct, 80, 200, tr("Text size %d%%"))) a.uiTextScale = std::max(0.8f, std::min(2.f, pct / 100.f));
        if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
        if (ImGui::Checkbox(tr("Touch interface"), &a.touchUi)) savePrefs(a);   // big targets, a bottom dock, full-screen pickers (ui/touch.h)
        if (ImGui::IsItemHovered()) wrapTooltip(280 * gUi, tr("For tablets and touch screens: big buttons, an on-screen keypad for the frequency, a full-screen mode picker."));
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (section(N_("Log"))) {
        if (ImGui::Checkbox(tr("Verbose"), &a.verboseLog)) savePrefs(a);   // 3.5: the same switch as View > Verbose logging
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds USB transfer counts, bytes per second, stall restarts and frame times to the log."));
        ImGui::SameLine();
        size_t nl = 0;
        const std::vector<std::string> all = a.engine.logSnapshot(nl);
        if (ImGui::SmallButton(tr("Save log..."))) {   // the lines shown below, as a text file to send with a bug report
            const std::string path = saveFileDialog("onair-log.txt");
            if (!path.empty()) { if (FILE* f = fopen(path.c_str(), "wb")) { for (const std::string& l : all) { fputs(l.c_str(), f); fputc('\n', f); } fclose(f); } }
        }
        ImGui::BeginChild("##logc", ImVec2(0, std::max(40.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)), ImGuiChildFlags_Borders);
        size_t n = 0;
        const std::vector<std::string> lines = all;
        for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
    ImGui::PopStyleVar();
}

// height of the labelled cells of the status strip: caption row + value row + padding (follows the text size)
// The real line heights of the two sizes (measured with the fonts pushed, so a high-DPI font whose line is taller than its size still fits).
struct StripLines { float cap, val; };
static StripLines stripLines() {
    const float fs = ImGui::GetFontSize();
    StripLines l;
    ImGui::PushFont(nullptr, fs * 0.78f); l.cap = std::max(fs * 0.78f, ImGui::GetTextLineHeight()); ImGui::PopFont();
    ImFont* f = typo::fonts().monoBold ? typo::fonts().monoBold : nullptr;
    ImGui::PushFont(f, fs * 1.3f); l.val = std::max(fs * 1.3f, ImGui::GetTextLineHeight()); ImGui::PopFont();
    return l;
}
float statusStripBlockHeight() {
    const StripLines l = stripLines();
    return std::max(ImGui::GetFrameHeight(), l.cap + l.val + 8 * gUi);   // 3 above the caption, 2 between, 3 below the value
}

// the status strip: labelled cells (frequency, mode, receiver, state, CPU level, mute), then the rate / level / load / log fields separated by rules
void statusLine(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool run = a.engine.running();
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    const float lh = ImGui::GetTextLineHeight();
    // ---- the labelled cells: FREQUENCY, MODE, RECEIVER, STATE, CPU level, and the mute button at the right edge (collapse order: statusstrip::plan)
    const bool scanning = !run && a.scanner.progress().running;   // the receiver let go of the radio for a channel scan
    static double cpuPct = 0, lastCpu = 0, lastWall = 0;   // this process's CPU (100 = one core), measured once a second
    {
        const double wall = ImGui::GetTime();
        if (wall - lastWall >= 1.0) {
            const double cpu = plat::processCpuSeconds();
            if (lastWall > 0) cpuPct = (cpu - lastCpu) / (wall - lastWall) * 100;
            lastCpu = cpu; lastWall = wall;
            if (const char* fk = getenv("DECT2_FAKE_CPU")) cpuPct = atof(fk);   // dev: simulate a load (percent of one core) to see the "Too slow" warning
            cpuReliefTick(a, (float)cpuPct, (int)std::max(1u, std::thread::hardware_concurrency()));
        }
        gCpuPct = (float)cpuPct;
    }
    const SampleLoss sl = a.engine.sampleLoss();
    const float stripX0 = ImGui::GetCursorScreenPos().x, stripY0 = ImGui::GetCursorScreenPos().y, stripR = stripX0 + ImGui::GetContentRegionAvail().x;
    const float blockH = statusStripBlockHeight();
    float limit = stripR;   // the trailing fields (rate, level, log) stop here
    {
        const float fs = ImGui::GetFontSize(), capS = fs * 0.78f, valS = fs * 1.3f;
        const float padX = 8 * gUi, gap = 6 * gUi, spc = 0.8f * gUi, rnd = 3.5f * gUi;
        ImFont* mono = typo::fonts().mono ? typo::fonts().mono : a.mono;
        ImFont* monoB = typo::fonts().monoBold ? typo::fonts().monoBold : mono;
        const auto upper = [](std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; };
        const auto capW = [&](const std::string& s) { ImGui::PushFont(nullptr, capS); float w = 0; for (size_t i = 0; i < s.size();) { const size_t n = utf8Len(s[i]); const std::string t = s.substr(i, n); w += ImGui::CalcTextSize(t.c_str()).x + spc; i += n; } ImGui::PopFont(); return w; };
        const auto textW = [&](const std::string& s, ImFont* f, float sz) { ImGui::PushFont(f, sz); const float w = ImGui::CalcTextSize(s.c_str()).x; ImGui::PopFont(); return w; };
        const std::string capF = upper(tr("Frequency")), capM = upper(tr("Mode")), capR = upper(tr("Receiver")), capS2 = upper(tr("State")), capC = "CPU";
        const std::string stateTxt = tr(run ? "Receiving" : scanning ? "Channel scan" : "Stopped");
        const statusstrip::FreqParts fp = statusstrip::splitFreq(listenMhz(a));   // the listening frequency (the VFO), as the big readout
        const std::string rxTxt = std::to_string(a.rxId), mhzTxt = " MHz";
        char pc[16]; snprintf(pc, sizeof pc, "%.0f%%", cpuPct);
        const float lampW = 9 * gUi, barW = 44 * gUi, mhzSz = valS * 0.62f;
        const float freqV = textW(fp.lead, monoB, valS) + textW(fp.rest, monoB, valS) + textW(mhzTxt, a.mono, mhzSz) + 3 * gUi;
        const float modeV = textW(cur->name, monoB, valS * 0.85f), rxV = textW(rxTxt, monoB, valS * 0.85f);
        const float sparkW = 34 * gUi;
        const float stateV = lampW + textW(stateTxt, nullptr, fs), cpuV = barW + 6 * gUi + textW("100%", mono, fs) + 6 * gUi + sparkW;
        const auto cell = [&](float cap, float val) { return statusstrip::Cell{std::max(cap, val) + 2 * padX, val + 2 * padX}; };
        statusstrip::Widths w{cell(capW(capF), freqV), cell(capW(capM), modeV), cell(capW(capR), rxV), cell(capW(capS2), stateV), cell(capW(capC), cpuV),
                              barW + 2 * padX, ImGui::GetFrameHeight(), gap};
        const statusstrip::Plan pl = statusstrip::plan(stripR - stripX0, w);
        limit = stripR - w.mute - gap;
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        float x = stripX0;
        const StripLines sl2 = stripLines();
        const float capY = stripY0 + 3 * gUi, valY = capY + sl2.cap + 2 * gUi, valH = sl2.val, lhRatio = sl2.val / valS;
        const auto box = [&](float cw, float& cx) { const ImVec2 a0(cx, stripY0), b0(cx + cw, stripY0 + blockH);
            dl2->AddRectFilled(a0, b0, u32(T.bg), rnd); dl2->AddRect(a0, b0, u32(T.edge), rnd, 0, 1.f); return a0; };
        const auto caption = [&](float cx, const std::string& s) {
            if (!pl.captions || s.empty()) return;
            ImGui::PushFont(nullptr, capS);
            float px = cx + padX;
            for (size_t i = 0; i < s.size();) { const size_t n = utf8Len(s[i]); const std::string t = s.substr(i, n); dl2->AddText(ImVec2(px, capY), u32(T.dim), t.c_str()); px += ImGui::CalcTextSize(t.c_str()).x + spc; i += n; }   // by character, not by byte: a Chinese caption was bytes shown as "?" (#297)
            ImGui::PopFont();
        };
        const auto valueAt = [&](float cx, float cw, float tw, float vy, float sz, ImFont* f, ImU32 col, const std::string& s) {   // centred in the value row
            ImGui::PushFont(f, sz); dl2->AddText(ImVec2(cx + (cw - tw) * 0.5f, vy), col, s.c_str()); ImGui::PopFont();
        };
        const float rowY = pl.captions ? valY : stripY0 + (blockH - valH) * 0.5f;
        const auto rowYFor = [&](float sz) { return rowY + (valH - sz * lhRatio) * 0.5f; };
        {   // FREQUENCY: first three digits in the accent, the rest bright, MHz small and dim
            const float cw = pl.captions ? w.freq.cap : w.freq.bare;
            const ImVec2 o = box(cw, x);
            caption(x, capF);
            float px = x + padX + (cw - 2 * padX - freqV) * 0.5f;
            ImGui::PushFont(monoB, valS);
            dl2->AddText(ImVec2(px, rowY), u32(T.accent), fp.lead.c_str()); px += ImGui::CalcTextSize(fp.lead.c_str()).x;
            dl2->AddText(ImVec2(px, rowY), u32(T.bright), fp.rest.c_str()); px += ImGui::CalcTextSize(fp.rest.c_str()).x + 3 * gUi;
            ImGui::PopFont();
            ImGui::PushFont(a.mono, mhzSz); dl2->AddText(ImVec2(px, rowY + valS - mhzSz * 1.15f), u32(T.dim), "MHz"); ImGui::PopFont();
            (void)o; x += cw + gap;
        }
        if (pl.showMode) {
            const float cw = pl.captions ? w.mode.cap : w.mode.bare;
            box(cw, x); caption(x, capM);
            valueAt(x, cw, modeV, rowYFor(valS * 0.85f), valS * 0.85f, monoB, u32(T.bright), cur->name);
            x += cw + gap;
        }
        if (pl.showRx) {
            const float cw = pl.captions ? w.rx.cap : w.rx.bare;
            box(cw, x); caption(x, capR);
            valueAt(x, cw, rxV, rowYFor(valS * 0.85f), valS * 0.85f, monoB, u32(T.bright), rxTxt);
            x += cw + gap;
        }
        if (pl.showState) {
            const float cw = pl.captions ? w.state.cap : w.state.bare;
            box(cw, x); caption(x, capS2);
            const ImVec4 lamp = run || scanning ? pal::okGreen() : pal::grey();
            const float tx = x + (cw - stateV) * 0.5f, cy = rowY + valH * 0.5f;
            dl2->AddCircleFilled(ImVec2(tx + 3.5f * gUi, cy), 3.5f * gUi, u32(lamp));
            ImGui::PushFont(nullptr, fs); dl2->AddText(ImVec2(tx + lampW, cy - ImGui::GetTextLineHeight() * 0.5f), u32(run || scanning ? T.bright : T.dim), stateTxt.c_str()); ImGui::PopFont();
            x += cw + gap;
        }
        if (pl.showCpu) {   // CPU as a level: a thin bar and the percentage, amber then orange then red
            const float cw = !pl.showCpuLabel ? w.cpuNoLabel : pl.captions ? w.cpu.cap : w.cpu.bare;
            const float cx0 = x;
            box(cw, x); if (pl.showCpuLabel) caption(x, capC);
            const int lv = statusstrip::cpuLevel((float)cpuPct);
            const ImVec4 lc = lv == 2 ? pal::badRed() : lv == 1 ? pal::warnAmber() : T.accent;
            const float bx = x + (cw - cpuV) * 0.5f, cy = rowY + valH * 0.5f, bh = 4 * gUi;
            dl2->AddRectFilled(ImVec2(bx, cy - bh * 0.5f), ImVec2(bx + barW, cy + bh * 0.5f), u32(T.edge), 2 * gUi);
            const float fill = statusstrip::cpuFill((float)cpuPct, (int)std::max(1u, std::thread::hardware_concurrency()));
            dl2->AddRectFilled(ImVec2(bx, cy - bh * 0.5f), ImVec2(bx + std::max(fill, 0.02f) * barW, cy + bh * 0.5f), u32(lc), 2 * gUi);
            ImGui::PushFont(mono, fs); dl2->AddText(ImVec2(bx + barW + 6 * gUi, cy - ImGui::GetTextLineHeight() * 0.5f), u32(lv ? lc : T.bright), pc);
            const float sx = bx + barW + 12 * gUi + textW("100%", mono, fs);
            ImGui::PopFont();
            cpuSparkDraw(dl2, ImVec2(sx, cy - valH * 0.4f), ImVec2(sparkW, valH * 0.8f), u32(lv ? lc : T.accent), u32(T.edge), (int)std::max(1u, std::thread::hardware_concurrency()));
            if (ImGui::IsMouseHoveringRect(ImVec2(cx0, stripY0), ImVec2(cx0 + cw, stripY0 + blockH)) && ImGui::IsWindowHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Process CPU: %.0f%% (100%% = one core). The line is the last 30 s; the thin rule is 85%% of the cores.", cpuPct);
                if (run && sl.live) ImGui::Text("DSP load: %.0f%% of real time on the sample thread (processing time, not lost samples)", sl.loadPct);
                if (run && sl.live && sl.deliveredPct >= 0)
                    ImGui::Text(sl.rateLow ? "Samples arriving: %.1f%% of the set rate (below 97%%: the radio or its driver)" : "Samples arriving: %.1f%% of the set rate", sl.deliveredPct);
                ImGui::TextDisabled("Drawing is slowed when nothing moves and paused while hidden.");
                ImGui::EndTooltip();
            }
            x += cw + gap;
        }
        // the mute button at the right edge (the same click as the volume control)
        ImGui::SetCursorScreenPos(ImVec2(stripR - w.mute, stripY0 + (blockH - w.mute) * 0.5f));
        if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, tr(a.muted ? "Unmute" : "Mute"))) {
            a.muted = !a.muted;
            a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, stripY0 + (blockH - lh) * 0.5f));   // the trailing fields continue after the cells
    }
    bool fresh = true;
    if (run) {   // the one signal indicator (same in every mode), then the "too slow" warning with its fix
        const float sw = signalStripWidth(a);
        if (ImGui::GetCursorScreenPos().x + sw < limit) {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            signalStripCell(a, c.x, c.y - (blockH - lh) * 0.5f, blockH);
            ImGui::Dummy(ImVec2(sw, lh));
            fresh = false;
        }
    }
    {
        if (!fresh) ImGui::SameLine(0, 12 * gUi);
        if (cpuReliefStrip(a, limit)) fresh = false;
    }
    auto field = [&](const char* text, bool first = false, ImVec4 col = ImVec4(-1, 0, 0, 0), bool fit = false) {
        const float gap = 8 * gUi;
        const float left = fresh ? ImGui::GetCursorScreenPos().x : ImGui::GetItemRectMax().x + 0;
        if (!first && !fresh) {
            // the rule sits in the middle of an 8 px gap on each side: text, rule, text never touch
            if (left + 2 * gap + 1 + (fit ? 40 * gUi : ImGui::CalcTextSize(text).x) > limit) return;   // no room left on the bar
            ImGui::SameLine(0, gap);
            const ImVec2 p = ImGui::GetCursorScreenPos();   // gap after the previous text
            dl->AddLine(ImVec2(p.x + 0.5f, p.y + 1), ImVec2(p.x + 0.5f, p.y + lh - 1), u32(T.dim));
            ImGui::SameLine(0, gap + 1);   // the next text starts a gap past the rule (1 px wide)
        } else if (ImGui::GetCursorScreenPos().x + (fit ? 40 * gUi : ImGui::CalcTextSize(text).x) > limit) return;
        const std::string t = fit ? ellipsize(text, limit - ImGui::GetCursorScreenPos().x, ImGui::GetFontSize()) : text;   // the log line: as much as fits
        if (col.x < 0) ImGui::TextUnformatted(t.c_str()); else ImGui::TextColored(col, "%s", t.c_str());
        fresh = false;
    };
    char b[96];
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); field(b); }
    if (run) { snprintf(b, sizeof b, tr("level %.1f dBFS"), a.spec.stats.rmsDbfs); field(b); }
    if (run) {
        const SampleLoss l = a.engine.sampleLoss();
        static OverloadHint hint;   // once per start: DSP drops repeating within 5 s
        if (hint.update(l.cpuEvents, ImGui::GetTime())) a.engine.log(tr("This mode is too slow for this computer: samples are being dropped. Try a lower sample rate or a narrower mode."));
        if (l.live) { snprintf(b, sizeof b, tr("load %.0f%%"), l.loadPct); field(b, false, l.loadPct > 90 ? pal::warnAmber() : ImVec4(-1, 0, 0, 0)); lossTooltip(l); }
        if (l.radioEvents || l.cpuEvents || l.rateLow) { field((std::string(tr("dropped")) + " " + lossText(l)).c_str(), false, lossColour(l)); lossTooltip(l); }
    }
    {
        size_t n = 0;
        const std::vector<std::string> lines = a.engine.logSnapshot(n);
        if (!lines.empty()) field(lines.back().c_str(), false, ImVec4(-1, 0, 0, 0), true);
    }
}

const char* kVariantNames[8] = {N_("Sidebar"), N_("Scope"), N_("Tiles"), N_("Faceplate"), N_("Scope: Cinema"), N_("Scope: Meters"), N_("Scope: Dock"), N_("Panel")};

// the View menu: interface variants, text size, light / high contrast, language, accent, the classic interface
static void viewMenu(App& a) {
    const float vx0 = ImGui::GetCursorScreenPos().x, vy0 = ImGui::GetWindowPos().y, vy1 = vy0 + ImGui::GetWindowHeight();
    struct Mark { float x0, y0, y1; ~Mark() { tourMark("view", ImVec2(x0, y0), ImVec2(ImGui::GetCursorScreenPos().x, y1)); } } mark{vx0, vy0, vy1};   // the tour points at the View menu
    gViewMenuCentre = ImVec2(ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(tr("View")).x * 0.5f + 6 * gUi, ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f);
    if (ImGui::BeginMenu(tr("View"))) {
        if (ImGui::BeginMenu(tr("Layout"))) {   // both switches act at once, the receiver keeps running; the same two values as Settings > Display
            if (ImGui::MenuItem(tr("Classic"), "Ctrl+Shift+L", !a.layoutUnified)) { a.layoutUnified = false; savePrefs(a); }
            if (ImGui::MenuItem(tr("Unified"), "Ctrl+Shift+L", a.layoutUnified)) { a.layoutUnified = true; savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Try the new layout: the spectrum and waterfall stay on screen above the decoded views"));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Look"))) {
            const int cur = a.uiTheme == 3 ? 1 : a.uiTheme == 4 ? 2 : 0;
            if (ImGui::MenuItem(tr("Classic"), nullptr, cur == 0)) { if (cur != 0) a.uiTheme = 1; savePrefs(a); }
            if (ImGui::MenuItem(tr("Dev A"), nullptr, cur == 1)) { a.uiTheme = 3; savePrefs(a); }
            if (ImGui::MenuItem(tr("Dev B"), nullptr, cur == 2)) { a.uiTheme = 4; savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Try the new look: flat, dense, one accent colour"));
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (int v = 0; v < 8; v++) if (ImGui::MenuItem(tr(kVariantNames[v]), nullptr, a.uiVariant == v)) { a.uiVariant = v; savePrefs(a); }
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Text size"))) {
            int pct = (int)std::lround(a.uiTextScale * 100);
            ImGui::SetNextItemWidth(160 * gUi);
            if (ImGui::SliderInt("##txtsz", &pct, 80, 200, "%d%%")) a.uiTextScale = std::max(0.8f, std::min(2.f, pct / 100.f));
            if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
            for (int p : {80, 100, 125, 150, 200}) { char l[16]; snprintf(l, sizeof l, "%d%%", p); if (ImGui::MenuItem(l, nullptr, pct == p)) { a.uiTextScale = p / 100.f; savePrefs(a); } }
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Ctrl + / Ctrl - / Ctrl 0"));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(tr("Touch interface"), nullptr, &a.touchUi)) savePrefs(a);   // the same flag as the side panel: big targets, bottom dock (ui/touch.h)
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For tablets and touch screens: big buttons, an on-screen keypad for the frequency, a full-screen mode picker."));
        dockMenu(a);   // View > Docking (ui2_dock.cpp)
        if (ImGui::MenuItem(tr("Reset layout"))) { if (!a.touchUi) layoutReset();   // forgets the saved window positions of every mode
            a.splitSide = a.splitRight = a.splitBottom = a.splitDock = a.splitBot2 = 0; savePrefs(a); }
        if (ImGui::MenuItem(tr("Reset spectrum"))) resetSpectrum(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Put the panels back to their automatic sizes (drag the bars between panels to resize them)"));
        if (ImGui::MenuItem(tr("Full screen"), "F11", gWindow && glfwGetWindowMonitor(gWindow))) toggleFullscreen();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Fill the display, hiding the taskbar (F11 or Alt+Enter). The same shortcut leaves it."));
        ImGui::Separator();
        static const char* names[5] = {N_("Terminal palette"), N_("Instrument palette"), N_("Mono palette"), N_("Dev A (graphite)"), N_("Dev B (terminal green)")};
        for (int t = 0; t < 5; t++) if (ImGui::MenuItem(tr(names[t]), nullptr, a.uiTheme == t)) { a.uiTheme = t; savePrefs(a); }
        themeChoice(a, true);
        accentMenu();
        typographyChoice(true);
        timeDisplayChoice(true);
        if (ImGui::BeginMenu(tr("Language"))) { languageMenuItems(a); ImGui::EndMenu(); }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Controls (keys and mouse) ..."), nullptr, &inputmap::windowOpen())) {}
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What the mouse wheel, keys and extra mouse buttons do on the spectrum and waterfall: zoom, tune, pan, step size, shortcuts"));
        if (ImGui::MenuItem(tr("Performance overlay"), nullptr, &a.perfOverlay)) savePrefs(a);
        if (ImGui::MenuItem(tr("Verbose logging"), nullptr, &a.verboseLog)) savePrefs(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds USB transfer counts, bytes per second, stall detections and restarts and frame times to the log. For bug reports: switch it on, reproduce the problem, then Save bug report (Diagnostics)."));
        if (ImGui::BeginMenu(tr("Power"))) {   // battery life: dect2/power/power.h (update rates, background work) and app/ui/frame_pace.h (redraws)
            static const char* lp[3] = {N_("Low power mode: off"), N_("Low power mode: automatic (on battery or hot)"), N_("Low power mode: always")};
            for (int i = 0; i < 3; i++) {
                if (ImGui::MenuItem(tr(lp[i]), nullptr, a.powerLowMode == i)) { a.powerLowMode = i; savePrefs(a); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("While it applies: the spectrum and telemetry update less often, the window redraws at 30 fps when you type and at 10 fps behind other windows. A playing TV picture keeps its frame rate."));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Reduce CPU when hidden"), nullptr, &a.reduceCpuHidden)) savePrefs(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("No drawing while the window is minimised or hidden. Decoding goes on."));
            static const char* sh[3] = {N_("Save power when hidden: off"), N_("Save power when hidden: on battery"), N_("Save power when hidden: always")};
            for (int i = 0; i < 3; i++) if (ImGui::MenuItem(tr(sh[i]), nullptr, a.powerHidden == i)) { a.powerHidden = i; savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("While the window is minimised or hidden: the spectrum and telemetry update a few times a second and scans wait. Decoding and audio go on."));
            ImGui::EndMenu();
        }
        rcaps::viewMenuItem();   // Low-power drawing (gfx/render_caps.h)
        {   // opt-in Discord Rich Presence
            const bool have = *discord::appId() != 0;
            ImGui::BeginDisabled(!have);
            if (ImGui::MenuItem(tr("Show in Discord"), nullptr, &a.discordOn)) savePrefs(a);
            ImGui::BeginDisabled(!a.discordOn);
            if (ImGui::MenuItem(tr("Show the frequency"), nullptr, &a.discordFreq)) savePrefs(a);
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr(have ? "Shows on your Discord profile that you are using OnAir, the mode and the frequency. Needs the Discord desktop app running." : "Discord application ID not set in this build"));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Classic interface"))) { a.newUi = false; savePrefs(a); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Tools"))) {
        if (ImGui::MenuItem(tr("Transcripts ..."), nullptr, tx::windowOpen())) tx::windowOpen() = !tx::windowOpen();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What is said on the airband and other voice channels, as text: live captions, search, export. Off until you turn it on."));
        if (ImGui::MenuItem(tr("Passes"), nullptr, &a.passesOpen)) {}
        ImGui::MenuItem(tr("Scheduler..."), nullptr, &a.schedulerOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Timed jobs and satellite passes: tune the radio, record audio, IQ or the log, then put everything back"));
        ImGui::MenuItem(tr("Recordings..."), nullptr, &a.recmgrOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every recording in one list: search, play, trim, convert, tag, delete with undo, and a size budget per folder"));
        ImGui::MenuItem(tr("SatNOGS transmitters"), nullptr, &a.satnogsOpen);
        ImGui::MenuItem(tr("Direction finder"), nullptr, &a.dfOpen);
        ImGui::MenuItem(tr("Interference hunter"), nullptr, &a.rfiOpen);
        ImGui::MenuItem(tr("Passive radar"), nullptr, &a.passiveRadarOpen);
        ImGui::MenuItem(tr("Images"), nullptr, &a.galleryOpen);
        ImGui::MenuItem(tr("Data feeds..."), nullptr, &a.feedsOpen);
        ImGui::MenuItem(tr("LAN swarm..."), nullptr, &a.swarmOpen);
        ImGui::MenuItem(tr("Messages..."), nullptr, &a.msgdbOpen);
        ImGui::MenuItem(tr("Protocol lab..."), nullptr, &a.protolabOpen);
        ImGui::MenuItem(tr("Signal Lab..."), nullptr, &a.signallabOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Blind analysis of an unknown digital signal: select it on the spectrum (Identify or Shift+drag) or load a recording"));
        ImGui::MenuItem(tr("Mode Builder..."), nullptr, &a.modeBuilderOpen);
        ImGui::MenuItem(tr("TX studio..."), nullptr, &a.txStudioOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Make a transmit signal as a file, with spectrum, waterfall, measurements and a decoder check. No radio, nothing is transmitted."));
        if (ImGui::MenuItem(tr("Digital video generator..."), nullptr, &a.videoGenOpen)) savePrefs(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Encode a camera, a still picture or a video file with an overlay into an MPEG transport stream: a .ts file or a UDP / RTP stream (port 5000)"));
        ImGui::MenuItem(tr("Integrations..."), nullptr, &a.integrationsOpen);
        ImGui::MenuItem(tr("Watchlists..."), nullptr, &a.watchlistOpen);
        ImGui::MenuItem(tr("Automations..."), nullptr, &a.automationOpen);
        ImGui::MenuItem(tr("Plugins..."), nullptr, &a.pluginsOpen);
        ImGui::MenuItem(tr("Backup and restore..."), nullptr, &a.backupOpen);
        ImGui::MenuItem(tr("Memory..."), "Ctrl+M", &a.memoryOpen);
        ImGui::MenuItem(tr("Datasets..."), nullptr, &a.datasetsOpen);
        ImGui::MenuItem(tr("Data packs..."), nullptr, &a.packsOpen);
        ImGui::MenuItem(tr("Frequencies..."), "Ctrl+Shift+F", &a.freqOpen);
        if (ImGui::MenuItem(tr("Show Frequencies as a tab"), nullptr, a.freqTab)) freqdbTabToggle(a);
        ImGui::MenuItem(tr("Transmit..."), nullptr, &txViewFlag());
        ImGui::MenuItem(tr("Map"), nullptr, &a.mapOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A world map: find a position by coordinates or Maidenhead locator, distance and bearing from your location"));
        if (ImGui::MenuItem(tr("Sweep scanner"), nullptr, &a.sweepOpen)) {}
        if (ImGui::MenuItem(tr("Station health"), nullptr, &a.stationHealthOpen)) {}
        if (ImGui::MenuItem(tr("Reception doctor"), nullptr, &a.doctorOpen)) {}
        if (ImGui::MenuItem(tr("RF fingerprint"), nullptr, &a.rfFpOpen)) {}
        if (ImGui::MenuItem(tr("Scanner (highly experimental)"), nullptr, &a.scanViewOpen)) {}
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("HIGHLY EXPERIMENTAL: step through a list of frequencies and modes, or a band, and stop on activity"));
        ImGui::MenuItem(tr("Propagation..."), nullptr, &a.propOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Where a transmitter can be received: predicted SNR on the map from power, frequency, time and the SNR you need. No terrain."));
        ImGui::MenuItem(tr("Antenna designer..."), nullptr, &a.antdOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Design and model wire antennas: templates, impedance and SWR across a band, radiation pattern, optimiser, calculators"));
        ImGui::MenuItem(tr("Antenna rotator..."), nullptr, &a.rotatorOpen);
        ImGui::MenuItem(tr("Voice keys..."), nullptr, &a.voiceKeysOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your own decryption keys for digital voice (P25, NXDN). Nothing is decrypted without a key you add."));
        if (ImGui::MenuItem(tr("Calibrate frequency..."))) calibrateFrequencyOpen(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Measure this radio's frequency error (ppm) against an FM station, an ATSC pilot, a time-signal carrier, or by typing what you see"));
        ImGui::EndMenu();
    }
}

// the right end of the menu bar: credit, Credits, Support, the update button and Clear all. One row of chips: the same height, border and
// background, one gap between them, centred in the bar. Placed from the width the row had last frame (it follows the update button's label).
static float gBarRestW = 0;
static void menuBarRight(App& a) {
    const ImGuiStyle& ist = ImGui::GetStyle();
    const float g = 6.f * gUi, margin = 12.f * gUi, W = ImGui::GetWindowWidth();
    const char* credit = tr("by: Kristijonas Adomaitis, made with help from the community");
    const float cw = ImGui::CalcTextSize(credit).x + 2.f * ist.FramePadding.x;
    const float lineH = ImGui::GetTextLineHeight();
    // centred on the line the menu titles are on (their frame is taller than the chips), never touching the bar's bottom edge
    const float y = std::max(0.f, ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - lineH) * 0.5f - 1.f * gUi);
    const float minX = ImGui::GetCursorPosX() + 20 * gUi;
    const float xRest = gBarRestW > 0 ? W - margin - gBarRestW : W - margin - 400 * gUi;
    const bool showBy = gBarRestW > 0 && xRest - g - cw > minX;   // the 'by' text goes first when the menu bar is narrow
    if (gBarRestW > 0 && xRest < minX) return;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    const ImVec4 dimText(0.62f, 0.68f, 0.75f, 1);
    if (showBy) {
        ImGui::SameLine(xRest - g - cw); ImGui::SetCursorPosY(y);
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        if (ImGui::SmallButton(credit)) { a.creditsOpen = true; a.creditsTab = 0; }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Thank you to everyone who reported bugs, sent recordings and opened pull requests."));
        ImGui::SameLine(0, g); ImGui::SetCursorPosY(y);
    } else { ImGui::SameLine(xRest); ImGui::SetCursorPosY(y); }
    ImGui::PushStyleColor(ImGuiCol_Text, dimText);
    if (ImGui::SmallButton(tr("Credits"))) { a.creditsOpen = true; a.creditsTab = 0; }
    ImGui::PopStyleColor();
    const float x0 = ImGui::GetItemRectMin().x;
    ImGui::SameLine(0, g); ImGui::SetCursorPosY(y);
    ImGui::PushStyleColor(ImGuiCol_Text, dimText);
    supportButton();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, g); ImGui::SetCursorPosY(y);
    updateButton(a);
    ImGui::SameLine(0, g); ImGui::SetCursorPosY(y);
    clearAllButtonAt(a, false);
    gBarRestW = ImGui::GetItemRectMax().x - x0;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

float menuBar(App& a) {
    float menuH = 0;
    // a taller bar: the buttons at its right end (credit, Support, version, Clear all) and the menu titles get room below their text
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + 3 * gUi));
    if (ImGui::BeginMainMenuBar()) {
        menuH = ImGui::GetWindowHeight();
        const bool running = a.engine.running();
        if (ImGui::BeginMenu(tr("File"))) {
            if (ImGui::MenuItem(tr(running ? "Stop receiver" : "Start receiver"), "Space")) { if (running) a.engine.stop(); else startReceiver(a); }
            ImGui::Separator();
            workspaceMenu(a);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Exit program"), "Alt+F4")) { if (gWindow) glfwSetWindowShouldClose(gWindow, GLFW_TRUE); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Mode"))) {
            int key = 0;
            bool firstGroup = true;
            for (int g = 0; g < kNumGroups; g++) {
                if (!modeGroupListed(a, g)) continue;   // Modes shown: a hidden group leaves the menu
                if (!firstGroup) ImGui::Separator();
                firstGroup = false;
                for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i])) {
                    char k[4]; snprintf(k, sizeof k, "%d", ++key);
                    if (ImGui::MenuItem(kModes[i].name, key <= 9 ? k : nullptr, modeSelected(a, kModes[i]))) selectMode(a, kModes[i].family, kModes[i].subId);   // only 1-9 are keys
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Modes shown..."))) a.modeVisOpen = true;
            ImGui::EndMenu();
        }
        viewMenu(a);
        const float mx0 = ImGui::GetCursorScreenPos().x, my0 = ImGui::GetWindowPos().y, my1 = my0 + ImGui::GetWindowHeight();
        memoryMenu(a);
        const float mx1 = ImGui::GetCursorScreenPos().x;
        tourMark("memory", ImVec2(mx0, my0), ImVec2(mx1, my1));
        mapSendServerMenu(a);
        const float sx1 = ImGui::GetCursorScreenPos().x;
        tourMark("server", ImVec2(mx1, my0), ImVec2(sx1, my1));
        const float hx0 = ImGui::GetCursorScreenPos().x;
        if (ImGui::BeginMenu(tr("Help"))) {
            if (ImGui::MenuItem(tr("Credits"))) { a.creditsOpen = true; a.creditsTab = 0; }
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Setup and lessons"))) {
                if (ImGui::MenuItem(tr("Run setup again"))) setupWizardOpen(a);
                if (ImGui::MenuItem(tr("Lessons"))) a.lessonsOpen = true;
                if (ImGui::MenuItem(tr("Continue the lessons"))) lessonsStart(a);
                if (ImGui::MenuItem(tr("Concepts (glossary)"))) a.glossaryOpen = true;
                if (ImGui::MenuItem(tr("Checkup"))) a.checkupOpen = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(tr("Guided tours"))) {
                if (ImGui::MenuItem(tr("Overview"))) tourStart(a);
                ImGui::Separator();
                int nt = 0;
                const tour::Tour* ts = tour::tours(&nt);
                for (int i = 0; i < nt; i++) if (ImGui::MenuItem(tr(ts[i].title))) tourStartTour(a, i);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("Get me listening"))) listenWizardOpen(a);
            if (ImGui::MenuItem(tr("Guided tour"))) tourStart(a);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Documentation"))) dect2::openUrl("https://on-air.dev/docs/");
            if (ImGui::MenuItem(tr("Troubleshooting"))) dect2::openUrl("https://on-air.dev/docs/troubleshooting/");
            if (ImGui::MenuItem(tr("Voice codecs"))) a.voiceCodecsOpen = true;
            if (ImGui::MenuItem(tr("Voice monitor"))) voiceui::open();
            if (ImGui::MenuItem(tr("Website"))) dect2::openUrl("https://on-air.dev/");
            if (ImGui::MenuItem(tr("Credits and licences"))) { a.creditsOpen = true; a.creditsTab = 1; }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Report a problem"))) a.reportOpen = true;
            ImGui::EndMenu();
        }
        tourMark("help", ImVec2(hx0, my0), ImVec2(ImGui::GetCursorScreenPos().x, my1));
        menuBarRight(a);
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    workspaceDialogs(a);
    creditsWindow(a);
    appcodec::voiceCodecsWindow(a);
    voiceui::window(a);
    voiceKeysWindow(a);
    inputmap::window();
    tx::windowPanel(a);
    reportProblemWindow(a);
    modeVisibilityWindow(a);
    backupWindow(a);
    tx::captionsWindow(a);
    return menuH;
}

void topBar(App& a, ImVec2 disp) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 4 * gUi));
    ImGui::BeginChild("##top", ImVec2(0, 50 * gUi), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    a.tgMin[TgToolbar] = ImGui::GetWindowPos(); a.tgMax[TgToolbar] = ImVec2(a.tgMin[TgToolbar].x + ImGui::GetWindowSize().x, a.tgMin[TgToolbar].y + ImGui::GetWindowSize().y);
    {
        const bool running = a.engine.running();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        if (runButton(running, 40 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        const float W = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorScreenPos(ImVec2(o.x + 58 * gUi, o.y));
        const float digitsEnd = freqDigits(a);
        tourMark("freq", ImVec2(o.x - 4 * gUi, o.y), ImVec2(digitsEnd, o.y + 40 * gUi));   // the lessons point at the run button and the digits
        // a narrow window: the level meter and the volume give up width (the meter goes last), then the summary is cut short
        const float sumX = digitsEnd + 34 * gUi;
        float vw = 230 * gUi, mw = 260 * gUi;
        const float spare = o.x + W - sumX - summaryWidth(a) - 40 * gUi;
        if (spare < vw + mw) {
            const float k = std::max(0.f, spare) / (vw + mw);
            vw = std::max(150.f * gUi, vw * k); mw = std::max(130.f * gUi, mw * k);
            if (o.x + W - vw - mw - 20 * gUi < sumX + 120 * gUi) mw = 0;
        }
        const float meterX = o.x + W - vw - mw - 20 * gUi;
        {   // subtle separators between the groups: run | frequency | state | meter | volume
            ImDrawList* sd = ImGui::GetWindowDrawList();
            const ImU32 sc = IM_COL32(255, 255, 255, 28);
            const float y0 = o.y + 4 * gUi, y1 = o.y + 38 * gUi;
            sd->AddLine(ImVec2(o.x + 49 * gUi, y0), ImVec2(o.x + 49 * gUi, y1), sc);
            sd->AddLine(ImVec2(sumX - 17 * gUi, y0), ImVec2(sumX - 17 * gUi, y1), sc);
            if (mw > 0) sd->AddLine(ImVec2(meterX - 9 * gUi, y0), ImVec2(meterX - 9 * gUi, y1), sc);
            sd->AddLine(ImVec2(o.x + W - vw - 4 * gUi, y0), ImVec2(o.x + W - vw - 4 * gUi, y1), sc);
        }
        summary(a, sumX, o.y + 6 * gUi, 1.f, (mw > 0 ? meterX : o.x + W - vw) - 16 * gUi - sumX);
        if (mw > 0) levelMeter(a, meterX, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + W - vw + 4 * gUi, o.y + 8 * gUi));
        volumeControl(a, vw);
        tourMark("audio", ImVec2(o.x + W - vw, o.y), ImVec2(o.x + W, o.y + 40 * gUi));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y), ImVec2(ImGui::GetWindowPos().x + disp.x, ImGui::GetCursorScreenPos().y), u32(kEdge));
}

void bottomBar(App& a, ImVec2 disp) {
    kit::statusBarBegin(disp.x);
    statusLine(a);
    kit::statusBarEnd();
}

void tgRect(App& a, int id) { a.tgMin[id] = ImGui::GetWindowPos(); a.tgMax[id] = ImVec2(a.tgMin[id].x + ImGui::GetWindowSize().x, a.tgMin[id].y + ImGui::GetWindowSize().y); }

void logConsole(App& a, float h) {
    ImGui::BeginChild("##logc", ImVec2(0, h), ImGuiChildFlags_Borders);
    size_t n = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(n);
    for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

void modeCombo(App& a, float w) {
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##modec", cur->name)) {
        const char* q = modeSearchBox();
        for (int g = 0; g < kNumGroups; g++) {
            if (!modeGroupListed(a, g, q)) continue;
            ImGui::Separator();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g]));
            for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i], q)) {
                ImGui::PushID(i);
                if (ImGui::Selectable(modeHiddenNow(a, kModes[i]) ? (std::string(kModes[i].name) + " (" + tr("hidden") + ")").c_str() : kModes[i].name, modeSelected(a, kModes[i]))) selectMode(a, kModes[i].family, kModes[i].subId);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", kModes[i].tip, modeCreditsLine(kModes[i].name).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

void displayControls(App& a, bool wide) {
    if (ImGui::Checkbox(tr("Peak hold"), &a.peakHold)) savePrefs(a);
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("%s", tr("Spectrum")); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth((wide ? 130 : 104) * gUi);
    if (ImGui::DragFloatRange2("##specdb", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f")) a.specAutoDb = false;
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("%s", tr("Waterfall")); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth((wide ? 130 : 104) * gUi);
    if (ImGui::DragFloatRange2("##wfdb", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f")) a.wf.autoTrack = a.specAutoDb = false;
    ImGui::SameLine(0, 4 * gUi);
    wfAutoButton(a);
}

}  // namespace ui2
