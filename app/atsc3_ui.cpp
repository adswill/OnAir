// ATSC 3.0: the status bar and the Receiver tab (lock, frame structure, service list, statistics)
#include "app.h"

static const char* modName(int bits) {
    switch (bits) { case 2: return "QPSK"; case 4: return "16QAM"; case 6: return "64QAM"; case 8: return "256QAM"; case 10: return "1024QAM"; case 12: return "4096QAM"; default: return "?"; }
}

static bool telem(App& a, Atsc3Telemetry& t) { return a.engine.running() && a.rx.standard == 4 && a.engine.atsc3Telemetry(t); }

bool atsc3Quality(App& a, QualityReport& q) {
    Atsc3Telemetry t;
    if (!telem(a, t) || !t.locked || !t.frame.valid) return false;
    long ok = 0, all = 0;
    for (const auto& p : t.frame.plps) { ok += p.blocksOk; all += p.blocks; }
    const double decoded = all > 0 ? (double)ok / (double)all : 0.0;
    const double frames = t.frames > 0 ? 1.0 - (double)t.framesFailed / (double)(t.frames + t.framesFailed) : 0.0;
    static double smooth = 0;
    smooth = smooth <= 0 ? decoded * frames * 100.0 : smooth * 0.9 + decoded * frames * 100.0 * 0.1;
    q.valid = true;
    q.percent = smooth;
    q.fecOk = decoded;
    q.snrDb = q.requiredDb = q.marginDb = 0;
    q.label = smooth >= 99 ? "excellent" : smooth >= 90 ? "good" : smooth >= 60 ? "marginal" : "poor";
    return true;
}

void atsc3Status(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    Atsc3Telemetry t;
    const bool live = telem(a, t);
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    const double okFrac = live && t.bbPackets > 0 ? 1.0 - (double)t.bbBad / (double)(t.bbPackets + t.bbBad) : 0.0;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Bootstrap", !live ? 0 : t.bootstraps > 0 ? 1 : 0); flowNext(12 * gUi);
    lamp("L1", !live ? 0 : t.locked ? 1 : t.bootstraps > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("PLP", !live ? 0 : t.bbPackets == 0 ? 0 : okFrac > 0.99 ? 1 : okFrac > 0.5 ? 2 : 3); flowNext(12 * gUi);
    lamp("Service", !live ? 0 : t.serviceReady ? 1 : !t.services.empty() ? 2 : 0); flowNext(12 * gUi);
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
    else if (t.locked) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (t.bootstraps > 0) ro("State", "syncing", ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (live && t.locked && t.frame.valid) {
        snprintf(b, sizeof b, "%dK", t.frame.fftSize / 1024); ro("FFT", b);
        snprintf(b, sizeof b, "%d", t.frame.guard); ro("GI", b);
    }
    flowBreak();
    if (live && t.locked) {
        snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
        snprintf(b, sizeof b, "%ld", t.frames); ro("Frames", b);
        snprintf(b, sizeof b, "%.0f%%", 100 * okFrac); ro("PLP ok", b);
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
    if (live && t.load > 0) {
        snprintf(b, sizeof b, "%.0f%%", 100 * t.load);
        ro("CPU", b, t.load > 0.95 ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1));
    }
    if (run) { snprintf(b, sizeof b, "%llu", (unsigned long long)(a.engine.droppedSamples() + (live ? (unsigned long long)t.droppedBlocks : 0)));
               ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1)); }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

void atsc3ReceiverTab(App& a) {
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("ATSC 3.0 receiver: bootstrap search with carrier offset, Preamble and L1 signalling, OFDM frames, LDPC, link layer, ROUTE, MP4 remux"); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    Atsc3Telemetry t;
    if (!telem(a, t)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Start the receiver with the ATSC 3.0 switch selected."); ImGui::PopTextWrapPos(); } return; }
    auto row = [&](const char* k, const char* fmt, auto... v) {   // a long value wraps under itself in a narrow tab
        ImGui::TextDisabled("%s", k); ImGui::SameLine(std::min(190 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    row("synchronisation", "%s", t.locked ? "locked" : t.bootstraps > 0 ? "bootstrap seen, no frame decodes yet" : "searching for a bootstrap");
    row("carrier offset", "%+.1f Hz", t.cfoHz);
    row("bootstraps / frames", "%ld found, %ld decoded, %ld failed", t.bootstraps, t.frames, t.framesFailed);
    if (t.frame.valid) {
        const auto& f = t.frame;
        row("frame", "%dK FFT, guard %d samples, scattered pilots 1 in %d (every %d symbols), %d symbols, %d Preamble symbol(s)", f.fftSize / 1024, f.guard, f.spDx, f.spDy, f.symbols, f.preambleSymbols);
        row("bootstrap", "minor version %d, %d MHz channel, preamble structure %d", f.bootstrapMinor, f.bandwidthMhz, f.preambleStructure);
        for (auto& p : f.plps)
            row("PLP", "%d: %s, code rate %d/15, %dK LDPC, %d block(s) in the last frame, %d decoded", p.id, modName(p.bitsPerCell), p.rate15, p.nInner / 1000, p.blocks, p.blocksOk);
    }
    row("baseband packets", "%ld (%ld bad)", t.bbPackets, t.bbBad);
    row("link layer", "%ld ALP packets, %ld UDP datagrams, %ld service list tables, %ld ROUTE objects", t.alpPackets, t.udp, t.llsTables, t.routeObjects);
    row("decoder load", "%.0f%% of real time%s", 100 * t.load, t.droppedBlocks ? "  (samples were dropped)" : "");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Services");
    if (t.services.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("waiting for the service list (sent about once a second)"); ImGui::PopTextWrapPos(); } return; }
    if (ImGui::BeginTable("atsc3svc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Id"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Delivery"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        static const char* cats[] = {"?", "TV", "Radio", "App", "Guide", "Alerts", "DRM"};
        for (auto& s : t.services) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d.%d", s.majorChannel, s.minorChannel);
            ImGui::TableNextColumn(); ImGui::Text("%s%s", s.shortName.c_str(), s.hidden ? " (hidden)" : "");
            ImGui::TableNextColumn(); ImGui::Text("%d", s.serviceId);
            ImGui::TableNextColumn(); ImGui::Text("%s", s.category >= 0 && s.category <= 6 ? cats[s.category] : "?");
            const bool route = s.slsProtocol == 1;
            ImGui::TableNextColumn(); ImGui::Text("%s", route ? "ROUTE" : s.slsProtocol == 2 ? "MMTP" : "-");
            ImGui::TableNextColumn();
            ImGui::PushID(s.serviceId);
            if (!route) { ImGui::TextDisabled("not supported yet"); }
            else if (t.selected == s.serviceId) { { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), t.serviceReady ? "receiving" : "waiting for its signaling"); ImGui::PopTextWrapPos(); } }
            else if (ImGui::SmallButton("Receive")) a.engine.atsc3Select(s.serviceId);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Picture and sound appear in the TV tab. Services that use MMTP instead of ROUTE are listed but cannot be received yet."); ImGui::PopTextWrapPos(); }
}
