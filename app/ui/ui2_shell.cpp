// The new interface: the shell around the body (menu bar, top bar with the mode combo and summary, bottom bar, the
// mode list and side bar, the status line and the log console).
// Split from ui2.cpp; the shared declarations are in ui2_internal.h.
#include "transcribe/transcribe_app.h"
#include "input_map.h"
#include "dect2/dsp/baseband_filter.h"
#include "ui2_internal.h"
#include "menu_groups.h"
#include "ui_preview.h"
#include "../general/general_panels.h"
#include "../state/listen_freq.h"
#include "../codec/speech_codec_ui.h"
#include "../voice/voice_monitor_ui.h"
#include "ui_common.h"
#include "tour_logic.h"
#include "status_strip.h"
#include "row_tint.h"
#include "dect2/common/time_fmt.h"
#include "../platform/proc_cpu.h"
#include <thread>
#include "../state/workspace.h"
#include "../state/layout.h"
#include "../canvas/canvas.h"
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
#include "../badges/badges_ui.h"
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

// the modes as a list grouped like the menu. Each category's heading and its mode rows sit on a background tinted with the category's colour
// (row_tint.h: opaque mixes, text lifted only where a tint needs it, so every theme keeps 4.5:1); the selected mode on a stronger tint with a
// bar in the accent colour and its name in semibold; hover a lighter step of the tint; keyboard focus an outline.
void modeList(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const ImVec2 first = ImGui::GetCursorScreenPos();
    const ImVec4 childBg = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
    const ImVec4 paneV = childBg.w > 0.5f ? childBg : ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const contrast::Rgb pane{paneV.x, paneV.y, paneV.z};
    const rowtint::Look look = a.highContrast ? rowtint::Look::HighContrast : (a.lightUi || a.uiTheme == 7) ? rowtint::Look::Light : rowtint::Look::Dark;
    auto rgb = [](ImVec4 v) { return contrast::Rgb{v.x, v.y, v.z}; };
    auto col = [](contrast::Rgb c) { return u32(ImVec4(c.r, c.g, c.b, 1)); };
    auto textOn = [&](contrast::Rgb fill, ImVec4 t) { return col(rowtint::liftText(fill, rgb(t), look)); };
    const float sp = ImGui::GetStyle().ItemSpacing.y;   // the fills reach over the gap below a row, so a category reads as one block
    ImFont* semi = typo::fonts().semibold;
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
        if (shown++) ImGui::Dummy(ImVec2(0, 1 * gUi));   // the same small gap above every heading but the first: the untinted line between categories
        const ImVec4 catV = ImGui::ColorConvertU32ToFloat4(kGroupCols[g]);
        const contrast::Rgb cat = rgb(catV);
        // the group header: a fold arrow, the name, the member count, and the selected mode when folded, on the category's tint
        const float hh = ImGui::GetFrameHeight() - 4 * gUi, hw = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(1000 + g);
        if (ImGui::InvisibleButton("##grp", ImVec2(hw, hh)) && !searching) setGroupOpen(a, g, !groupOpen(a, g));
        ImGui::PopID();
        const bool unfolded = searching || groupOpen(a, g);
        const ImVec2 g0 = ImGui::GetItemRectMin();
        const bool hovHead = ImGui::IsItemHovered();
        const contrast::Rgb headFill = rowtint::rowFill(pane, cat, rowtint::kHeadAlpha + (hovHead ? 0.06f : 0.f), look);
        dl->AddRectFilled(g0, ImVec2(g0.x + hw, g0.y + hh + (unfolded ? sp : 0.f)), col(headFill));
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) dl->AddRect(ImVec2(g0.x + 1, g0.y + 1), ImVec2(g0.x + hw - 1, g0.y + hh - 1), u32(T.accent), 0, 0, 1.f * gUi);
        const float ty = g0.y + (hh - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::RenderArrow(dl, ImVec2(g0.x + 7 * gUi, ty), textOn(headFill, T.dim), unfolded ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
        const float nx = g0.x + 22 * gUi;
        dl->AddText(ImVec2(nx, ty), textOn(headFill, T.text), tr(kGroupNames[g]));
        char cnt[16];
        if (searching) snprintf(cnt, sizeof cnt, "%d/%d", matches, members); else snprintf(cnt, sizeof cnt, "%d", members);
        const float cx = nx + ImGui::CalcTextSize(tr(kGroupNames[g])).x + 7 * gUi;
        const ImVec2 cs0 = ImGui::CalcTextSize(cnt);
        dl->AddText(ImVec2(cx, ty), textOn(headFill, T.dim), cnt);
        if (!unfolded && cur) {
            const ImVec2 cs = ImGui::CalcTextSize(cur->name);
            if (cx + cs0.x + 12 * gUi < g0.x + hw - cs.x - 6 * gUi) dl->AddText(ImVec2(g0.x + hw - cs.x - 6 * gUi, ty), textOn(headFill, T.accent), cur->name);
        }
        if (!unfolded) continue;
        int last = -1;   // the last row of the group gets no reach over the gap below it (that gap separates the categories)
        for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i], q)) last = i;
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.group != g || !modeListed(a, m, q)) continue;
            const float h = ImGui::GetFrameHeight() - 2 * gUi, w = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##mode", ImVec2(w, h));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const bool sel = modeSelected(a, m);
            const contrast::Rgb fill = rowtint::rowFill(pane, cat, sel ? rowtint::kSelAlpha : hov ? rowtint::kHoverAlpha : rowtint::kModeAlpha, look);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h + (i == last ? 0.f : sp)), col(fill));
            if (sel) dl->AddRectFilled(p0, ImVec2(p0.x + 3 * gUi, p0.y + h), u32(T.accent));   // the selection bar
            if (focus) dl->AddRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p0.x + w - 1, p0.y + h - 1), u32(T.accent), 0, 0, 1.f * gUi);   // keyboard focus: an outline, never a fill
            const bool hid = searching && modeHiddenNow(a, m);   // found by the search only: tagged
            const std::string name = hid ? std::string(m.name) + " (" + tr("hidden") + ")" : std::string(m.name);
            const ImVec2 np(p0.x + 28 * gUi, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
            if (sel) {
                const ImU32 nc = textOn(fill, T.bright);
                if (semi) dl->AddText(semi, ImGui::GetFontSize(), np, nc, name.c_str());
                else { dl->AddText(np, nc, name.c_str()); dl->AddText(ImVec2(np.x + 0.6f * gUi, np.y), nc, name.c_str()); }   // semibold by drawing twice
            } else dl->AddText(np, textOn(fill, running || hid ? T.dim : T.text), name.c_str());
            const char* sub = hid ? "" : tr(m.sub);
            const ImVec2 ss = ImGui::CalcTextSize(sub);
            const float nameW = sel && semi ? semi->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, name.c_str()).x : ImGui::CalcTextSize(name.c_str()).x + (sel ? 0.6f * gUi : 0.f);
            if (np.x + nameW + 10 * gUi < p0.x + w - ss.x - 6 * gUi)   // no room: the name alone, not both on top of each other
                dl->AddText(ImVec2(p0.x + w - ss.x - 6 * gUi, p0.y + (h - ss.y) * 0.5f), textOn(fill, sel ? T.text : T.dim), sub);
            if (clicked) selectMode(a, m.family, m.subId);
            if (hov && running && !sel) ImGui::SetTooltip("%s", tr("Click to switch mode and restart receiver automatically"));
        }
    }
    if (!shown) ImGui::TextDisabled(tr("no mode matches \"%s\""), q);
    a.tgMin[TgSwitch] = first; a.tgMax[TgSwitch] = ImVec2(first.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
}

// the bodies of the sidebar sections (sidebar() below; the General screen of 4.0 puts them in its own sidebar, general/general_screen.cpp)
void sideDisplayBody(App& a) {
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
        const int cur = a.layoutUnified ? 1 : 0;
        if (ImGui::BeginCombo("##layoutsel", lay[cur])) {   // each row shows a preview of the layout when hovered (ui_preview.h)
            for (int i = 0; i < 2; i++) {
                if (ImGui::Selectable(lay[i], cur == i)) { a.layoutUnified = i == 1; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::Unified, i);
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Unified: the spectrum and waterfall stay on screen above the decoded views (F6 folds them)"));
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Look")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
    {
        static const char* lk[6] = {N_("Classic"), N_("Dev A"), N_("Dev B"), N_("Studio"), N_("Mono Pro"), N_("Daylight")};
        const char* names[6] = {tr(lk[0]), tr(lk[1]), tr(lk[2]), tr(lk[3]), tr(lk[4]), tr(lk[5])};
        const int cur = a.uiTheme == 3 ? 1 : a.uiTheme == 4 ? 2 : a.uiTheme >= 5 ? a.uiTheme - 2 : 0;
        if (ImGui::BeginCombo("##looksel", names[cur])) {
            for (int i = 0; i < 6; i++) {
                if (ImGui::Selectable(names[i], cur == i)) { a.uiTheme = i == 0 ? 1 : 2 + i; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::Theme, i == 0 ? 1 : 2 + i);
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::SmallButton(tr("Reset spectrum"))) resetSpectrum(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Restore the default spectrum and waterfall levels and the usual half-and-half split (does not change panel layout)"));
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Palette")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wfmap", tr(Waterfall::mapName(a.wf.effMap())))) {   // also in the Waterfall popup on the plot
        for (int i = 0; i < Waterfall::NumMaps; i++)
        {
            if (ImGui::Selectable(tr(Waterfall::mapName(i)), a.wf.effMap() == i)) { a.wf.map = i; a.wf.palette(); savePrefs(a); }
            uiprev::item(a, uiprev::Opt::WfMap, i);
        }
        ImGui::EndCombo();
    }
    {
        const char* ts[] = {N_("Classic"), N_("Simple")};
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Tuner style")); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##tunerstyle", tr(ts[std::max(0, std::min(1, a.tunerStyle))]))) {
            for (int i = 0; i < 2; i++) {
                if (ImGui::Selectable(tr(ts[i]), a.tunerStyle == i)) { a.tunerStyle = i; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::TunerStyle, i);
            }
            ImGui::EndCombo();
        }
    }
}

bool sideDecoderShown(const App& a) { return a.family == 0 || a.family == 3 || captureRowShown(a) || ((modeUi(a.family) && modeUi(a.family)->decoder) || *dect2::bbf::keyForFamily(a.family)); }

void sideToolsBody(App& a) {
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
    tool("EchoLink...", a.echolinkOpen, N_("Talk to EchoLink stations, links, repeaters and conferences over the Internet"));
    tool("Wireshark...", a.wiresharkOpen, N_("Packets of a mode in Wireshark: capture interface, pcapng files and GSMTAP"));
    tool("LAN swarm...", a.swarmOpen, "See and cooperate with other OnAir programs on this network (opt-in)");
    tool("Messages...", a.msgdbOpen, N_("Every decoded message in one searchable database, with filters and export"));
    tool("Protocol lab...", a.protolabOpen, N_("Work out an unknown digital signal: modulation, baud rate, sync word, CRC and fields, with an export"));
    tool("nRF24 / ESP32 radio...", a.esp32Open, N_("An ESP32 board (with an nRF24L01+ module) on USB: receive nRF24 packets, Bluetooth LE and WiFi networks; transmit behind the Arm switch"));
    tool("Signal Lab...", a.signallabOpen, N_("Blind analysis of an unknown digital signal from the spectrum or a recording, stage by stage with confidences, a bit viewer and a Mode Builder export"));
    tool("Mode Builder...", a.modeBuilderOpen, N_("Build a decoder without code: describe the modulation, sync word, frame, FEC, CRC and fields, test it on a recording, add it to the mode list"));
    tool("TX studio...", a.txStudioOpen, N_("Make a transmit signal as a file: spectrum, waterfall, measurements and a decoder check. No radio, nothing is transmitted"));
    tool("Digital video generator...", a.videoGenOpen, N_("Encode a camera, a still picture or a video file with an overlay into an MPEG transport stream: a .ts file or a UDP / RTP stream (port 5000)"));
    tool("Integrations...", a.integrationsOpen, N_("Send decoded events (aircraft, ships, APRS, pagers, sondes ...) to an MQTT broker, Home Assistant, a webhook or a JSON-lines file. Off until you set it up"));
    tool("Conversations...", a.ssbConvOpen, N_("Finds SSB voice on the span and organises it into conversations you can just listen to: auto-listen, record, timeline"));
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
    tool("Search everything heard", a.heardOpen, N_("One box for a call sign, ICAO address, MMSI or frequency, across the history of every mode"));
    tool("Insights", a.insightOpen, N_("What your decodes show: who hears you, antenna pattern, APRS health, talkgroup busy hours, trunk load, automatic pages, overdue broadcasts"));
    tool("My station", a.stationOpen, N_("Your radios and antennas, entered once: modes pick the right ones and the mode list shows what can be received"));
    tool("Reception doctor", a.doctorOpen, N_("Why is nothing decoding? Checks overload, level, offset, bandwidth, interferers, drops and the mode from the live signal, with one-click fixes"));
    tool("RF fingerprint", a.rfFpOpen, N_("Recognise an individual transmitter by its RF fingerprint (carrier offset, key-up transient, pilot, spurs), also when it sends only a carrier. Logs every transmission."));
    tool("Station health", a.stationHealthOpen, N_("Noise floor, ADC level and reference stations per band, with plain advice on gain, filters and antenna placement"));
    tool("Scanner (highly experimental)", a.scanViewOpen, "HIGHLY EXPERIMENTAL: memories, scan list and tones in one view; steps through them and stops on activity");
    ImGui::Separator();
    themeChoice(a, false);
    accentMenu();
    typographyChoice(false, &a);
    dataFolderChoice();
    timeDisplayChoice(false);
    int pct = (int)std::lround(a.uiTextScale * 100);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##sidetxt", &pct, 80, 200, tr("Text size %d%%"))) a.uiTextScale = std::max(0.8f, std::min(2.f, pct / 100.f));
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    if (ImGui::Checkbox(tr("Touch interface"), &a.touchUi)) savePrefs(a);   // big targets, a bottom dock, full-screen pickers (ui/touch.h)
    if (ImGui::IsItemHovered()) wrapTooltip(280 * gUi, tr("For tablets and touch screens: big buttons, an on-screen keypad for the frequency, a full-screen mode picker."));
    uiprev::item(a, uiprev::Opt::Touch, a.touchUi ? 0 : 1);
}

void sideLogBody(App& a) {
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
        sideDisplayBody(a);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (sideDecoderShown(a) && section(N_("Decoder"))) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbDecoder, true);
        ImGui::Unindent(6 * gUi);
    }
    if (section(N_("Tools"), false)) {   // what used to be only in the View menu; folded until it is wanted
        ImGui::Indent(6 * gUi);
        sideToolsBody(a);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (section(N_("Log"))) {
        sideLogBody(a);
    }
    ImGui::PopStyleVar();
}

// height of the status bar: one thin row, the height of a frame (the mute button), so it follows the text size and the compact typography
float statusStripBlockHeight() { return ImGui::GetFrameHeight(); }

// The status bar, one flat row in the style of an IDE's: state (a dot and a word), frequency to the hertz in groups of three, mode, receiver,
// then while running the signal, sample rate, level, load, dropped samples and CPU with a one-line sparkline. Labels are muted in the text font,
// numbers in the monospaced font, a thin rule between segments. On the right: the "too slow" fix while it applies, the last log line, the
// clock and the mute button. On a narrow window the segments drop out from the right, the least important first (statusstrip::fit).
void statusLine(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool run = a.engine.running();
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
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
    const int cores = (int)std::max(1u, std::thread::hardware_concurrency());
    const SampleLoss sl = a.engine.sampleLoss();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float R = o.x + ImGui::GetContentRegionAvail().x;

    const float rowH = statusStripBlockHeight();

    constexpr float footerScale = 1.00f;
    const float fs = ImGui::GetFontSize() * footerScale;
    const float ty = o.y + (rowH - fs) * 0.5f;

    const float pad = 8 * gUi;
    const float divW = 2 * pad + 1;
    const float lamp = 10 * gUi;
    const float sparkW = 28 * gUi;

    ImFont* mono = typo::fonts().mono
        ? typo::fonts().mono : a.mono;

    const auto textW = [&](const std::string& t, ImFont* f) {
        if (!f) f = ImGui::GetFont();
        return f->CalcTextSizeA(
            fs, FLT_MAX, 0.f, t.c_str()
        ).x;
    };

    const auto put = [&](float x, ImFont* f, ImU32 c, const std::string& t) {
        if (!f) f = ImGui::GetFont();
        dl->AddText(
            f, fs, ImVec2(x, ty),
            c, t.c_str()
        );
        return x + textW(t, f);
    };

    // the state word: Locked / Searching / Running while the receiver runs (from the mode's own summary line), else Stopped or Channel scan
    std::string stateTxt = tr(scanning ? "Channel scan" : "Stopped");
    ImVec4 stateCol = scanning ? pal::warnAmber() : pal::grey();
    if (run) {
        std::string l1, l2;
        summaryText(a, l1, l2);
        if (l2.find(tr("locked")) != std::string::npos) { stateTxt = tr("Locked"); stateCol = pal::okGreen(); }
        else if (l2.find(tr("searching")) != std::string::npos || l2.find(tr("weak")) != std::string::npos) { stateTxt = tr("Searching"); stateCol = pal::warnAmber(); }
        else { stateTxt = tr("Running"); stateCol = pal::okGreen(); }
    }
    enum Kind { KState, KFreq, KMode, KRx, KSignal, KRate, KLevel, KLoad, KDrop, KCpu };
    struct Item { Kind k; std::string label, value; ImU32 col; };
    std::vector<Item> items;
    char b[96];
    const int cpuLv = statusstrip::cpuLevel((float)cpuPct);
    const ImVec4 cpuCol = cpuLv == 2 ? pal::badRed() : cpuLv == 1 ? pal::warnAmber() : T.text;
    items.push_back({KState, "", stateTxt, u32(run || scanning ? T.bright : T.dim)});
    items.push_back({KFreq, "", statusstrip::groupedMhz(listenMhz(a)) + " MHz", u32(T.bright)});
    items.push_back({KMode, tr("mode"), cur->name, u32(T.text)});
    items.push_back({KRx, "RX", std::to_string(a.rxId), u32(T.text)});
    if (run) {
        items.push_back({KSignal, "", "", 0});
        snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); items.push_back({KRate, tr("rate"), b, u32(T.text)});
        snprintf(b, sizeof b, "%.1f dBFS", a.spec.stats.rmsDbfs); items.push_back({KLevel, tr("level"), b, u32(T.text)});
        static OverloadHint hint;   // once per start: DSP drops repeating within 5 s
        if (hint.update(sl.cpuEvents, ImGui::GetTime())) a.engine.log(tr("This mode is too slow for this computer: samples are being dropped. Try a lower sample rate or a narrower mode."));
        if (sl.live) { snprintf(b, sizeof b, "%.0f%%", sl.loadPct); items.push_back({KLoad, tr("load"), b, u32(sl.loadPct > 90 ? pal::warnAmber() : T.text)}); }
        if (sl.radioEvents || sl.cpuEvents || sl.rateLow) items.push_back({KDrop, tr("dropped"), lossText(sl), u32(lossColour(sl, T.text))});
    }
    snprintf(b, sizeof b, "%.0f%%", cpuPct);
    items.push_back({KCpu, "cpu", b, u32(cpuCol)});
    // widths and priorities (0 = always shown): state and frequency, then mode, dropped samples, CPU, signal, rate, level / load, receiver
    std::vector<statusstrip::Seg> segs;
    for (const Item& it : items) {
        float w = (it.label.empty() ? 0.f : textW(it.label, nullptr) + 5 * gUi) + (it.value.empty() ? 0.f : textW(it.value, mono));
        int prio = 0;
        switch (it.k) {
            case KState: w += lamp; prio = 0; break;
            case KFreq: prio = 0; break;
            case KMode: prio = 1; break;
            case KDrop: prio = 2; break;
            case KCpu: w += 5 * gUi + sparkW; prio = 3; break;
            case KSignal: w = signalStripWidth(a); prio = 4; break;
            case KRate: prio = 5; break;
            case KLevel: case KLoad: prio = 6; break;
            case KRx: prio = 7; break;
        }
        segs.push_back({w, prio, true});
    }
    const std::string clock = clockText((int64_t)std::time(nullptr)) + " " + dect2::timefmt::headZone();
    const float muteW = ImGui::GetFrameHeight(), clockW = textW(clock, mono);
    const bool showClock = R - o.x > 520 * gUi;
    const float rightW = muteW + pad + (showClock ? clockW + divW : 0.f);
    statusstrip::fit(segs, R - o.x - rightW, divW);
    float x = o.x;
    bool firstSeg = true;
    const auto divider = [&](float at) { dl->AddLine(ImVec2(at + 0.5f, o.y + 4 * gUi), ImVec2(at + 0.5f, o.y + rowH - 4 * gUi), u32(T.edge)); };
    for (size_t i = 0; i < items.size(); i++) {
        if (!segs[i].on) continue;
        const Item& it = items[i];
        if (!firstSeg) { divider(x + pad); x += divW; }
        firstSeg = false;
        const float w = segs[i].w;
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID((int)i);
        ImGui::InvisibleButton("##seg", ImVec2(std::max(1.f, w), rowH));   // an item under the segment, for its tooltip
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered();
        float tx = x;
        if (it.k == KState) {
            dl->AddCircleFilled(ImVec2(tx + 3.5f * gUi, o.y + rowH * 0.5f), 3.5f * gUi, u32(stateCol));
            put(tx + lamp, nullptr, it.col, it.value);
            if (hov) ImGui::SetTooltip("%s", run ? tr("The receiver is running; the word follows the mode's lock") : scanning ? tr("A channel scan has the radio") : tr("The receiver is stopped: press Start (Space)"));
        } else if (it.k == KSignal) {
            signalStripCell(a, tx, o.y, rowH);   // the same four steps in every mode, with its own tooltip
        } else {
            if (!it.label.empty()) tx = put(tx, nullptr, u32(T.dim), it.label) + 5 * gUi;
            tx = put(tx, mono, it.col, it.value);
            if (it.k == KCpu) {
                cpuSparkDraw(dl, ImVec2(tx + 5 * gUi, o.y + rowH * 0.3f), ImVec2(sparkW, rowH * 0.4f), u32(cpuLv ? cpuCol : T.dim), u32(T.edge), cores);
                if (hov) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Process CPU: %.0f%% (100%% = one core). The line is the last 30 s; the thin rule is 85%% of the cores.", cpuPct);
                    if (run && sl.live) ImGui::Text("DSP load: %.0f%% of real time on the sample thread (processing time, not lost samples)", sl.loadPct);
                    if (run && sl.live && sl.deliveredPct >= 0)
                        ImGui::Text(sl.rateLow ? "Samples arriving: %.1f%% of the set rate (below 97%%: the radio or its driver)" : "Samples arriving: %.1f%% of the set rate", sl.deliveredPct);
                    ImGui::TextDisabled("Drawing is slowed when nothing moves and paused while hidden.");
                    ImGui::EndTooltip();
                }
            } else if (it.k == KLoad || it.k == KDrop) lossTooltip(sl);
            else if (it.k == KMode && hov) ImGui::SetTooltip("%s", cur->blurb);
            else if (it.k == KFreq && hov) ImGui::SetTooltip("%s", tr("The listening frequency, to the hertz"));
        }
        x += w;
    }
    // the right side: the "too slow" fix, the last log line (as much as fits), the clock, the mute button at the edge
    const float rightX = R - rightW;
    ImGui::SetCursorScreenPos(ImVec2(x + pad, ty));
    if (cpuReliefStrip(a, rightX)) x = ImGui::GetItemRectMax().x;
    {
        size_t n = 0;
        const std::vector<std::string> lines = a.engine.logSnapshot(n);
        const float room = rightX - x - divW;
        if (!lines.empty() && room > 60 * gUi) {
            divider(x + pad);
            const std::string t = ellipsize(lines.back(), room, fs);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(x + divW, ty), u32(T.dim), t.c_str());
        }
    }
    if (showClock) {
        divider(rightX);
        ImGui::SetCursorScreenPos(ImVec2(rightX + divW - pad, o.y));
        ImGui::InvisibleButton("##clock", ImVec2(clockW + pad, rowH));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The time, as set in Settings > Time display"));
        put(rightX + divW - pad + pad * 0.5f, mono, u32(T.dim), clock);
    }
    // the mute button at the right edge (the same click as the volume control)
    ImGui::SetCursorScreenPos(ImVec2(R - muteW, o.y));
    if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, tr(a.muted ? "Unmute" : "Mute"))) {
        a.muted = !a.muted;
        a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted);
    }
}

const char* kVariantNames[8] = {N_("Sidebar"), N_("Scope"), N_("Tiles"), N_("Faceplate"), N_("Scope: Cinema"), N_("Scope: Meters"), N_("Scope: Dock"), N_("Panel")};

// one Tools menu item (the label, tooltip, shortcut text and checked state it always had)
static void toolMenuItem(App& a, const menugroups::ToolItem& it) {
    using T = menugroups::Tool;
    const char* lab = tr(it.label);
    auto tip = [](const char* t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t); };   // t is already tr()-ed at the call
    switch (it.id) {
        case T::Transcripts: if (ImGui::MenuItem(lab, nullptr, tx::windowOpen())) tx::windowOpen() = !tx::windowOpen();
            tip(tr("What is said on the airband and other voice channels, as text: live captions, search, export. Off until you turn it on.")); break;
        case T::Passes: ImGui::MenuItem(lab, nullptr, &a.passesOpen); break;
        case T::Scheduler: ImGui::MenuItem(lab, nullptr, &a.schedulerOpen);
            tip(tr("Timed jobs and satellite passes: tune the radio, record audio, IQ or the log, then put everything back")); break;
        case T::Recordings: ImGui::MenuItem(lab, nullptr, &a.recmgrOpen);
            tip(tr("Every recording in one list: search, play, trim, convert, tag, delete with undo, and a size budget per folder")); break;
        case T::SatNogs: ImGui::MenuItem(lab, nullptr, &a.satnogsOpen); break;
        case T::Df: ImGui::MenuItem(lab, nullptr, &a.dfOpen); break;
        case T::Rfi: ImGui::MenuItem(lab, nullptr, &a.rfiOpen); break;
        case T::PassiveRadar: ImGui::MenuItem(lab, nullptr, &a.passiveRadarOpen); break;
        case T::Images: ImGui::MenuItem(lab, nullptr, &a.galleryOpen); break;
        case T::Feeds: ImGui::MenuItem(lab, nullptr, &a.feedsOpen); break;
        case T::EchoLink: ImGui::MenuItem(lab, nullptr, &a.echolinkOpen); break;
        case T::Wireshark: ImGui::MenuItem(lab, nullptr, &a.wiresharkOpen); break;
        case T::Swarm: ImGui::MenuItem(lab, nullptr, &a.swarmOpen); break;
        case T::Messages: ImGui::MenuItem(lab, nullptr, &a.msgdbOpen); break;
        case T::ProtoLab: ImGui::MenuItem(lab, nullptr, &a.protolabOpen); break;
        case T::Esp32: ImGui::MenuItem(lab, nullptr, &a.esp32Open); break;
        case T::SignalLab: ImGui::MenuItem(lab, nullptr, &a.signallabOpen);
            tip(tr("Blind analysis of an unknown digital signal: select it on the spectrum (Identify or Shift+drag) or load a recording")); break;
        case T::ModeBuilder: ImGui::MenuItem(lab, nullptr, &a.modeBuilderOpen); break;
        case T::TxStudio: ImGui::MenuItem(lab, nullptr, &a.txStudioOpen);
            tip(tr("Make a transmit signal as a file, with spectrum, waterfall, measurements and a decoder check. No radio, nothing is transmitted.")); break;
        case T::VideoGen: if (ImGui::MenuItem(lab, nullptr, &a.videoGenOpen)) savePrefs(a);
            tip(tr("Encode a camera, a still picture or a video file with an overlay into an MPEG transport stream: a .ts file or a UDP / RTP stream (port 5000)")); break;
        case T::Integrations: ImGui::MenuItem(lab, nullptr, &a.integrationsOpen); break;
        case T::Conversations: ImGui::MenuItem(lab, nullptr, &a.ssbConvOpen); break;
        case T::Watchlists: ImGui::MenuItem(lab, nullptr, &a.watchlistOpen); break;
        case T::Automations: ImGui::MenuItem(lab, nullptr, &a.automationOpen); break;
        case T::Plugins: ImGui::MenuItem(lab, nullptr, &a.pluginsOpen); break;
        case T::PluginStore: if (ImGui::MenuItem(lab)) pluginStoreOpen(a);   // the plugin window on its Store tab (the marketplace)
            tip(tr("Browse, install and update plugins from the signed plugin registry. Nothing is fetched until you ask.")); break;
        case T::Backup: ImGui::MenuItem(lab, nullptr, &a.backupOpen); break;
        case T::Memory: ImGui::MenuItem(lab, inputmap::shortcut(inputmap::Action::MemoryWin).c_str(), &a.memoryOpen); break;
        case T::Datasets: ImGui::MenuItem(lab, nullptr, &a.datasetsOpen); break;
        case T::Packs: ImGui::MenuItem(lab, nullptr, &a.packsOpen); break;
        case T::Frequencies: ImGui::MenuItem(lab, inputmap::shortcut(inputmap::Action::FreqDbWin).c_str(), &a.freqOpen); break;
        case T::FreqTab: if (ImGui::MenuItem(lab, nullptr, a.freqTab)) freqdbTabToggle(a); break;
        case T::Transmit: ImGui::MenuItem(lab, nullptr, &txViewFlag()); break;
        case T::Map: ImGui::MenuItem(lab, nullptr, &a.mapOpen);
            tip(tr("A world map: find a position by coordinates or Maidenhead locator, distance and bearing from your location")); break;
        case T::Sweep: ImGui::MenuItem(lab, nullptr, &a.sweepOpen); break;
        case T::StationHealth: ImGui::MenuItem(lab, nullptr, &a.stationHealthOpen); break;
        case T::Heard: ImGui::MenuItem(lab, nullptr, &a.heardOpen); break;
        case T::Insights: ImGui::MenuItem(lab, nullptr, &a.insightOpen); break;
        case T::MyStation: ImGui::MenuItem(lab, nullptr, &a.stationOpen); break;
        case T::Doctor: ImGui::MenuItem(lab, nullptr, &a.doctorOpen); break;
        case T::RfFp: ImGui::MenuItem(lab, nullptr, &a.rfFpOpen); break;
        case T::WideAll: ImGui::MenuItem(lab, nullptr, &a.wideAllOpen); break;
        case T::Trunking: ImGui::MenuItem(lab, nullptr, &a.trunkPoolOpen); break;
        case T::Scanner: ImGui::MenuItem(lab, nullptr, &a.scanViewOpen);
            tip(tr("HIGHLY EXPERIMENTAL: step through a list of frequencies and modes, or a band, and stop on activity")); break;
        case T::Propagation: ImGui::MenuItem(lab, nullptr, &a.propOpen);
            tip(tr("Where a transmitter can be received: predicted SNR on the map from power, frequency, time and the SNR you need. No terrain.")); break;
        case T::AntDesigner: ImGui::MenuItem(lab, nullptr, &a.antdOpen);
            tip(tr("Design and model wire antennas: templates, impedance and SWR across a band, radiation pattern, optimiser, calculators")); break;
        case T::Rotator: ImGui::MenuItem(lab, nullptr, &a.rotatorOpen); break;
        case T::VoiceKeys: ImGui::MenuItem(lab, nullptr, &a.voiceKeysOpen);
            tip(tr("Your own decryption keys for digital voice (P25, NXDN). Nothing is decrypted without a key you add.")); break;
        case T::Calibrate: if (ImGui::MenuItem(lab)) calibrateFrequencyOpen(a);
            tip(tr("Measure this radio's frequency error (ppm) against an FM station, an ATSC pilot, a time-signal carrier, or by typing what you see")); break;
    }
}

// the View menu: interface variants, text size, light / high contrast, language, accent, the classic interface
static void viewMenu(App& a) {
    const float vx0 = ImGui::GetCursorScreenPos().x, vy0 = ImGui::GetWindowPos().y, vy1 = vy0 + ImGui::GetWindowHeight();
    struct Mark { float x0, y0, y1; ~Mark() { tourMark("view", ImVec2(x0, y0), ImVec2(ImGui::GetCursorScreenPos().x, y1)); } } mark{vx0, vy0, vy1};   // the tour points at the View menu
    gViewMenuCentre = ImVec2(ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(tr("View")).x * 0.5f + 6 * gUi, ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f);
    if (ImGui::BeginMenu(tr("View"))) {
        // 4.0: a short top level of submenus (docs/MENUS-4.0.md). hover previews (app/ui/ui_preview.h) follow the items of Layout, Theme and Text.
        if (ImGui::BeginMenu(tr("Layout"))) {
            ImGui::TextDisabled("%s", tr("Screen arrangement"));
            for (int v = 0; v < 8; v++) {
                if (ImGui::MenuItem(tr(kVariantNames[v]), nullptr, a.uiVariant == v)) { a.uiVariant = v; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::Variant, v);
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Panels"));   // both switches act at once, the receiver keeps running; the same two values as Settings > Display
            if (ImGui::MenuItem(tr("Classic"), inputmap::shortcut(inputmap::Action::LayoutToggle).c_str(), !a.layoutUnified && !canvas::on())) { a.layoutUnified = false; canvas::setOn(false); savePrefs(a); }
            uiprev::item(a, uiprev::Opt::Unified, 0);
            if (ImGui::MenuItem(tr("Unified"), inputmap::shortcut(inputmap::Action::LayoutToggle).c_str(), a.layoutUnified && !canvas::on())) { a.layoutUnified = true; canvas::setOn(false); savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Try the new layout: the spectrum and waterfall stay on screen above the decoded views"));
            uiprev::item(a, uiprev::Opt::Unified, 1);   // after the text tooltip: the picture goes under it
            canvas::layoutMenuItem(a);   // Workspace: blocks on a blank canvas (app/canvas)
            if (ImGui::MenuItem(tr("Fit panels to their content"), nullptr, &a.fitPanels)) savePrefs(a);   // ui/overview_layout.h, docs/LAYOUT-4.0.md
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The list on the right and the lower row take only the room they need: an empty row is hidden, a small plot moves beside the list, a list of a line or two becomes a strip over the spectrum. Off: the fixed sizes."));
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Touch interface"), nullptr, &a.touchUi)) savePrefs(a);   // the same flag as the side panel: big targets, bottom dock (ui/touch.h)
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For tablets and touch screens: big buttons, an on-screen keypad for the frequency, a full-screen mode picker."));
            uiprev::item(a, uiprev::Opt::Touch, a.touchUi ? 0 : 1);
            dockMenu(a);   // Docking (ui2_dock.cpp)
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Classic interface"))) { a.newUi = false; savePrefs(a); }   // keeps this menu bar; Ctrl+Shift+M or View > Reset view > Restore the default view comes back
            uiprev::item(a, uiprev::Opt::NewUi, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Theme"))) {
            // two separate settings, each with exactly one item checked: the colour scheme (a.uiTheme) and the brightness (Dark, Light, High contrast)
            ImGui::TextDisabled("%s", tr("Colour scheme"));
            static const char* names[8] = {N_("Terminal palette"), N_("Instrument palette"), N_("Mono palette"), N_("Dev A (graphite)"), N_("Dev B (terminal green)"), N_("Studio (slate, rounded)"), N_("Mono Pro (OLED, amber)"), N_("Daylight (light, blue)")};
            for (int t = 0; t < 8; t++) {
                if (ImGui::MenuItem(tr(names[t]), nullptr, a.uiTheme == t)) { a.uiTheme = t; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::Theme, t);
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Brightness"));
            themeChoice(a, true);
            ImGui::Separator();
            accentMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Text"))) {
            ImGui::TextDisabled("%s", tr("Text size"));
            int pct = (int)std::lround(a.uiTextScale * 100);
            ImGui::SetNextItemWidth(160 * gUi);
            if (ImGui::SliderInt("##txtsz", &pct, 80, 200, "%d%%")) a.uiTextScale = std::max(0.8f, std::min(2.f, pct / 100.f));
            if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
            for (int p : {80, 100, 125, 150, 200}) {
                char l[16]; snprintf(l, sizeof l, "%d%%", p);
                if (ImGui::MenuItem(l, nullptr, pct == p)) { a.uiTextScale = p / 100.f; savePrefs(a); }
                uiprev::item(a, uiprev::Opt::TextScale, p / 100.0);
            }
            ImGui::TextDisabled("%s", tr("Ctrl + / Ctrl - / Ctrl 0"));
            ImGui::Separator();
            typographyChoice(true, &a);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Time"))) { timeDisplayChoice(true); ImGui::EndMenu(); }
        if (ImGui::BeginMenu(tr("Language"))) { languageMenuItems(a); ImGui::EndMenu(); }
        if (ImGui::MenuItem(tr("Controls (keys and mouse) ..."), inputmap::shortcut(inputmap::Action::ControlsWin).c_str(), &inputmap::windowOpen())) {}
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What the mouse wheel, keys and extra mouse buttons do on the spectrum and waterfall: zoom, tune, pan, step size, shortcuts"));
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Power and system"))) {
            if (ImGui::MenuItem(tr("Performance overlay"), nullptr, &a.perfOverlay)) savePrefs(a);
            if (ImGui::MenuItem(tr("Verbose logging"), nullptr, &a.verboseLog)) savePrefs(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds USB transfer counts, bytes per second, stall detections and restarts and frame times to the log. For bug reports: switch it on, reproduce the problem, then Save bug report (Diagnostics)."));
            ImGui::Separator();
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
            ImGui::Separator();
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Reset view"))) {
            if (ImGui::MenuItem(tr("Restore the default view"), inputmap::shortcut(inputmap::Action::RestoreView).c_str())) restoreDefaultView(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Back to the default interface: the new interface with classic panels, the Sidebar arrangement, no docking, no touch mode"));
            if (ImGui::MenuItem(tr("Reset layout"))) { if (!a.touchUi) layoutReset();   // forgets the saved window positions of every mode
                a.splitSide = a.splitRight = a.splitBottom = a.splitDock = a.splitBot2 = 0; a.panelSizes.clear(); savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forgets the saved window positions of every mode and the sizes of the panels"));
            if (ImGui::MenuItem(tr("Reset spectrum"))) resetSpectrum(a);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Put the panels back to their automatic sizes (drag the bars between panels to resize them)"));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(tr("Full screen"), inputmap::shortcut(inputmap::Action::Fullscreen).c_str(), gWindow && glfwGetWindowMonitor(gWindow))) toggleFullscreen();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", inputmap::tip(tr("Fill the display, hiding the taskbar (F11 or Alt+Enter). The same shortcut leaves it."), inputmap::Action::Fullscreen).c_str());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Tools"))) {
        // grouped: the table is app/ui/menu_groups.h; a search entry filters across all groups (docs/MENUS-4.0.md)
        static char q[64] = "";
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputTextWithHint("##toolsearch", tr("Search the tools"), q, sizeof q);
        ImGui::Separator();
        int n = 0;
        const menugroups::ToolItem* items = menugroups::toolItems(n);
        if (*q) {
            bool any = false;
            for (int i = 0; i < n; i++) if (menugroups::matchesQuery(tr(items[i].label), q)) { toolMenuItem(a, items[i]); any = true; }
            if (!any) ImGui::TextDisabled("%s", tr("No tool matches"));
        } else {
            for (int i = 0; i < n; i++) if (items[i].group == menugroups::kTopLevel) toolMenuItem(a, items[i]);
            ImGui::Separator();
            for (int g = 0; g < menugroups::kNumToolGroups; g++) {
                if (!ImGui::BeginMenu(tr(menugroups::toolGroupTitle(g)))) continue;   // hover the title: the next list opens beside it
                for (int i = 0; i < n; i++) if (items[i].group == g) toolMenuItem(a, items[i]);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenu();
    }
}

// the right end of the menu bar: credit, Credits, Support, the update button and Clear all. One row of chips: the same height, border and
// background, one gap between them, centred in the bar. Placed from the width the row had last frame (it follows the update button's label).
static float gBarRestW = 0;
// Discord icon chip for the menu bar; true when clicked
// the Discord invite: the same height as the text buttons beside it (a small button's height, the text line), the icon fitted inside
static bool discordButton() {
    ImGui::PushID("discord_invite");
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 sz(h * 1.5f, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##dc", sz);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(88, 101, 242, 255), 2.f * gUi);
    icons::draw(Ic::Discord, ImVec2(p.x + sz.x * 0.5f, p.y + sz.y * 0.5f), h * 0.72f, hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 165, 245, 255), dl);
    if (hov) ImGui::SetTooltip("%s", tr("Join the OnAir Discord"));
    ImGui::PopID();
    return clicked;
}
// The items at the right end of the menu bar (credit, Credits, Support, Discord, version, Clear all). 4.0: flat like the status bar: no frames,
// a faint fill only under the pointer, thin rules between items, every item the height of the text line and centred in the bar, so there is
// a gap above and below whatever the text size and typography (the bar's height comes from the frame padding pushed in menuBar()).
static void menuBarRight(App& a) {
    const ImGuiStyle& ist = ImGui::GetStyle();
    const float g = 6.f * gUi, margin = 12.f * gUi, W = ImGui::GetWindowWidth();
    const char* credit = tr("by: Kristijonas Adomaitis, made with help from the community");
    const float cw = ImGui::CalcTextSize(credit).x + 2.f * ist.FramePadding.x;
    const float lineH = ImGui::GetTextLineHeight();
    const float y = std::max(1.f * gUi, std::floor((ImGui::GetWindowHeight() - lineH) * 0.5f));   // centred in the bar itself
    const float minX = ImGui::GetCursorPosX() + 20 * gUi;
    const float xRest = gBarRestW > 0 ? W - margin - gBarRestW : W - margin - 400 * gUi;
    const bool showBy = gBarRestW > 0 && xRest - 2 * g - 1 - cw > minX;   // the 'by' text goes first when the menu bar is narrow
    if (gBarRestW > 0 && xRest < minX) return;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    const ImVec4 dimText(0.62f, 0.68f, 0.75f, 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 rule = IM_COL32(255, 255, 255, 34);
    const auto next = [&]() {   // a thin rule in the middle of the gap, then the next item on the same centred line
        const float rx = ImGui::GetItemRectMax().x + g, top = ImGui::GetWindowPos().y + y;
        dl->AddLine(ImVec2(rx + 0.5f, top + 2 * gUi), ImVec2(rx + 0.5f, top + lineH - 2 * gUi), rule);
        ImGui::SameLine(0, 2 * g + 1); ImGui::SetCursorPosY(y);
    };
    if (showBy) {
        ImGui::SameLine(xRest - 2 * g - 1 - cw); ImGui::SetCursorPosY(y);
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        if (ImGui::SmallButton(credit)) { a.creditsOpen = true; a.creditsTab = 0; }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Thank you to everyone who reported bugs, sent recordings and opened pull requests."));
        next();
    } else { ImGui::SameLine(xRest); ImGui::SetCursorPosY(y); }
    ImGui::PushStyleColor(ImGuiCol_Text, dimText);
    if (ImGui::SmallButton(tr("Credits"))) { a.creditsOpen = true; a.creditsTab = 0; }
    ImGui::PopStyleColor();
    const float x0 = ImGui::GetItemRectMin().x;
    next();
    ImGui::PushStyleColor(ImGuiCol_Text, dimText);
    supportButton();
    ImGui::PopStyleColor();
    next();
    if (discordButton()) dect2::openUrl(appcodec::discordInviteUrl());
    next();
    updateButton(a);
    next();
    clearAllButtonAt(a, false);
    gBarRestW = ImGui::GetItemRectMax().x - x0;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void restoreDefaultView(App& a) {
    a.newUi = true; a.touchUi = false; a.layoutUnified = false; a.uiVariant = 0; a.dockOn = false;
    canvas::setOn(false);   // the Workspace layout off too
    savePrefs(a);
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
            // one submenu per category of the Mode Browser (kGroupNames, table order); a category over 25 modes gets a second level of chunks
            // (menu_groups.h). The keys 1-9 still pick the first nine listed modes in table order, whatever the submenus.
            static std::vector<int> keyOf;
            keyOf.assign(kNumModes, 0);
            for (int g = 0, key = 0; g < kNumGroups; g++)
                for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i])) keyOf[i] = ++key;
            auto modeItem = [&](int i) {
                char k[4]; snprintf(k, sizeof k, "%d", keyOf[i]);
                if (ImGui::MenuItem(kModes[i].name, keyOf[i] <= 9 ? k : nullptr, modeSelected(a, kModes[i]))) selectMode(a, kModes[i].family, kModes[i].subId);   // only 1-9 are keys
            };
            for (int g = 0; g < kNumGroups; g++) {
                if (!modeGroupListed(a, g)) continue;   // Modes shown: a hidden group leaves the menu
                std::vector<int> rows;
                bool hasCurrent = false;
                for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i])) { rows.push_back(i); hasCurrent |= modeSelected(a, kModes[i]); }
                const std::string title = std::string(tr(kGroupNames[g])) + "  (" + std::to_string(rows.size()) + ")" + (hasCurrent ? "  *" : "") + "###modegrp" + std::to_string(g);
                if (!ImGui::BeginMenu(title.c_str())) continue;
                const int n = (int)rows.size(), chunks = menugroups::chunkCount(n);
                if (chunks == 1) { for (int i : rows) modeItem(i); }
                else for (int c = 0; c < chunks; c++) {
                    int cb, ce; menugroups::chunkRange(n, c, cb, ce);
                    bool cur = false;
                    for (int j = cb; j < ce; j++) cur |= modeSelected(a, kModes[rows[j]]);
                    const std::string ct = std::string(kModes[rows[cb]].name) + "  ...  " + kModes[rows[ce - 1]].name + "  (" + std::to_string(ce - cb) + ")" + (cur ? "  *" : "") + "###modegrp" + std::to_string(g) + "_" + std::to_string(c);
                    if (!ImGui::BeginMenu(ct.c_str())) continue;
                    for (int j = cb; j < ce; j++) modeItem(rows[j]);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Browse all modes..."), "M")) modePickerOpen(a);
            if (ImGui::MenuItem(tr("Modes shown..."))) a.modeVisOpen = true;
            ImGui::EndMenu();
        }
        viewMenu(a);
        const float mx0 = ImGui::GetCursorScreenPos().x, my0 = ImGui::GetWindowPos().y, my1 = my0 + ImGui::GetWindowHeight();
        memoryMenu(a);
        const float mx1 = ImGui::GetCursorScreenPos().x;
        tourMark("memory", ImVec2(mx0, my0), ImVec2(mx1, my1));
        mapSendServerMenu(a);
        if (ImGui::MenuItem(tr("Settings"), inputmap::shortcut(inputmap::Action::OpenSettings).c_str())) settingsOpen() = !settingsOpen();   // a window of its own, not a submenu (Ctrl+, by default; the menu shows the user's key)
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
            if (ImGui::MenuItem(tr("Join the OnAir Discord"))) dect2::openUrl(appcodec::discordInviteUrl());
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
    settingsWindow(a);
    badgesui::window(a);   // Server > Badges... and the "Badge earned" toast (app/badges)
    backupWindow(a);
    tx::captionsWindow(a);
    return menuH;
}

// 4.0: the space between the mode's summary and the level meter, which stood empty. The things wanted all the time, as flat segments in the
// style of the status bar (muted label, monospaced value, thin rules), right-aligned against the meter: IQ record with its time and size, the
// memories (quick tune), the offset of the listening frequency from the radio's centre, the mode's bandwidth. A narrow window drops them from
// the right by priority (statusstrip::fit), record last. The same controls stay where they were (Record IQ in the side panel, the Memory menu).
static void quickStrip(App& a, float x0, float x1, float y, float h) {
    if (x1 - x0 < 60 * gUi) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* mono = typo::fonts().mono ? typo::fonts().mono : a.mono;
    const float fs = ImGui::GetFontSize(), pad = 8 * gUi, divW = 2 * pad + 1, fh = ImGui::GetFrameHeight(), ty = y + (h - ImGui::GetTextLineHeight()) * 0.5f, by = y + (h - fh) * 0.5f;
    const auto textW = [&](const std::string& t, ImFont* f) { ImGui::PushFont(f, fs); const float w = ImGui::CalcTextSize(t.c_str()).x; ImGui::PopFont(); return w; };
    char off[48], bwv[48];
    const double offKhz = (a.freqMhz - radioCenterMhz(a)) * 1e3;
    snprintf(off, sizeof off, "%+.3f kHz", std::fabs(offKhz) < 0.0005 ? 0.0 : offKhz);
    const double bwMhz = a.tune.bandwidthMhz;
    if (bwMhz > 0 && bwMhz < 1) snprintf(bwv, sizeof bwv, "%.1f kHz", bwMhz * 1e3); else snprintf(bwv, sizeof bwv, "%.3f MHz", bwMhz);
    const RecordingStats rs = a.engine.recordingStats();
    enum { QRec, QMem, QOff, QBw };
    struct Q { int k; std::string label, value; };
    const Q items[4] = {{QRec, "", ""}, {QMem, "", tr("Memory")}, {QOff, tr("offset"), off}, {QBw, tr("bw"), bwv}};
    std::vector<statusstrip::Seg> segs;
    for (const Q& q : items) {
        float w = 0; int prio = 1;
        if (q.k == QRec) { w = 12 * gUi + ImGui::CalcTextSize(rs.active ? "00:00  000.0 MB" : tr("rec")).x + 8 * gUi; prio = 1; }
        else if (q.k == QMem) { w = ImGui::CalcTextSize(q.value.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x; prio = 2; }
        else { w = textW(q.label, nullptr) + 5 * gUi + textW(q.value, mono); prio = q.k == QOff ? 3 : 4; }
        segs.push_back({w, prio, true});
    }
    const float used = statusstrip::fit(segs, x1 - x0, divW);
    bool any = false;
    for (const statusstrip::Seg& sg : segs) any = any || sg.on;
    if (!any || used > x1 - x0) return;
    float x = x1 - used;
    dl->AddLine(ImVec2(x - pad - 1, y + 4 * gUi), ImVec2(x - pad - 1, y + h - 6 * gUi), IM_COL32(255, 255, 255, 28));   // the same rule as between the top bar's groups
    bool first = true;
    for (int i = 0; i < 4; i++) {
        if (!segs[(size_t)i].on) continue;
        if (!first) { dl->AddLine(ImVec2(x + pad + 0.5f, by + 3 * gUi), ImVec2(x + pad + 0.5f, by + fh - 3 * gUi), u32(T.edge)); x += divW; }
        first = false;
        const Q& q = items[i];
        ImGui::SetCursorScreenPos(ImVec2(x, by));
        if (q.k == QRec) recordCompact(a);
        else if (q.k == QMem) {
            ImGui::SetCursorScreenPos(ImVec2(x, ty));   // a small button: no vertical frame padding, so on the text line
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
            memoryToolbarButton(a);   // the quick list: favourites and the ten most recent, Store current..., the Memory panel
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushFont(nullptr, fs); dl->AddText(ImVec2(x, ty), u32(T.dim), q.label.c_str()); const float lw = ImGui::CalcTextSize(q.label.c_str()).x; ImGui::PopFont();
            ImGui::PushFont(mono, fs); dl->AddText(ImVec2(x + lw + 5 * gUi, ty), u32(T.text), q.value.c_str()); ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            ImGui::PushID(i);
            ImGui::InvisibleButton("##qs", ImVec2(std::max(1.f, segs[(size_t)i].w), fh));
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", q.k == QOff ? tr("How far the listening frequency is from the radio's centre frequency") : tr("The bandwidth of the mode's channel filter (Reset this mode puts the default back)"));
        }
        x += segs[(size_t)i].w;
    }
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
        float digitsEnd = freqDigits(a);
        if (isGeneralFamily(a.family)) {   // 4.0: the General receiver's demodulator and filter as a button by the frequency (a click: the choices)
            ImGui::SetCursorScreenPos(ImVec2(digitsEnd + 12 * gUi, o.y + 8 * gUi));
            digitsEnd = generalDemodChip(a);
        }
        tourMark("freq", ImVec2(o.x - 4 * gUi, o.y), ImVec2(digitsEnd, o.y + 40 * gUi));   // the lessons point at the run button and the digits
        // a narrow window: the level meter and the volume give up width (the meter goes last), then the summary is cut short
        const float sumX = digitsEnd + 34 * gUi;
        const float modesW = ImGui::CalcTextSize(tr("Modes")).x + 2 * ImGui::GetStyle().FramePadding.x + 12 * gUi;   // the Mode Browser button, left of the mode name
        float vw = 230 * gUi, mw = 260 * gUi;
        const float spare = o.x + W - sumX - modesW - summaryWidth(a) - 40 * gUi;
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
        ImGui::SetCursorScreenPos(ImVec2(sumX, o.y + 8 * gUi));
        modePickerButton(a, "##modes_top");
        const float sumMax = (mw > 0 ? meterX : o.x + W - vw) - 16 * gUi - sumX - modesW;
        summary(a, sumX + modesW, o.y + 6 * gUi, 1.f, sumMax);
        quickStrip(a, sumX + modesW + std::min(summaryWidth(a), sumMax) + 24 * gUi, (mw > 0 ? meterX : o.x + W - vw) - 18 * gUi, o.y, 40 * gUi);
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
    ImGui::SameLine(0, 4 * gUi);
    modePickerButton(a);   // the Mode Browser: every mode as a button (mode_picker.cpp)
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
