// FM radio UI panels
#include "app.h"

// FM receiver status and controls

struct FmFreq { const char* name; double mhz; };
static const FmFreq kFmStations[] = {
    {"FM 88.0", 88.0}, {"FM 89.0", 89.0}, {"FM 90.0", 90.0}, {"FM 91.0", 91.0}, {"FM 92.0", 92.0},
    {"FM 93.0", 93.0}, {"FM 94.0", 94.0}, {"FM 95.0", 95.0}, {"FM 96.0", 96.0}, {"FM 97.0", 97.0},
    {"FM 98.0", 98.0}, {"FM 99.0", 99.0}, {"FM 100.0", 100.0}, {"FM 101.0", 101.0}, {"FM 102.0", 102.0},
    {"FM 103.0", 103.0}, {"FM 104.0", 104.0}, {"FM 105.0", 105.0}, {"FM 106.0", 106.0}, {"FM 107.0", 107.0},
};
static constexpr int kNumFmStations = (int)(sizeof kFmStations / sizeof *kFmStations);

bool fmFrequencyCombo(App& a) {
    char lbl[48];
    snprintf(lbl, sizeof lbl, "Frequency: %.2f MHz", a.freqMhz);
    bool changed = false;
    ImGui::SetNextItemWidth(180 * gUi);
    if (ImGui::BeginCombo("##fmfreq", lbl)) {
        for (const auto& st : kFmStations) {
            if (ImGui::Selectable(st.name, std::fabs(a.freqMhz - st.mhz) < 0.01)) {
                a.freqMhz = st.mhz;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("FM broadcast band: 87.5 - 108 MHz");
    return changed;
}

void fmStatus(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const FmTelemetry& fm = a.rx.fm;
    const bool live = run && a.rx.standard == 6;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave);
    ImGui::SameLine(0, 12 * gUi);
    lamp("Lock", !live ? 0 : fm.state == 2 ? 1 : fm.state == 1 ? 2 : 0);
    ImGui::SameLine(0, 12 * gUi);
    lamp("Stereo", !live ? 0 : fm.stereo ? 1 : 2, (int)Ic::Radio);
    ImGui::SameLine(0, 12 * gUi);
    lamp("RDS", !live ? 0 : fm.rdsSync ? 1 : 2);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 10 * gUi);
    
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(col, "%s", val.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (!live) ro("State", "starting", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (fm.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (fm.state == 1) ro("State", "Tracking", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "Searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    
    if (live && fm.state >= 1) {
        if (fm.stereo) ro("Mode", "Stereo FM");
        else ro("Mode", "Mono FM");
        
        if (!fm.psName.empty()) ro("Station", fm.psName);
    }
    
    ImGui::NewLine();
    if (live && fm.state >= 1) {
        snprintf(b, sizeof b, "%+.1f Hz", fm.cfoHz);
        ro("CFO", b);
        snprintf(b, sizeof b, "%.1f dB", fm.snrDb);
        ro("SNR", b);
        snprintf(b, sizeof b, "%.1f dB", fm.pilotLockDb);
        ro("Pilot", b);
    }
    if (run) {
        snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6);
        ro("fs", b);
    }
    
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f);
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("Level");
    ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? IM_COL32(40, 112, 150, 255) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    ImGui::SameLine(0, 15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f);
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("Signal");
    ImGui::SameLine(0, 5 * gUi);
    {
        const float t = live && fm.state >= 1 ? std::min(1.f, std::max(0.f, (float)((fm.snrDb + 10.0) / 30.0))) : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : IM_COL32(40, 112, 150, 255);
        snprintf(b, sizeof b, live && fm.state >= 1 ? "%.0f%%  %s" : "-", t * 100, t > 0.8f ? "excellent" : t > 0.55f ? "good" : t > 0.3f ? "fair" : "poor");
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && live) ImGui::SetTooltip("FM signal quality.\nSNR %.1f dB", fm.snrDb);
    }
    ImGui::SameLine(0, 15 * gUi);
    if (run) {
        snprintf(b, sizeof b, "%llu", (unsigned long long)a.engine.droppedSamples());
        ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)");
        else if (adc == AdcStatus::Low) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain");
        else if (adc == AdcStatus::NoSignal) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?");
        else ImGui::NewLine();
    } else ImGui::NewLine();
}

void fmPanels(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float side = std::max(90.f, std::min(availH - 26.f - ImGui::GetFrameHeight(), availW / 3.f - 16.f));
    const float gap = std::max(6.f, (availW - 3 * side) / 4.f);
    const ImVec2 sz(side, side);

    // Convert deques to vectors for plotting
    std::vector<float> snrVec(a.fmSnrH.begin(), a.fmSnrH.end());
    std::vector<float> pilotVec(a.fmPilotH.begin(), a.fmPilotH.end());
    std::vector<float> rdsVec(a.fmRdsH.begin(), a.fmRdsH.end());

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Signal Level");
    if (plt::BeginPlot("##fmlevel", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("samples", "dB", 0, plt::AxisFlags_NoTickLabels);
        if (!snrVec.empty()) {
            plt::PlotLine("SNR", snrVec.data(), (int)snrVec.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Pilot Lock");
    if (plt::BeginPlot("##fmpilot", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("samples", "dB", 0, plt::AxisFlags_NoTickLabels);
        if (!pilotVec.empty()) {
            plt::PlotLine("Pilot", pilotVec.data(), (int)pilotVec.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("RDS Status");
    if (plt::BeginPlot("##fmrds", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("samples", "sync", 0, plt::AxisFlags_NoTickLabels);
        if (!rdsVec.empty()) {
            plt::PlotLine("RDS", rdsVec.data(), (int)rdsVec.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
}

void fmHistory(App& a) {
    static uint64_t lastSeq = 0;
    if (a.rx.standard != 6 || a.rx.seq == lastSeq) return;
    lastSeq = a.rx.seq;
    
    auto push = [](std::deque<float>& dq, float v) {
        dq.push_back(v);
        if (dq.size() > 600) dq.pop_front();
    };
    
    const FmTelemetry& fm = a.rx.fm;
    if (fm.state >= 1) {
        push(a.fmSnrH, fm.snrDb);
        push(a.fmPilotH, fm.pilotLockDb);
        push(a.fmRdsH, fm.rdsSync ? 1.0f : 0.0f);
    }
}

void fmRdsDisplay(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    const bool live = a.engine.running() && a.rx.standard == 6;
    
    if (!live || fm.state < 1) {
        ImGui::TextDisabled("RDS not available");
        return;
    }
    
    if (!fm.psName.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Station:");
        ImGui::SameLine();
        ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(fm.psName.c_str());
        ImGui::PopFont();
    }
    
    if (!fm.radioText.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Text:");
        ImGui::SameLine();
        ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(fm.radioText.c_str());
        ImGui::PopFont();
    }
    
    if (fm.trafficAlert) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "TA - TRAFFIC ALERT");
    }
    
    if (!fm.ptyText.empty()) {
        ImGui::TextDisabled("Programme Type: %s", fm.ptyText.c_str());
    }
}
