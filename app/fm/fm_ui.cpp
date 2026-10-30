// FM broadcast radio panels: tuning, status line, station panel with RDS, diagnostic plots and the band scanner.
#include "app.h"
#include "../ui/typography.h"
#include "state/results_export.h"
#include <chrono>
#include <climits>
#include <cstdlib>

bool freqStepArrow(const char* id, ImGuiDir dir);


// the dial steps and rounds to 0.1 MHz; the broadcast band is the slider's span only, a frequency outside it (typed, or stepped to) is kept
static double fmRound(double mhz) { return std::round(std::max(0.0, mhz) * 10.0) / 10.0; }
// the fine tuning step: 0.1 MHz, or 0.2 MHz where the stations sit 200 kHz apart (the setup wizard sets "fmStepKhz" from the place; Americas)
static double fmStepMhz() { return plat::prefs().getI("fmStepKhz", 100) >= 200 ? 0.2 : 0.1; }

// Tune now (or at the next start, when the receiver is stopped).
void fmTune(App& a, double mhz) {
    a.freqMhz = fmRound(mhz);
    a.tune.centerHz = a.freqMhz * 1e6;
    if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    savePrefs(a);
}

static std::string fmTrim(const std::string& s) {
    size_t b = s.find_first_not_of(" -");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" -");
    return s.substr(b, e - b + 1);
}

// The name of a station found by the scan, for lists
static std::string fmStationLabel(const App::FmScan::Res& r) {
    char b[96];
    snprintf(b, sizeof b, "%.1f MHz  %s", r.mhz, r.name.empty() ? "" : r.name.c_str());
    return b;
}

// ------------------------------------------------------------------ tuning in the toolbar
// Step buttons and a list of the stations the scan found. Returns true when the frequency changed.
bool fmFrequencyCombo(App& a) {
    bool changed = false;
    if (freqStepArrow("##fmdn", ImGuiDir_Left)) { a.freqMhz = fmRound(a.freqMhz - fmStepMhz()); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune down one step (%.1f MHz). Hold for a continuous change.", fmStepMhz());
    ImGui::SameLine(0, 2 * gUi);
    if (freqStepArrow("##fmup", ImGuiDir_Right)) { a.freqMhz = fmRound(a.freqMhz + fmStepMhz()); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune up one step (%.1f MHz). Hold for a continuous change.", fmStepMhz());
    ImGui::SameLine(0, 8 * gUi);
    char lbl[64];
    int cur = -1;
    for (size_t i = 0; i < a.fmScan.results.size(); i++) if (a.fmScan.results[i].found && std::fabs(a.fmScan.results[i].mhz - a.freqMhz) < 0.05) cur = (int)i;
    snprintf(lbl, sizeof lbl, cur >= 0 ? "%s" : "Stations", cur >= 0 ? fmStationLabel(a.fmScan.results[(size_t)cur]).c_str() : "");
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##fmsta", lbl)) {
        bool any = false;
        for (size_t i = 0; i < a.fmScan.results.size(); i++) {
            const auto& r = a.fmScan.results[i];
            if (!r.found) continue;
            any = true;
            if (ImGui::Selectable(fmStationLabel(r).c_str(), (int)i == cur)) { a.freqMhz = r.mhz; changed = true; }
        }
        if (!any) ImGui::TextDisabled("run the band scan to list stations");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        double lo = 87.5, hi = 108.0;
        fmBandRange(a, lo, hi);
        ImGui::SetTooltip("Stations found by the band scan (Scan tab).\nType a frequency in the box on the left to tune by hand: %.1f - %.0f MHz.", lo, hi);
    }
    if (changed) a.freqMhz = fmRound(a.freqMhz);
    return changed;
}

// ------------------------------------------------------------------ status line
void fmStatus(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const FmTelemetry& fm = a.rx.fm;
    const bool live = run && a.rx.standard == 6;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !live ? 0 : fm.state == 2 ? 1 : fm.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Stereo", !live ? 0 : fm.stereo ? 1 : fm.carrier ? 2 : 0, (int)Ic::Speaker); flowNext(12 * gUi);
    lamp("RDS", !live ? 0 : fm.rdsSync && fm.rdsBlockOkPct >= 60 ? 1 : fm.rdsSync ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    // Each readout must be one flow group. Reserve space for changing numeric
    // values so the next-frame layout does not repeatedly switch rows.
    auto ro = [&](const char* label, const std::string& val, int minChars = 0,
                  ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0);
        const float valueWidth = ImGui::CalcTextSize(val.c_str()).x;
        ImGui::TextColored(col, "%s", val.c_str());
        if (minChars > 0) {
            const std::string sample((size_t)minChars, '0');
            const float reserved = ImGui::CalcTextSize(sample.c_str()).x;
            if (valueWidth < reserved) {
                ImGui::SameLine(0, 0);
                ImGui::Dummy(ImVec2(reserved - valueWidth, ImGui::GetTextLineHeight()));
            }
        }
        ImGui::PopFont();
        ImGui::EndGroup();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", 10, ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (!live) ro("State", "starting", 10, ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (fm.state == 2) ro("State", "Locked", 10, ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (fm.state == 1) ro("State", "Weak", 10, ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "No station", 10, ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (run) {
        const bool tuned = live && fm.state >= 1;
        ro("Sound", tuned ? (fm.stereo ? "Stereo" : "Mono") : "-", 6);
        const std::string ps = tuned ? fmTrim(fm.psName) : std::string();
        ro("Station", ps.empty() ? "-" : ps, 8);
    }
    flowBreak();
    if (run) {
        const bool tuned = live && fm.state >= 1;
        if (tuned) snprintf(b, sizeof b, "%.1f dB", fm.snrDb); else snprintf(b, sizeof b, "-");
        ro("SNR", b, 9);
        if (tuned) snprintf(b, sizeof b, "%.0f kHz", fm.devKhz); else snprintf(b, sizeof b, "-");
        ro("Deviation", b, 9, tuned && fm.devKhz > 80 ? ImVec4(0.95f, 0.70f, 0.15f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
        if (tuned && fm.stereo) snprintf(b, sizeof b, "%.1f %%", fm.pilotPct); else snprintf(b, sizeof b, "-");
        ro("Pilot", b, 8);
        if (tuned) snprintf(b, sizeof b, "%+.0f Hz", fm.cfoHz); else snprintf(b, sizeof b, "-");
        ro("Offset", b, 11);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b, 12); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level of the whole captured band (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Signal"); ImGui::SameLine(0, 5 * gUi);
    {
        const bool ok = live && fm.carrier;
        const float t = ok ? std::min(1.f, std::max(0.f, fm.snrDb / 45.f)) : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, ok ? "%.0f%%  %s" : "-", t * 100, t > 0.8f ? "excellent" : t > 0.55f ? "good" : t > 0.3f ? "fair" : "poor");
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && live) ImGui::SetTooltip("Audio signal-to-noise ratio %.1f dB (45 dB and up is studio-clean).\nChannel power %.1f dBFS.", fm.snrDb, fm.levelDbfs);
    }
    flowNext(15 * gUi);
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), 27, lossColour(l)); lossTooltip(l); }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (strong FM stations nearby)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

// ------------------------------------------------------------------ history, volume
void fmHistory(App& a) {
    static uint64_t lastSeq = 0;
    static bool wasRunning = false;
    static float lastVol = -1;
    static bool lastMute = false;
    static int lastDe = 0, lastW = -1;
    static bool lastScan = false;
    if (a.fmMode && a.engine.running()) {   // the sound controls are shared with the other modes: push them to the receiver when they change or it starts
        if (!wasRunning || a.volume != lastVol || a.muted != lastMute || a.fmScan.running != lastScan) {
            a.engine.fm().setVolume(a.volume);
            a.engine.fm().setMuted(a.muted || a.fmScan.running);
            lastVol = a.volume; lastMute = a.muted; lastScan = a.fmScan.running;
        }
    }
    {   // the stereo choice (an old saved Mono reads as mode 1), the audio bandwidth and the squelch (issue #213)
        static int lastStereo = -1, lastBw = -1, lastSq = -1;
        const int sm = a.fmStereo ? a.fmStereo : a.fmMono ? 1 : 0;
        const bool push = a.engine.running() && !wasRunning;
        if (a.fmMode && (sm != lastStereo || push)) { a.engine.fm().setStereoMode(sm); lastStereo = sm; }
        if (a.fmMode && (a.fmAudioBw != lastBw || push)) { a.engine.fm().setAudioBandwidth(a.fmAudioBw); lastBw = a.fmAudioBw; }
        if (a.fmMode && (a.fmSquelch != lastSq || push)) { a.engine.fm().setSquelch((float)a.fmSquelch); lastSq = a.fmSquelch; }
    }
    if (a.fmMode && (a.fmDeemph != lastDe || (a.engine.running() && !wasRunning))) { a.engine.fm().setDeemphasis(a.fmDeemph); lastDe = a.fmDeemph; }
    {   // the tone control: pushed when a control changes (the key is unique for the values in range)
        static int lastTone = INT_MIN;
        const int key = ((a.fmBass * 16) + a.fmTreble) * 2 + (a.fmWarm ? 1 : 0);
        if (a.fmMode && (key != lastTone || (a.engine.running() && !wasRunning))) { a.engine.fm().setTone(a.fmBass, a.fmTreble, a.fmWarm); lastTone = key; }
    }
    {   // the RDS / RBDS choice follows the tuned frequency under Auto: pushed when the setting or the tenth of a MHz changes
        static long lastRegion = -1;
        const long key = a.fmRegion * 100000L + std::lround(a.freqMhz * 10);
        if (a.fmMode && (key != lastRegion || (a.engine.running() && !wasRunning))) { a.engine.fm().setRdsRegion(a.fmRegion, a.freqMhz); lastRegion = key; }
    }
    if (a.fmChanKhz != lastW) { a.engine.fm().setChannelWidth(a.fmChanKhz * 1e3); lastW = a.fmChanKhz; }   // the receiver keeps it between starts
    wasRunning = a.fmMode && a.engine.running();
    if (a.rx.standard != 6 || a.rx.seq == lastSeq) return;
    lastSeq = a.rx.seq;
    auto push = [](std::deque<float>& dq, float v) { dq.push_back(v); if (dq.size() > 600) dq.pop_front(); };
    const FmTelemetry& fm = a.rx.fm;
    if (fm.carrier) { push(a.fmSnrH, fm.snrDb); push(a.fmPilotH, fm.pilotPct); push(a.fmRdsH, fm.rdsBlockOkPct); }
}

// ------------------------------------------------------------------ plots under the tabs
void fmPanels(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const bool live = a.engine.running() && a.rx.standard == 6;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.25f));                    // the square plot keeps its own width,
    const float colW = std::max(120.f, (availW - 5 * gap - sqW) / 3.f);                    // the other three share the rest
    const ImVec2 sz(colW, plotH);
    const ImVec2 sq(sqW, sqW);        // the symbol plot stays square
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(colW, "Multiplex spectrum (kHz)");
    if (plt::BeginPlot("##fmmpx", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, 0, 80, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -100, 0, plt::Cond_Always);
        const double marks[3] = {19, 38, 57};   // pilot, stereo difference, RDS
        for (int i = 0; i < 3; i++) {
            const double xs[2] = {marks[i], marks[i]}, ys[2] = {-100, 0};
            plt::Spec sp; sp.LineColor = ImVec4(0.5f, 0.55f, 0.6f, 0.45f); sp.LineWeight = 1.f;
            plt::PlotLine(i == 0 ? "pilot" : i == 1 ? "L-R" : "RDS", xs, ys, 2, sp);
        }
        if (live && !fm.mpxDb.empty()) {
            const int n = (int)fm.mpxDb.size();
            std::vector<double> x((size_t)n), y((size_t)n);
            for (int i = 0; i < n; i++) { x[(size_t)i] = fm.mpxMaxHz / 1000.0 * i / n; y[(size_t)i] = fm.mpxDb[(size_t)i]; }
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("mpx", x.data(), y.data(), n, sp);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(sqW, "RDS symbols (%zu)", fm.rdsConst.size());
    scatter("##fmrds", live ? fm.rdsConst : std::vector<cf32>(), sq, 2.6, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Audio SNR (dB)");
    historyPlot("##fmsnr", "dB", a.fmSnrH, sz);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "RDS blocks ok (%%)");
    historyPlot("##fmrdsh", "%", a.fmRdsH, sz);
    ImGui::EndGroup();
}

// ------------------------------------------------------------------ saved stations (a favourites library, under the RDS decoder)
// A library the user builds by hand. "Save station" takes the frequency tuned now and the live RDS name (kept editable, or left for the
// user to type when the station sends no name), the list can be searched, and clicking an entry tunes to it. Kept in the preferences.
static void fmSavedPanel(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const bool live = a.engine.running() && a.rx.standard == 6 && fm.carrier;
    const std::string rdsName = live ? fmTrim(fm.psName) : "";
    fmSavedValidateEdit(a.fmSavedEdit, (int)a.fmSaved.size());
    FmSavedEdit& S = a.fmSavedEdit;

    sectionHeader(Ic::Star, "Saved stations");
    if (iconButton(Ic::Save, "Save station", pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) {
        FmSavedStation st;
        st.mhz = a.freqMhz;
        st.name = rdsName;   // the RDS name if one is coming in, otherwise empty for the user to fill in
        a.fmSaved.push_back(st);
        S.editing = (int)a.fmSaved.size() - 1;   // open the new entry for naming straight away
        snprintf(S.name, sizeof S.name, "%s", st.name.c_str());
        S.focus = true; S.filter[0] = '\0';      // clear any search so the new entry is visible
        savePrefs(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save %.1f MHz%s to the library.\nYou can rename it; click a saved station to tune to it.", a.freqMhz, !rdsName.empty() ? " with its RDS name" : " (then type a name)");

    if (a.fmSaved.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("No saved stations yet. Tune to a station and press Save station."); ImGui::PopTextWrapPos(); return; }

    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##fmsavedsearch", "Search saved stations", S.filter, sizeof S.filter);

    auto matches = [&](const FmSavedStation& st) -> bool { return fmSavedMatches(st, S.filter); };
    int shown = 0; for (const auto& st : a.fmSaved) if (matches(st)) shown++;
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    const float h = std::min(170.f * gUi, std::max(rowH, shown * rowH + 8));
    ImGui::BeginChild("##fmsaved", ImVec2(0, h), ImGuiChildFlags_Borders);
    auto askDelete = [&](int i) { S.confirmDel = i; S.askConfirm = true; };   // route every delete through the confirmation dialog
    if (!shown) ImGui::TextDisabled("no saved station matches the search");
    else if (ImGui::BeginTable("fmsavedtbl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 52 * gUi);
        ImGui::TableSetupColumn("Station");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        for (size_t i = 0; i < a.fmSaved.size(); i++) {
            FmSavedStation& st = a.fmSaved[i];
            if (!matches(st)) continue;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();   // frequency: click to tune (its own cell, so it never covers the remove button)
            char fl[16]; snprintf(fl, sizeof fl, "%.1f", st.mhz);
            const bool sel = std::fabs(st.mhz - a.freqMhz) < 0.05;
            if (ImGui::Selectable(fl, sel) && S.editing != (int)i && !a.fmScan.running) fmTune(a, st.mhz);
            if (ImGui::IsItemHovered() && S.editing != (int)i) ImGui::SetTooltip("Click to tune to %.1f MHz.", st.mhz);
            ImGui::TableNextColumn();   // name: click to rename, right-click to remove
            if (S.editing == (int)i) {
                ImGui::SetNextItemWidth(-1);
                if (S.focus) { ImGui::SetKeyboardFocusHere(); S.focus = false; }
                if (ImGui::InputText("##edname", S.name, sizeof S.name, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
                    if (fmSavedRename(a.fmSaved, (int)i, S.name)) savePrefs(a);
                    S.editing = -1;
                }
            } else {
                ImGui::AlignTextToFramePadding();
                if (st.name.empty()) ImGui::TextDisabled("(click to name)"); else ImGui::TextUnformatted(st.name.c_str());
                if (ImGui::IsItemClicked()) { S.editing = (int)i; S.focus = true; snprintf(S.name, sizeof S.name, "%s", st.name.c_str()); }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) askDelete((int)i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to rename; right-click to remove.");
            }
            ImGui::TableNextColumn();   // remove
            if (iconFlat(Ic::Cross, "Remove this station")) askDelete((int)i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // confirmation before a station is removed. OpenPopup / BeginPopupModal run outside any PushID so their ids match.
    char popId[40];
    snprintf(popId, sizeof popId, "Remove saved station##rx%d", a.rxId);
    if (S.askConfirm) { ImGui::OpenPopup(popId); S.askConfirm = false; }
    if (ImGui::BeginPopupModal(popId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (S.confirmDel >= 0 && S.confirmDel < (int)a.fmSaved.size()) {
            const FmSavedStation& st = a.fmSaved[(size_t)S.confirmDel];
            ImGui::Text("Remove \"%s\" (%.1f MHz) from your saved stations?", st.name.empty() ? "unnamed station" : st.name.c_str(), st.mhz);
        } else ImGui::TextUnformatted("Remove this saved station?");
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(90 * gUi, 0))) {
            if (fmSavedRemove(a.fmSaved, S.confirmDel)) { S.editing = -1; savePrefs(a); }
            S.confirmDel = -1; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::Button("Cancel", ImVec2(90 * gUi, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { S.confirmDel = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::Spacing();
}

// ------------------------------------------------------------------ station panel (right) and the Radio tab
void fmRadioPanel(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const bool run = a.engine.running();
    const bool live = run && a.rx.standard == 6 && fm.carrier;
    sectionHeader(Ic::Radio, "FM radio");
    if (ImGui::BeginTable("fmtbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn("");
        auto row = [&](Ic ic, const char* k, const char* fmt, auto... v) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();   // wraps in a narrow column
        };
        row(Ic::Antenna, "Frequency", "%.1f MHz", a.freqMhz);
        if (live && fmTrim(fm.psName).empty() && !fm.rds.psPartial.empty()) row(Ic::Radio, "Station", "%s (receiving)", fm.rds.psPartial.c_str());   // the name so far, '_' not yet received
        else row(Ic::Radio, "Station", "%s", live && !fmTrim(fm.psName).empty() ? fmTrim(fm.psName).c_str() : "-");
        if (live) {   // RadioText+: what is playing, as the station tags it
            std::string artist, title;
            for (const auto& t : fm.rds.rtPlus) { if (t.type == 4 || (t.type == 9 && artist.empty())) artist = t.text; if (t.type == 1) title = t.text; }
            if (!title.empty() || !artist.empty()) row(Ic::Speaker, tr("Now playing"), "%s%s%s", artist.c_str(), !artist.empty() && !title.empty() ? " - " : "", title.c_str());
            if (!fm.rds.ert.empty()) row(Ic::Wave, tr("Text (eRT)"), "%s", fm.rds.ert.c_str());
        }
        row(Ic::Layers, "Programme", "%s", live && !fm.ptyText.empty() && fm.ptyText != "None" ? fm.ptyText.c_str() : "-");
        if (live && fm.rbds && !fm.callSign.empty()) row(Ic::Radio, tr("Call letters"), "%s", fm.callSign.c_str());
        {
            char snd[64];
            if (!live) snprintf(snd, sizeof snd, "-");
            else if (fm.stereo && fm.stereoPct < 95) snprintf(snd, sizeof snd, "stereo, blended %.0f %% (weak signal)", fm.stereoPct);
            else snprintf(snd, sizeof snd, "%s", fm.stereo ? "stereo" : "mono");
            row(Ic::Speaker, "Sound", "%s", snd);
            if (ImGui::IsItemHovered() && live && fm.stereo && fm.stereoPct < 95)
                ImGui::SetTooltip("%s", tr("On a weak station the stereo part carries most of the hiss: Auto mixes it down towards mono as the signal gets weaker. Pick Stereo below to keep full stereo."));
        }
        char rds[64] = "-";
        if (live && fm.rdsSync) {
            const uint64_t all = fm.rds.blocksGood + fm.rds.blocksCorrected + fm.rds.blocksBad;
            if (fm.rds.blocksCorrected && all) snprintf(rds, sizeof rds, "%.0f %% blocks ok (%.0f %% repaired)", fm.rdsBlockOkPct, 100.0 * fm.rds.blocksCorrected / all);
            else snprintf(rds, sizeof rds, "%.0f %% blocks ok", fm.rdsBlockOkPct);
        }
        row(Ic::Wave, "RDS", "%s", rds);
        if (live && fm.equaliser) row(Ic::Wave, tr("Multipath"), "%s", tr("echo found, equaliser on"));
        row(Ic::Warning, "Traffic", "%s", !live ? "-" : fm.trafficAlert ? "ANNOUNCEMENT" : fm.trafficProgram ? "station (TP)" : "no");
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Wave, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", tr("RDS standard")); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        {   // the programme type names and call letters differ between RDS and the US RBDS
            static const char* const names[3] = {N_("Auto (RBDS on odd tenths)"), N_("RDS (Europe)"), N_("RBDS (US)")};
            const std::string cur = fitCaption(tr(names[a.fmRegion]), ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().FramePadding.x);
            if (ImGui::BeginCombo("##fmreg", cur.c_str())) {
                for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(names[i]), a.fmRegion == i)) { a.fmRegion = i; savePrefs(a); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("RDS and the US RBDS share the data but name the programme types differently, and RBDS stations send call letters. Auto uses RBDS on the US channel raster (87.9, 88.1 ... 107.9 MHz); Europe has stations on those channels too, so pick RDS (Europe) there."));
        }
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Stereo"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        {
            const char* sn[3] = {"Auto (stereo when the pilot is locked)", "Mono", "Stereo (also on a weak pilot)"};
            const int sm = a.fmStereo ? a.fmStereo : a.fmMono ? 1 : 0;
            if (ImGui::BeginCombo("##fmmono", sn[sm])) {
                for (int i = 0; i < 3; i++) if (ImGui::Selectable(sn[i], sm == i)) { a.fmStereo = i; a.fmMono = i == 1; savePrefs(a); }
                ImGui::EndCombo();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mono ignores the stereo part of the signal. On a weak station the stereo part carries most of the hiss, so mono sounds cleaner.\nStereo decodes the stereo part as soon as a pilot is there, even one too weak for Auto.");
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("De-emphasis"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        // a narrow list: the region in brackets goes first ("50 us"), the box keeps the value
        const std::string de = fitCaption(a.fmDeemph == 75 ? "75 us (Americas)" : a.fmDeemph == 0 ? "Off" : "50 us (Europe, Middle East)", ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().FramePadding.x);
        if (ImGui::BeginCombo("##fmde", de.c_str())) {
            if (ImGui::Selectable("50 us (Europe, Middle East)", a.fmDeemph == 50)) { a.fmDeemph = 50; savePrefs(a); }
            if (ImGui::Selectable("75 us (Americas, South Korea)", a.fmDeemph == 75)) { a.fmDeemph = 75; savePrefs(a); }
            if (ImGui::Selectable("Off (bright, for testing)", a.fmDeemph == 0)) { a.fmDeemph = 0; savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The treble boost the station adds before sending. Wrong setting: dull (75 on a 50 station) or harsh (the other way round) sound.\nOff: no de-emphasis, very bright on a broadcast station.");
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Bandwidth"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        {
            const int bws[4] = {0, 12000, 8000, 5000};
            const char* bwn[4] = {"Full (15 kHz)", "12 kHz", "8 kHz (softer)", "5 kHz (speech, weak stations)"};
            int bi = 0; for (int i = 0; i < 4; i++) if (a.fmAudioBw == bws[i]) bi = i;
            if (ImGui::BeginCombo("##fmbw", bwn[bi])) {
                for (int i = 0; i < 4; i++) if (ImGui::Selectable(bwn[i], bi == i)) { a.fmAudioBw = bws[i]; savePrefs(a); }
                ImGui::EndCombo();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The highest audio frequency kept. Narrower is less sharp and hides hiss on a weak station; full keeps all the treble.");
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Squelch"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##fmsq", &a.fmSquelch, 0, 40, a.fmSquelch == 0 ? "off" : "below %d dB SNR")) savePrefs(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sound is muted while the signal to noise ratio is below this: no loud hiss between stations or when a station fades.");
        // the tone control, off at 0 dB: bass and treble shelves, and Warm (a gentle cut above 6 kHz for a sharp-sounding station)
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Bass"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##fmbass", &a.fmBass, -6, 6, "%+d dB")) savePrefs(a);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Treble"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##fmtreble", &a.fmTreble, -6, 6, "%+d dB")) savePrefs(a);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Warm"); ImGui::TableNextColumn();
        if (ImGui::Checkbox("##fmwarm", &a.fmWarm)) savePrefs(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warm: a gentle cut above 6 kHz (3 dB) for a station that sounds sharp or hissy. Bass and treble are off at 0 dB; with all three off the sound is unchanged.");
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, a.muted ? "Unmute" : "Mute")) { a.muted = !a.muted; }
        ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        float vol = a.volume * 100.f;
        if (ImGui::SliderFloat("##fmvol", &vol, 0, 100, "%.0f %%")) a.volume = vol / 100.f;
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (live && fm.rdsSync && (!fm.rds.afMhz.empty() || !fm.rds.ptyn.empty() || fm.rds.clockUtc || fm.rds.ecc >= 0 || !fm.rds.eon.empty() || !fm.rds.oda.empty())) {
        // the rest of RDS: what the station sends beyond the name and the text
        if (typo::collapsingHeader(trw("RDS details"), 0)) {
            ImGui::PushTextWrapPos(0);
            if (!fm.rds.ptyn.empty()) ImGui::Text("%s: %s", tr("Programme type name"), fmTrim(fm.rds.ptyn).c_str());
            if (fm.rds.clockUtc) {
                const int64_t loc = fm.rds.clockUtc + (int64_t)fm.rds.clockOffsetMin * 60;
                const int64_t day = loc / 86400, sec = loc % 86400;
                // civil date from days since 1970 (Howard Hinnant's algorithm)
                const int64_t z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
                const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const int64_t mp = (5 * doy + 2) / 153, dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9, yy = yoe + era * 400 + (mm <= 2);
                ImGui::Text("%s: %04d-%02d-%02d %02d:%02d (UTC%+d:%02d)", tr("Station clock"), (int)yy, (int)mm, (int)dd, (int)(sec / 3600), (int)(sec / 60 % 60),
                            fm.rds.clockOffsetMin / 60, std::abs(fm.rds.clockOffsetMin % 60));
            }
            if (!fm.rds.afMhz.empty()) {
                std::string af;
                for (double f : fm.rds.afMhz) { char b[16]; snprintf(b, sizeof b, "%s%.1f", af.empty() ? "" : ", ", f); af += b; }
                ImGui::Text("%s: %s MHz", tr("Also on"), af.c_str());
            }
            if (fm.rds.ecc >= 0) ImGui::Text("%s: %02X  PI %04X", tr("Country code (ECC)"), fm.rds.ecc, fm.piCode);
            if (fm.rds.pinDay) ImGui::Text("%s: day %d, %02d:%02d", tr("Programme item"), fm.rds.pinDay, fm.rds.pinHour, fm.rds.pinMinute);
            for (const auto& e : fm.rds.eon) ImGui::Text("%s: %04X %s", tr("Other network"), e.pi, e.ps.empty() ? "..." : fmTrim(e.ps).c_str());
            for (const auto& t : fm.rds.rtPlus) if (t.type != 1 && t.type != 4) ImGui::Text("%s: %s", t.name.c_str(), t.text.c_str());
            if (!fm.rds.oda.empty()) {
                std::string o;
                for (const auto& p : fm.rds.oda) {
                    const char* n = p.first == 0x4BD7 ? "RT+" : p.first == 0x6552 ? "eRT" : (p.first == 0xCD46 || p.first == 0xCD47) ? "TMC" : nullptr;
                    char b[48]; snprintf(b, sizeof b, "%s%s %d%c", o.empty() ? "" : ", ", n ? n : "", p.second / 2, p.second & 1 ? 'B' : 'A');
                    if (!n) snprintf(b, sizeof b, "%s%04X %d%c", o.empty() ? "" : ", ", p.first, p.second / 2, p.second & 1 ? 'B' : 'A');
                    o += b;
                }
                ImGui::TextDisabled("%s: %s", tr("Data applications"), o.c_str());
            }
            ImGui::TextDisabled("%s: %llu good, %llu repaired, %llu lost", tr("Blocks"), (unsigned long long)fm.rds.blocksGood,
                                (unsigned long long)fm.rds.blocksCorrected, (unsigned long long)fm.rds.blocksBad);
            ImGui::PopTextWrapPos();
        }
    }
    if (live && fm.tmc.seen) {   // RDS-TMC traffic messages, only for a station that carries TMC
        char hd[96];
        snprintf(hd, sizeof hd, "Traffic (TMC) - %zu", fm.tmcMessages.size());
        if (typo::collapsingHeader(hd, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Table %d%s%s. Locations are shown as codes: the country's location table is not included.", fm.tmc.ltn, fm.tmc.mode == 1 ? ", enhanced mode" : "", "");
            ImGui::PopTextWrapPos();
            ImGui::BeginChild("##fmtmc", ImVec2(0, 150 * gUi), ImGuiChildFlags_Borders);
            if (fm.tmcMessages.empty()) ImGui::TextDisabled("No messages yet.");
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < fm.tmcMessages.size(); i++) {
                const TmcMessage& m = fm.tmcMessages[i];
                ImGui::PushID((int)i);
                ImGui::PushTextWrapPos(0);
                ImGui::Text("%s", m.text.c_str());
                for (const auto& s : m.more) ImGui::TextDisabled("  + %s", s.c_str());
                char loc[160];
                int n = snprintf(loc, sizeof loc, "location %d (table %d), %s", m.location, fm.tmc.ltn, m.negative ? "negative direction" : "positive direction");
                if (m.extent) n += snprintf(loc + n, sizeof loc - (size_t)n, ", +%d locations", m.extent);
                if (m.diversion) n += snprintf(loc + n, sizeof loc - (size_t)n, ", diversion advised");
                if (!m.durationText.empty()) n += snprintf(loc + n, sizeof loc - (size_t)n, ", %s", m.durationText.c_str());
                ImGui::TextDisabled("%s", loc);
                const int ago = (int)std::max(0.0, (now - m.received) / 60), left = (int)std::max(0.0, (m.expires - now) / 60);
                ImGui::TextDisabled("received %d min ago, expires in %d min", ago, left);
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::Spacing();
    }
    fmSavedPanel(a);   // the saved-stations library, under the RDS decoder
    if (pal::dev()) {   // compact now-playing, then the scanned stations fill the rest of the panel
        ImGui::BeginChild("fmnow", ImVec2(0, 74 * gUi), ImGuiChildFlags_Borders);
        const std::string ps = live ? fmTrim(fm.psName) : "";
        char big[48];
        snprintf(big, sizeof big, "%.1f MHz", a.freqMhz);
        ImGui::PushFont(a.ui, 20.f);
        ImGui::TextUnformatted(big);
        ImGui::PopFont();
        ImGui::SameLine(0, 10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(!run ? "stopped" : !live ? "no station" : ps.empty() ? "reading name..." : ps.c_str());
        if (live && !fm.radioText.empty()) { ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX()); ImGui::TextDisabled("%s", fm.radioText.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
        ImGui::Spacing();
        sectionHeader(Ic::Radio, "Stations");
        ImGui::BeginChild("##fmstations", ImVec2(0, 0));
        size_t found = 0;
        for (const auto& r : a.fmScan.results) if (r.found) found++;
        if (!found) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Run the band scan (Scan tab) to list stations."); ImGui::PopTextWrapPos(); }
        else if (ImGui::BeginTable("fmst", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 52 * gUi); ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
            for (size_t i = 0; i < a.fmScan.results.size(); i++) {
                const auto& r = a.fmScan.results[i];
                if (!r.found) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                char fl[16]; snprintf(fl, sizeof fl, "%.1f", r.mhz);
                if (ImGui::Selectable(fl, std::fabs(r.mhz - a.freqMhz) < 0.05, ImGuiSelectableFlags_SpanAllColumns) && !a.fmScan.running) fmTune(a, r.mhz);
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.empty() ? "-" : r.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f dB", r.snr);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        return;
    }
    {
        const float vh = std::max(60.f, ImGui::GetContentRegionAvail().y - 30.f);
        ImGui::BeginChild("fmnow", ImVec2(0, vh), ImGuiChildFlags_Borders);
        const std::string ps = live ? fmTrim(fm.psName) : "";
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        if (!pal::dev()) icons::draw(Ic::Radio, ImVec2(p.x + cw * 0.5f, p.y + 28), 36.f, live ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(70, 80, 92, 255));
        ImGui::Dummy(ImVec2(1 * gUi, (pal::dev() ? 6 : 52) * gUi));
        char big[48];
        snprintf(big, sizeof big, "%.1f MHz", a.freqMhz);
        ImGui::PushFont(a.ui, 26.f);
        const ImVec2 ts = ImGui::CalcTextSize(big);
        ImGui::SetCursorPosX(std::max(0.f, (cw - ts.x) * 0.5f));
        ImGui::TextUnformatted(big);
        ImGui::PopFont();
        const std::string name = !run ? "stopped" : !live ? "no station" : ps.empty() ? "reading name..." : ps;
        const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
        ImGui::SetCursorPosX(std::max(0.f, (cw - ns.x) * 0.5f));
        ImGui::TextUnformatted(name.c_str());
        if (live && !fm.radioText.empty()) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(cw - 8); ImGui::TextDisabled("%s", fm.radioText.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
    }
}

void fmRadioTab(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const bool live = a.engine.running() && a.rx.standard == 6 && fm.carrier;
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.ui, 26.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%.1f MHz", a.freqMhz);
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * gUi);
    const std::string ps = live ? fmTrim(fm.psName) : "";
    ImGui::TextUnformatted(ps.c_str());
    if (live && !fm.ptyText.empty() && fm.ptyText != "None") { ImGui::SameLine(0, 10 * gUi); ImGui::TextDisabled("%s", fm.ptyText.c_str()); }
    if (live && !fm.radioText.empty()) ImGui::TextDisabled("%s", fm.radioText.c_str());
    ImGui::Spacing();
    {   // tuning dial and steps
        float f = (float)a.freqMhz;
        ImGui::SetNextItemWidth(std::max(200.f, ImGui::GetContentRegionAvail().x - 330 * gUi));
        double lo = 87.5, hi = 108.0;
        fmBandRange(a, lo, hi);   // 87 - 108 MHz, or from 76 MHz where the band plan has extended FM (Brazil) or Japan's band
        if (ImGui::SliderFloat("##fmdial", &f, (float)std::min(lo, 87.0), (float)hi, "%.1f MHz")) a.freqMhz = fmRound(f);
        if (ImGui::IsItemDeactivatedAfterEdit()) fmTune(a, a.freqMhz);
        const double fs = fmStepMhz();
        char dn[8], up[8];
        snprintf(dn, sizeof dn, "-%.1f", fs); snprintf(up, sizeof up, "+%.1f", fs);
        const struct { const char* l; double d; } steps[] = {{"-1", -1}, {dn, -fs}, {up, fs}, {"+1", 1}};
        for (auto& s : steps) { ImGui::SameLine(0, 6 * gUi); if (ImGui::Button(s.l)) fmTune(a, a.freqMhz + s.d); }
    }
    ImGui::Spacing();
    sectionHeader(Ic::Radio, "Stations");
    size_t found = 0;
    for (const auto& r : a.fmScan.results) if (r.found) found++;
    if (!found) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("No stations listed yet. Start the receiver and run the band scan in the Scan tab."); ImGui::PopTextWrapPos(); } return; }
    if (ImGui::BeginTable("fmsta", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34 * gUi); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < a.fmScan.results.size(); i++) {
            const auto& r = a.fmScan.results[i];
            if (!r.found) continue;
            const bool sel = std::fabs(r.mhz - a.freqMhz) < 0.05;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26);
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (iconFlat(Ic::Play, "Listen to this station", sel) && !a.fmScan.running) fmTune(a, r.mhz);
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::Text("%.1f", r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.empty() ? "-" : r.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.pty.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.snr);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.stereo ? "stereo" : "mono");
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ band scanner
// Phase 1 reads the live spectrum in 2 MHz steps and picks the 100 kHz channels that stand out; phase 2 tunes to each of them,
// checks that a real FM carrier is there and waits for its RDS name.
// The text of a scrolling name from the 8-character windows the station sent, in order: a window that is the previous one moved on by
// 1 - 7 characters adds only its new characters ("NOSTALGI", "OSTALGIA" -> "NOSTALGIA"), any other window is a new word. cyclic: the
// windows are one round of the name, so where the last one runs on into the first, the start is not repeated
static size_t fmScrollShift(const std::string& p, const std::string& w) {   // how far w is p moved on, 0 = not a move
    if (p.size() != 8 || w.size() != 8) return 0;
    for (size_t k = 1; k < 8; k++) if (p.compare(k, 8 - k, w, 0, 8 - k) == 0) return k;
    return 0;
}
static std::string fmJoinScroll(const std::vector<std::string>& seq, bool cyclic) {
    std::string text;
    for (size_t i = 0; i < seq.size(); i++) {
        const size_t shift = i ? fmScrollShift(seq[i - 1], seq[i]) : 0;
        text += shift ? seq[i].substr(8 - shift) : (i ? " " : "") + seq[i];
    }
    if (cyclic && seq.size() >= 2) {
        if (const size_t k = fmScrollShift(seq.back(), seq.front())) text.resize(text.size() - (8 - k));
        else text += ' ';
        // one round has no start of its own: begin after the longest gap, which stations leave between two rounds of the text
        const size_t n = text.size();
        size_t best = 0, bestLen = 0;
        for (size_t i = 0; i < n; i++) {
            if (text[i] != ' ' || text[(i + n - 1) % n] == ' ') continue;   // the first space of a run
            size_t len = 0;
            while (len < n && text[(i + len) % n] == ' ') len++;
            if (len > bestLen) { bestLen = len; best = (i + len) % n; }
        }
        if (bestLen && bestLen < n) std::rotate(text.begin(), text.begin() + (long)best, text.end());
    }
    std::string out;   // spaces collapsed and trimmed
    for (char c : text) if (c != ' ' || (!out.empty() && out.back() != ' ')) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// A name that came round: the newest window was also sent p windows before (p >= 2) with no repeat in between. Returns those p windows
// in the order they came, starting with the one seen first, or none
static std::vector<std::string> fmScrollCycle(const std::vector<std::string>& seq) {
    const size_t n = seq.size();
    if (n < 3) return {};
    for (size_t p = 2; p < n; p++) {
        const size_t i = n - 1 - p;
        if (seq[i] != seq.back()) continue;
        std::vector<std::string> c(seq.begin() + (long)i, seq.end() - 1);
        for (size_t k = 0; k < c.size(); k++) if (c[k] == seq.front()) { std::rotate(c.begin(), c.begin() + (long)k, c.end()); break; }
        return c;
    }
    return {};
}

// full RDS: the scan stays on each station until its name, radio text and PI are complete (up to 12 s), instead of leaving at the first name
static bool fmScanFullRds() { return plat::prefs().getB("fmScanFullRds", false); }

static void fmScanPrefsRange(double& lo, double& hi) {
    lo = plat::prefs().getD("fmScanLoMhz", 87.0);
    hi = plat::prefs().getD("fmScanHiMhz", 108.0);
    lo = std::max(76.0, std::min(lo, 107.5));
    hi = std::max(lo + 0.5, std::min(hi, 108.0));
}

static int fmSurveyHopCount(double lo, double hi) {
    const double c0 = lo + 1.0, cLast = hi - 1.0;
    if (cLast <= c0) return 1;
    return (int)std::floor((cLast - c0) / 2.0) + 1;
}

static double fmSurveyCentre(double lo, int hop) { return lo + 1.0 + 2.0 * hop; }

static void fmScanBeginRange(App::FmScan& s) {
    fmScanPrefsRange(s.scanLoMhz, s.scanHiMhz);
    s.surveyHops = fmSurveyHopCount(s.scanLoMhz, s.scanHiMhz);
}

// power of one 100 kHz channel (+-50 kHz) in a spectrum frame, dB; the radio's DC spike is skipped
static double fmChannelDb(const SpectrumFrame& f, double fs, double offHz) {
    const size_t n = f.dbfs.size();
    const double binHz = fs / (double)n;
    double sum = 0;
    int cnt = 0;
    for (double o = offHz - 50e3; o < offHz + 50e3; o += binHz) {
        double fo = o;
        if (std::fabs(fo) < 20e3) fo = fo < 0 ? -20e3 : 20e3;
        const long k = (long)std::lround((double)n / 2 + fo / binHz);
        if (k < 0 || k >= (long)n) continue;
        sum += std::pow(10.0, f.dbfs[(size_t)k] / 10.0);
        cnt++;
    }
    return cnt ? 10 * std::log10(sum / cnt + 1e-20) : -200;
}

void fmScanTab(App& a) {
    App::FmScan& s = a.fmScan;
    const bool run = a.engine.running();
    double lo = 87.0, hi = 108.0;
    fmScanPrefsRange(lo, hi);
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Surveys an FM range for carriers, then listens to each one and reads its name. About two minutes for 87 - 108 MHz."); ImGui::PopTextWrapPos(); }
    {
        ImGui::BeginDisabled(s.running);
        float flo = (float)lo, fhi = (float)hi;
        if (ImGui::InputFloat("from MHz", &flo, 0.1f, 1.0f, "%.1f")) { lo = flo; plat::prefs().setD("fmScanLoMhz", lo); plat::prefs().flush(); }
        ImGui::SameLine();
        if (ImGui::InputFloat("to MHz", &fhi, 0.1f, 1.0f, "%.1f")) { hi = fhi; plat::prefs().setD("fmScanHiMhz", hi); plat::prefs().flush(); }
        fmScanPrefsRange(lo, hi);
        ImGui::EndDisabled();
    }
    {
        bool full = fmScanFullRds();
        ImGui::BeginDisabled(s.running);
        if (ImGui::Checkbox("full RDS", &full)) { plat::prefs().setB("fmScanFullRds", full); plat::prefs().flush(); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stay on each station until its whole name (also a scrolling one), the radio text and the PI code are in,\nup to 12 s per station instead of about 2 s (30 s while a scrolling name has not come round). A scan of a busy band then takes several minutes.");
    }
    ImGui::Spacing();
    if (!s.running) {
        ImGui::BeginDisabled(!run);
        if (iconButton(Ic::Scan, "Scan FM band", pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) {
            fmScanBeginRange(s);
            s.running = true; s.phase = 0; s.idx = -1; s.t0 = 0; s.results.clear(); s.cand.clear(); s.savedFreq = a.freqMhz;
        }
        ImGui::EndDisabled();
        if (!run) { ImGui::SameLine(); ImGui::TextDisabled("start the receiver first"); }
    } else {
        if (iconButton(Ic::Stop, "Stop scan", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255))) { s.running = false; fmTune(a, s.savedFreq); }
        ImGui::SameLine(0, 12 * gUi);
        float frac;
        if (s.phase == 0) {
            { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("surveying the band, step %d of %d", std::max(0, s.idx) + 1, s.surveyHops); ImGui::PopTextWrapPos(); }
            frac = 0.2f * (float)std::max(0, s.idx) / std::max(1, s.surveyHops);
        } else {
            { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("checking %.1f MHz (%d of %d)", s.idx >= 0 && s.idx < (int)s.cand.size() ? s.cand[(size_t)s.idx] : 0.0, s.idx + 1, (int)s.cand.size()); ImGui::PopTextWrapPos(); }
            frac = 0.2f + 0.8f * (float)std::max(0, s.idx) / std::max<size_t>(1, s.cand.size());
        }
        ImGui::ProgressBar(frac, ImVec2(220, ImGui::GetFrameHeight() - 6));
    }
    ImGui::Spacing();
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "station", "type", "snr_db", "sound", "rds", "pi", "radio_text", "names"};
        const std::string now = resexp::stamp();
        for (const auto& r : s.results) if (r.found) {
            char pi[8] = ""; if (r.pi) snprintf(pi, sizeof pi, "%04X", r.pi);
            t.add({now, resexp::num(r.mhz * 1e6, 0), "FM", r.name, r.pty, resexp::num(r.snr, 0), r.stereo ? "stereo" : "mono", r.rds ? "yes" : "no", pi, r.rt, r.psText});
        }
        resexp::saveButton(t, "fm", note, "svfm");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (ImGui::BeginTable("fmscan", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80 * gUi); ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70 * gUi); ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("RDS", ImGuiTableColumnFlags_WidthFixed, 50 * gUi); ImGui::TableSetupColumn("PI", ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
        ImGui::TableSetupColumn("Radio text");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.results.size(); i++) {
            const auto& r = s.results[i];
            if (!r.found) continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26);
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            char fl[24]; snprintf(fl, sizeof fl, "%.1f", r.mhz);
            if (ImGui::Selectable(fl, false, ImGuiSelectableFlags_SpanAllColumns) && !s.running) fmTune(a, r.mhz);
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.empty() ? "-" : r.name.c_str());
            if (!r.psText.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.psText.c_str());   // a scrolling name, all its parts
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.pty.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.snr);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.stereo ? "stereo" : "mono");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.rds ? "yes" : "-");
            ImGui::TableNextColumn(); if (r.pi) ImGui::TextDisabled("%04X", r.pi);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.rt.c_str());
        }
        ImGui::EndTable();
    }
    if (!s.running && s.results.empty()) ImGui::TextDisabled("no scan yet");
}

void fmScanStep(App& a) {
    App::FmScan& s = a.fmScan;
    if (!s.running) return;
    if (!a.engine.running()) { s.running = false; return; }
    const double now = ImGui::GetTime();
    auto tuneTo = [&](double mhz) {
        a.freqMhz = mhz;
        a.tune.centerHz = mhz * 1e6;
        a.engine.retuneReset(a.tune);
        a.peak.clear();
        s.t0 = now;
        s.seq0 = a.lastSeq;
        s.rxSeq0 = a.rx.seq;
        s.psSeq.clear(); s.psT = now;
    };
    auto finish = [&]() {
        s.running = false;
        a.freqMhz = s.savedFreq; a.tune.centerHz = a.freqMhz * 1e6; a.engine.retuneReset(a.tune);
        size_t n = 0; for (const auto& r : s.results) if (r.found) n++;
        a.engine.log("FM scan: " + std::to_string(n) + " stations");
        savePrefs(a);
    };

    if (s.phase == 0) {
        if (s.idx < 0) { fmScanBeginRange(s); s.idx = 0; tuneTo(fmSurveyCentre(s.scanLoMhz, 0)); return; }
        if (now - s.t0 < 0.7 || a.lastSeq == s.seq0) return;
        const double centre = fmSurveyCentre(s.scanLoMhz, s.idx), fs = a.engine.sampleRate();
        double db[20];
        for (int j = 0; j < 20; j++) db[j] = fmChannelDb(a.spec, fs, (-1.0 + 0.1 * j) * 1e6);
        double sorted[20]; std::copy(db, db + 20, sorted); std::sort(sorted, sorted + 20);
        const double floorDb = sorted[4];
        for (int j = 0; j < 20; j++) {
            const double mhz = std::round((centre - 1.0 + 0.1 * j) * 10) / 10;
            if (mhz < s.scanLoMhz - 0.01 || mhz > s.scanHiMhz + 0.01) continue;
            const bool peak = (j == 0 || db[j] >= db[j - 1]) && (j == 19 || db[j] >= db[j + 1]);
            if (db[j] - floorDb > 9 && peak) s.cand.push_back(mhz);
        }
        if (++s.idx >= s.surveyHops) {
            std::sort(s.cand.begin(), s.cand.end());
            s.phase = 1; s.idx = 0;
            if (s.cand.empty()) { finish(); return; }
            tuneTo(s.cand[0]);
        } else tuneTo(fmSurveyCentre(s.scanLoMhz, s.idx));
        return;
    }

    const double dwell = now - s.t0;
    const FmTelemetry& fm = a.rx.fm;
    const bool fresh = a.rx.standard == 6 && a.rx.seq > s.rxSeq0 && dwell > 0.9;
    if (!fresh && dwell < 3.0) return;   // no fresh telemetry yet
    const bool station = fresh && fm.carrier && fm.state >= 1;
    const bool nameKnown = station && !fmTrim(fm.psName).empty();
    bool done = !station ? dwell > 1.4 : (nameKnown && dwell > 1.6) || dwell > 5.0;
    if (station && fmScanFullRds()) {
        // full RDS: stay while the station sends RDS, until the name has come round (a scrolling name: back to the first one; a fixed
        // name: unchanged for 4 s), the radio text is whole (or none came in 6 s) and the PI is known; never longer than 12 s, or 30 s
        // while a scrolling name has not come round yet (a name scrolled a letter at a time takes one window per letter)
        std::string ps = fm.psName;   // the whole 8-character window: the spaces tell how far a scrolling name moved
        ps.resize(8, ' ');
        if (!fmTrim(ps).empty() && (s.psSeq.empty() || ps != s.psSeq.back()) && fmScrollCycle(s.psSeq).empty() && s.psSeq.size() < 64) {
            s.psSeq.push_back(ps);   // until a window comes again: then the cycle is complete
            s.psT = now;
        }
        const bool cycled = !fmScrollCycle(s.psSeq).empty();
        const bool scrolling = s.psSeq.size() >= 2 && !cycled;
        const bool nameDone = cycled || (s.psSeq.size() == 1 && now - s.psT > 4.0);
        const bool rtDone = fm.radioTextComplete || (fm.radioText.empty() && dwell > 6.0);
        const bool rdsDone = nameDone && rtDone && fm.piCode != 0;
        done = (!fm.rdsSync && dwell > 2.5) || (fm.rdsSync && rdsDone) || dwell > (scrolling && fm.rdsSync ? 30.0 : 12.0);
    }
    if (!done) return;
    App::FmScan::Res r;
    r.mhz = s.cand[(size_t)s.idx];
    r.found = station;
    if (station) {
        r.snr = fm.snrDb; r.stereo = fm.stereo; r.rds = fm.rdsSync; r.name = fmTrim(fm.psName); r.pty = fm.ptyText == "None" ? "" : fm.ptyText;
        r.pi = fm.piCode; r.tp = fm.trafficProgram; r.rt = fmTrim(fm.radioText);
        if (s.psSeq.size() >= 2) {   // a scrolling name: its whole text once is the station's name; the windows as sent in psText
            std::vector<std::string> seq = fmScrollCycle(s.psSeq);
            const bool round = !seq.empty();
            if (!round) seq = s.psSeq;   // did not come round in time
            r.name = fmJoinScroll(seq, round);
            for (const auto& w : seq) r.psText += (r.psText.empty() ? "'" : " '") + w + "'";
            a.engine.log("FM scan: " + fmStationLabel(r) + " scrolling name, windows " + r.psText);
        }
        a.engine.log("FM scan: " + fmStationLabel(r) + " SNR " + std::to_string((int)r.snr) + " dB" + (r.stereo ? " stereo" : ""));
    }
    s.results.push_back(r);
    if (++s.idx >= (int)s.cand.size()) { finish(); return; }
    tuneTo(s.cand[(size_t)s.idx]);
}
