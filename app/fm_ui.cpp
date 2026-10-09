// FM broadcast radio panels: tuning, status line, station panel with RDS, diagnostic plots and the band scanner.
#include "app.h"

static constexpr double kFmLow = 87.5, kFmHigh = 108.0;

static double fmRound(double mhz) { return std::round(std::min(kFmHigh, std::max(kFmLow, mhz)) * 10.0) / 10.0; }

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
    if (ImGui::ArrowButton("##fmdn", ImGuiDir_Left)) { a.freqMhz = fmRound(a.freqMhz - 0.1); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune down 0.1 MHz");
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::ArrowButton("##fmup", ImGuiDir_Right)) { a.freqMhz = fmRound(a.freqMhz + 0.1); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune up 0.1 MHz");
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stations found by the band scan (Scan tab).\nType a frequency in the box on the left to tune by hand: 87.5 - 108 MHz.");
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
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (!live) ro("State", "starting", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (fm.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (fm.state == 1) ro("State", "Weak", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "No station", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (live && fm.state >= 1) {
        ro("Sound", fm.stereo ? "Stereo" : "Mono");
        const std::string ps = fmTrim(fm.psName);
        if (!ps.empty()) ro("Station", ps);
    }
    flowBreak();
    if (live && fm.state >= 1) {
        snprintf(b, sizeof b, "%.1f dB", fm.snrDb); ro("SNR", b);
        snprintf(b, sizeof b, "%.0f kHz", fm.devKhz); ro("Deviation", b, fm.devKhz > 80 ? ImVec4(0.95f, 0.70f, 0.15f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
        if (fm.stereo) { snprintf(b, sizeof b, "%.1f %%", fm.pilotPct); ro("Pilot", b); }
        snprintf(b, sizeof b, "%+.0f Hz", fm.cfoHz); ro("Offset", b);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
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
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
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
    static int lastDe = 0;
    static bool lastScan = false;
    if (a.fmMode && a.engine.running()) {   // the sound controls are shared with the other modes: push them to the receiver when they change or it starts
        if (!wasRunning || a.volume != lastVol || a.muted != lastMute || a.fmScan.running != lastScan) {
            a.engine.fm().setVolume(a.volume);
            a.engine.fm().setMuted(a.muted || a.fmScan.running);
            lastVol = a.volume; lastMute = a.muted; lastScan = a.fmScan.running;
        }
    }
    if (a.fmMode && (a.fmDeemph != lastDe || (a.engine.running() && !wasRunning))) { a.engine.fm().setDeemphasis(a.fmDeemph); lastDe = a.fmDeemph; }
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
        row(Ic::Radio, "Station", "%s", live && !fmTrim(fm.psName).empty() ? fmTrim(fm.psName).c_str() : "-");
        row(Ic::Layers, "Programme", "%s", live && !fm.ptyText.empty() && fm.ptyText != "None" ? fm.ptyText.c_str() : "-");
        row(Ic::Speaker, "Sound", "%s", !live ? "-" : fm.stereo ? "stereo" : "mono");
        char rds[48] = "-";
        if (live && fm.rdsSync) snprintf(rds, sizeof rds, "%.0f %% blocks ok", fm.rdsBlockOkPct);
        row(Ic::Wave, "RDS", "%s", rds);
        row(Ic::Warning, "Traffic", "%s", !live ? "-" : fm.trafficAlert ? "ANNOUNCEMENT" : fm.trafficProgram ? "station (TP)" : "no");
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(Ic::Sliders, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("De-emphasis"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        // a narrow list: the region in brackets goes first ("50 us"), the box keeps the value
        const std::string de = fitCaption(a.fmDeemph == 75 ? "75 us (Americas)" : "50 us (Europe, Middle East)", ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().FramePadding.x);
        if (ImGui::BeginCombo("##fmde", de.c_str())) {
            if (ImGui::Selectable("50 us (Europe, Middle East)", a.fmDeemph == 50)) { a.fmDeemph = 50; savePrefs(a); }
            if (ImGui::Selectable("75 us (Americas, South Korea)", a.fmDeemph == 75)) { a.fmDeemph = 75; savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The treble boost the station adds before sending. Wrong setting: dull (75 on a 50 station) or harsh (the other way round) sound.");
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, a.muted ? "Unmute" : "Mute")) { a.muted = !a.muted; }
        ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        float vol = a.volume * 100.f;
        if (ImGui::SliderFloat("##fmvol", &vol, 0, 100, "%.0f %%")) a.volume = vol / 100.f;
        ImGui::EndTable();
    }
    ImGui::Spacing();
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
        if (ImGui::SliderFloat("##fmdial", &f, (float)kFmLow, (float)kFmHigh, "%.1f MHz")) a.freqMhz = fmRound(f);
        if (ImGui::IsItemDeactivatedAfterEdit()) fmTune(a, a.freqMhz);
        const struct { const char* l; double d; } steps[] = {{"-1", -1}, {"-0.1", -0.1}, {"+0.1", 0.1}, {"+1", 1}};
        for (auto& s : steps) { ImGui::SameLine(0, 6 * gUi); if (ImGui::Button(s.l)) fmTune(a, a.freqMhz + s.d); }
    }
    ImGui::Spacing();
    sectionHeader(Ic::Radio, "Stations");
    size_t found = 0;
    for (const auto& r : a.fmScan.results) if (r.found) found++;
    if (!found) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("No stations listed yet. Start the receiver and run the band scan in the Scan tab."); ImGui::PopTextWrapPos(); } return; }
    if (ImGui::BeginTable("fmsta", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 70);
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
static constexpr int kSurveyHops = 11;
static double fmSurveyCentre(int hop) { return 88.5 + 2.0 * hop; }

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
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Surveys the FM band (87.5 - 108 MHz) for carriers, then listens to each one and reads its name. About two minutes."); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    if (!s.running) {
        ImGui::BeginDisabled(!run);
        if (iconButton(Ic::Scan, "Scan FM band", pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) {
            s.running = true; s.phase = 0; s.idx = -1; s.t0 = 0; s.results.clear(); s.cand.clear(); s.savedFreq = a.freqMhz;
        }
        ImGui::EndDisabled();
        if (!run) { ImGui::SameLine(); ImGui::TextDisabled("start the receiver first"); }
    } else {
        if (iconButton(Ic::Stop, "Stop scan", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255))) { s.running = false; fmTune(a, s.savedFreq); }
        ImGui::SameLine(0, 12 * gUi);
        float frac;
        if (s.phase == 0) {
            { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("surveying the band, step %d of %d", std::max(0, s.idx) + 1, kSurveyHops); ImGui::PopTextWrapPos(); }
            frac = 0.2f * (float)std::max(0, s.idx) / kSurveyHops;
        } else {
            { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("checking %.1f MHz (%d of %d)", s.idx >= 0 && s.idx < (int)s.cand.size() ? s.cand[(size_t)s.idx] : 0.0, s.idx + 1, (int)s.cand.size()); ImGui::PopTextWrapPos(); }
            frac = 0.2f + 0.8f * (float)std::max(0, s.idx) / std::max<size_t>(1, s.cand.size());
        }
        ImGui::ProgressBar(frac, ImVec2(220, ImGui::GetFrameHeight() - 6));
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("fmscan", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80); ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70); ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("RDS", ImGuiTableColumnFlags_WidthFixed, 50);
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
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.pty.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.snr);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.stereo ? "stereo" : "mono");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.rds ? "yes" : "-");
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
    };
    auto finish = [&]() {
        s.running = false;
        a.freqMhz = s.savedFreq; a.tune.centerHz = a.freqMhz * 1e6; a.engine.retuneReset(a.tune);
        size_t n = 0; for (const auto& r : s.results) if (r.found) n++;
        a.engine.log("FM scan: " + std::to_string(n) + " stations");
        savePrefs(a);
    };

    if (s.phase == 0) {
        if (s.idx < 0) { s.idx = 0; tuneTo(fmSurveyCentre(0)); return; }
        if (now - s.t0 < 0.7 || a.lastSeq == s.seq0) return;
        const double centre = fmSurveyCentre(s.idx), fs = a.engine.sampleRate();
        double db[20];
        for (int j = 0; j < 20; j++) db[j] = fmChannelDb(a.spec, fs, (-1.0 + 0.1 * j) * 1e6);
        double sorted[20]; std::copy(db, db + 20, sorted); std::sort(sorted, sorted + 20);
        const double floorDb = sorted[4];
        for (int j = 0; j < 20; j++) {
            const double mhz = std::round((centre - 1.0 + 0.1 * j) * 10) / 10;
            if (mhz < kFmLow - 0.01 || mhz > kFmHigh + 0.01) continue;
            const bool peak = (j == 0 || db[j] >= db[j - 1]) && (j == 19 || db[j] >= db[j + 1]);
            if (db[j] - floorDb > 9 && peak) s.cand.push_back(mhz);
        }
        if (++s.idx >= kSurveyHops) {
            std::sort(s.cand.begin(), s.cand.end());
            s.phase = 1; s.idx = 0;
            if (s.cand.empty()) { finish(); return; }
            tuneTo(s.cand[0]);
        } else tuneTo(fmSurveyCentre(s.idx));
        return;
    }

    const double dwell = now - s.t0;
    const FmTelemetry& fm = a.rx.fm;
    const bool fresh = a.rx.standard == 6 && a.rx.seq > s.rxSeq0 && dwell > 0.9;
    if (!fresh && dwell < 3.0) return;   // no fresh telemetry yet
    const bool station = fresh && fm.carrier && fm.state >= 1;
    const bool nameKnown = station && !fmTrim(fm.psName).empty();
    const bool done = !station ? dwell > 1.4 : (nameKnown && dwell > 1.6) || dwell > 5.0;
    if (!done) return;
    App::FmScan::Res r;
    r.mhz = s.cand[(size_t)s.idx];
    r.found = station;
    if (station) {
        r.snr = fm.snrDb; r.stereo = fm.stereo; r.rds = fm.rdsSync; r.name = fmTrim(fm.psName); r.pty = fm.ptyText == "None" ? "" : fm.ptyText;
        a.engine.log("FM scan: " + fmStationLabel(r) + " SNR " + std::to_string((int)r.snr) + " dB" + (r.stereo ? " stereo" : ""));
    }
    s.results.push_back(r);
    if (++s.idx >= (int)s.cand.size()) { finish(); return; }
    tuneTo(s.cand[(size_t)s.idx]);
}
