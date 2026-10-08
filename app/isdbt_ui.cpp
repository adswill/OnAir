// ISDB-T: the status bar and the Receiver tab (lock, carrier layout, the three hierarchical layers with their statistics)
#include "app.h"
#include "dect2/isdbt.h"

static bool telemI(App& a) { return a.engine.running() && a.rx.standard == 5; }

static isdbt::Params paramsOf(const RxTelemetry& t) {
    isdbt::Params p;
    p.mode = t.isdbt.mode > 0 ? t.isdbt.mode : 3;
    p.guard = t.giIdx >= 0 ? t.giIdx : 2;
    p.partial = t.isdbt.partial;
    for (int i = 0; i < 3; i++) { p.layer[i].segments = t.isdbt.layer[i].segments; p.layer[i].mod = t.isdbt.layer[i].mod; p.layer[i].rate = t.isdbt.layer[i].rate; p.layer[i].ti = t.isdbt.layer[i].ti; }
    return p;
}

void isdbtStatus(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const bool live = telemI(a);
    const RxTelemetry& t = a.rx;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    bool sync = false;
    uint64_t okAll = 0, badAll = 0;
    for (int i = 0; i < 3; i++) { sync |= t.isdbt.layer[i].synced; okAll += t.isdbt.layer[i].rsClean + t.isdbt.layer[i].rsCorrected; badAll += t.isdbt.layer[i].rsFailed; }
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Mode", !live ? 0 : t.fftN ? 1 : 0); flowNext(12 * gUi);
    lamp("TMCC", !live ? 0 : t.isdbt.tmccOk ? 1 : t.fftN ? 2 : 0); flowNext(12 * gUi);
    lamp("Layers", !live || !t.isdbt.tmccOk ? 0 : sync ? 1 : 2); flowNext(12 * gUi);
    lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : 1, (int)Ic::Layers); flowNext(12 * gUi);
    const PlayerStats ps = a.engine.player().stats();
    const bool pl = run && ps.active;
    lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); flowNext(12 * gUi);
    lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (a.engine.radioLost()) ro("State", "radio disconnected", ImVec4(0.95f, 0.35f, 0.3f, 1));
    else if (!live) ro("State", "starting", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (t.isdbt.tmccOk && sync) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (t.isdbt.tmccOk) ro("State", "TMCC decoded", ImVec4(0.95f, 0.75f, 0.2f, 1));
    else if (t.fftN) ro("State", "syncing", ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (live && t.fftN) {
        snprintf(b, sizeof b, "%d", t.isdbt.mode ? t.isdbt.mode : (t.fftN == 2048 ? 1 : t.fftN == 4096 ? 2 : 3)); ro("Mode", b);
        ro("GI", isdbt::guardName(t.giIdx));
    }
    flowBreak();
    if (live && t.fftN) {
        snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
        if (t.isdbt.tmccOk) { snprintf(b, sizeof b, "%.1f dB", t.dataSnrDb); ro("SNR", b); }
        if (okAll + badAll > 0) { snprintf(b, sizeof b, "%.1f%%", 100.0 * (double)okAll / (double)(okAll + badAll)); ro("Packets ok", b); }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    if (run) { snprintf(b, sizeof b, "%llu", (unsigned long long)a.engine.droppedSamples()); ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1)); }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

void isdbtReceiverTab(App& a) {
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("ISDB-T receiver: mode and guard interval from the cyclic prefix, carrier and clock tracking, TMCC, equalisation, de-interleaving, Viterbi, Reed-Solomon"); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    if (!telemI(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Start the receiver with the ISDB-T switch selected."); ImGui::PopTextWrapPos(); } return; }
    const RxTelemetry& t = a.rx;
    auto row = [&](const char* k, const char* fmt, auto... v) {   // a long value wraps under itself in a narrow tab
        ImGui::TextDisabled("%s", k); ImGui::SameLine(std::min(190 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    row("synchronisation", "%s", t.isdbt.tmccOk ? "TMCC decoded" : t.fftN ? "signal found, waiting for the TMCC" : "searching for an ISDB-T signal");
    if (t.fftN) {
        const int mode = t.isdbt.mode ? t.isdbt.mode : (t.fftN == 2048 ? 1 : t.fftN == 4096 ? 2 : 3);
        row("mode", "%d: %d carriers, %.3f kHz spacing, %d-point FFT, guard interval %s", mode, isdbt::totalCarriers(mode), isdbt::kSampleRate / t.fftN / 1e3, t.fftN, isdbt::guardName(t.giIdx));
        row("frame", "%d symbols, %.1f ms", isdbt::kSymbolsPerFrame, t.frameMs);
    }
    row("carrier offset", "%+.1f Hz", t.cfoHz);
    row("sample clock", "%+.1f ppm corrected", t.sroPpm);
    if (t.isdbt.tmccOk) {
        row("signal to noise", "%.1f dB (pilots)", t.dataSnrDb);
        row("TMCC", "%s%s, parameter switching %s, emergency alert flag %s", t.isdbt.partial ? "one-segment partial reception layer present" : "no partial reception layer",
            "", t.isdbt.switching == 15 ? "not announced" : "announced", t.isdbt.emergency ? "set" : "off");
        const isdbt::Params p = paramsOf(t);
        ImGui::Spacing();
        if (ImGui::BeginTable("isdbtlayers", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Layer"); ImGui::TableSetupColumn("Segments"); ImGui::TableSetupColumn("Modulation"); ImGui::TableSetupColumn("Code rate");
            ImGui::TableSetupColumn("Interleaving"); ImGui::TableSetupColumn("Bit rate"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Reed-Solomon");
            ImGui::TableHeadersRow();
            for (int i = 0; i < 3; i++) {
                const auto& L = t.isdbt.layer[i];
                if (!L.segments) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%c%s", 'A' + i, i == 0 && t.isdbt.partial ? " (partial)" : "");
                ImGui::TableNextColumn(); ImGui::Text("%d", L.segments);
                ImGui::TableNextColumn(); ImGui::Text("%s", isdbt::modName(L.mod));
                ImGui::TableNextColumn(); ImGui::Text("%s", isdbt::rateName(L.rate));
                ImGui::TableNextColumn(); ImGui::Text("I = %d", isdbt::interleavingLength(p.mode, L.ti));
                ImGui::TableNextColumn(); ImGui::Text("%.2f Mbit/s", isdbt::layerBitrate(p, i) / 1e6);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)L.packets);
                ImGui::TableNextColumn();
                const double tot = (double)(L.rsClean + L.rsCorrected + L.rsFailed);
                { ImGui::PushTextWrapPos(0); ImGui::Text("%s  %llu fixed, %llu failed (%.1f%%)", L.synced ? "locked" : "searching", (unsigned long long)L.rsCorrected, (unsigned long long)L.rsFailed, tot > 0 ? 100.0 * (double)L.rsFailed / tot : 0.0); ImGui::PopTextWrapPos(); }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        row("total net rate", "%.2f Mbit/s", isdbt::totalBitrate(p) / 1e6);
    }
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Picture and sound appear in the TV tab. Names written in the Japanese character set may show up with placeholder characters."); ImGui::PopTextWrapPos(); }
}
