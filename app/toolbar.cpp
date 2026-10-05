// the top bar, the source options, the status bar, gain control and the standard switch
#include "app.h"

void toolbar(App& a) {
    bool running = a.engine.running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::accent(), "ONAIR");
    ImGui::SameLine(0, 16 * gUi);
    ImGui::TextDisabled("SRC"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(190 * gUi);
    ImGui::BeginDisabled(running);
    if (ImGui::BeginCombo("##src", a.devices[a.devIdx].name.c_str())) {
        for (int i = 0; i < (int)a.devices.size(); i++)
            if (ImGui::Selectable(a.devices[i].name.c_str(), i == a.devIdx)) a.devIdx = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (iconFlat(Ic::Refresh, "Rescan for devices")) refreshDevices(a), a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1);
    ImGui::EndDisabled();

    bool isHw = a.devices[a.devIdx].isRadio();
    const DeviceInfo& curDev = a.devices[a.devIdx];
    const bool generic = curDev.isGeneric();
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;

    vSeparator();
    ImGui::TextDisabled("FREQ");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centre frequency");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    bool retune = false;
    ImGui::InputDouble("##freq", &a.freqMhz, 0, 0, "%.3f MHz");
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true;
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled(a.dabMode ? "CH" : "BW");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(a.dabMode ? "DAB channel" : "Channel bandwidth");
    ImGui::SameLine(0, 5 * gUi);
    if (a.dabMode) { if (dabChannelCombo(a)) retune = true; }
    else {
    ImGui::SetNextItemWidth((a.bwAuto ? 125 : 80) * gUi);
    {
        char bl[32];
        snprintf(bl, sizeof bl, a.bwAuto ? "%s (auto)" : "%s", kBw[a.bwIdx].label);
        if (a.atscMode) snprintf(bl, sizeof bl, "6 MHz");
        ImGui::BeginDisabled(a.atscMode || (running && !a.bwAuto));
        if (ImGui::BeginCombo("##bw", bl)) {
            for (int i = 0; i < (int)(sizeof kBw / sizeof *kBw); i++)
                if (ImGui::Selectable(kBw[i].label, i == a.bwIdx && !a.bwAuto)) { a.bwIdx = i; a.bwAuto = false; savePrefs(a); }
            ImGui::Separator();
            if (ImGui::Selectable("Automatic", a.bwAuto)) { a.bwAuto = true; savePrefs(a); }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Automatic: the width of the signal is measured in the spectrum and the channel bandwidth (5, 6, 7 or 8 MHz) is set for you.");
        if (generic && curDev.maxRateHz > 0 && curDev.maxRateHz < 7.9e6 * kBw[a.bwIdx].mhz / 8.0 - 1) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "(!)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This radio tops out at %.2f Msps; a %g MHz channel needs about %.1f Msps.\nChoose a narrower channel (for example 1.7 MHz DVB-T2-Lite) or a faster radio.", curDev.maxRateHz / 1e6, kBw[a.bwIdx].mhz, 7.9 * kBw[a.bwIdx].mhz / 8.0);
        }
    }
    }

    vSeparator();
    ImGui::BeginDisabled(!isHw);
    ImGui::TextDisabled("GAIN"); ImGui::SameLine(0, 5 * gUi);
    if (generic) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * gUi);
        float gdb = (float)a.tune.gainDb;
        ImGui::SliderFloat("##gain", &gdb, (float)curDev.gainMinDb, (float)std::max(curDev.gainMaxDb, curDev.gainMinDb + 1.0), "%.0f dB");
        a.tune.gainDb = std::round(gdb);
        if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    } else {
    ImGui::TextDisabled("LNA");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(78 * gUi);
    int lna = a.tune.lnaDb;
    ImGui::SliderInt("##lna", &lna, 0, 40, "%d dB");
    a.tune.lnaDb = (lna + 4) / 8 * 8; // hardware steps are 8 dB
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    ImGui::SameLine();
    ImGui::TextDisabled("VGA");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(78 * gUi);
    int vga = a.tune.vgaDb;
    ImGui::SliderInt("##vga", &vga, 0, 62, "%d dB");
    a.tune.vgaDb = (vga + 1) / 2 * 2; // 2 dB steps
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Amp", &a.tune.ampOn)) retune = true, a.agcOn = false;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("AGC", &a.agcOn)) { a.agc.reset(); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Keep the ADC level in a healthy window (about -16 dBFS rms, no clipping) by adjusting LNA, VGA and amp.\nTouching a gain control turns it off.");
    ImGui::SameLine();
    if (a.sweep.active()) {
        if (ImGui::Button("Stop tune")) { a.sweep = GainSweep(); }
    } else if (ImGui::Button("Auto-tune") && running) {
        a.sweep.start(ImGui::GetTime(), generic ? (int)curDev.gainMaxDb : 0);
        a.agcOn = false;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Gain helper: tries LNA/VGA/amp combinations for about 40 s and keeps the one with the best SNR that does not clip.\nNeeds a signal the receiver can lock to.");
    ImGui::EndDisabled();
    if (!isHw && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Gain controls apply to radios only (not to a recording or the synthetic signal)");

    vSeparator();
    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.42f, 0.28f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.56f, 0.36f, 1));
        const bool startClicked = iconButton(Ic::Play, "Start", IM_COL32(40, 70, 82, 255), IM_COL32(56, 94, 110, 255)) || a.wizStart;
        a.wizStart = false;
        ImGui::PopStyleColor(2);
        if (startClicked) {
            a.tune.centerHz = a.freqMhz * 1e6;
            applyBandwidth(a);
            if (isFile) { const double r = guessSampleRate(a.file.path); if (r > 0) a.file.sampleRate = r; } // the name says the rate
            if (isFile && a.file.path.empty()) a.engine.log("choose an IQ file first");
            else {
                a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
                a.engine.start(a.devices[a.devIdx], a.tune, a.file);
                savePrefs(a);
                a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1));
        const bool stopClicked = iconButton(Ic::Stop, "Stop", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255));
        ImGui::PopStyleColor(2);
        if (stopClicked) a.engine.stop();
    }

    if (a.family != 2) {
    vSeparator();
    {
        static const char* modes[] = {"CPU", "GPU", "Auto"};
        char lbl[48];
        const bool gpuNow = a.rx.plpOnGpu;
        snprintf(lbl, sizeof lbl, "%s", a.computeMode == 2 ? (gpuNow ? "Auto (GPU)" : "Auto (CPU)") : modes[a.computeMode]);
        ImGui::TextDisabled("COMPUTE"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(104 * gUi);
        if (ImGui::BeginCombo("##compute", lbl)) {
            for (int i = 0; i < 3; i++) {
                const bool dis = i == 1 && !a.rx.gpuAvailable && running;
                if (dis) ImGui::BeginDisabled();
                if (ImGui::Selectable(modes[i], a.computeMode == i)) { a.computeMode = i; a.engine.setComputeMode(i); savePrefs(a); }
                if (dis) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            const char* gpuName = GpuLdpc::instance().deviceName();
            ImGui::SetTooltip("LDPC decoding backend.\n"
#ifdef __APPLE__
                "CPU: NEON on all cores. GPU: Metal compute.\n"
                "Auto: the GPU when there is one, the CPU otherwise.\n"
#else
                "CPU: AVX2 on all cores. GPU: Direct3D 11 compute (Windows).\n"
                "Auto: the GPU when a discrete graphics card is found, otherwise the CPU\n"
                "first and the GPU if it falls behind real time.\n"
#endif
                "Graphics: %s", a.rx.gpuAvailable ? gpuName : "none usable");
        }
    }
    if (a.family == 0) {   // ATSC, ATSC 3.0, ISDB-T and DAB have one standard each: nothing to choose
        ImGui::SameLine(0, 14 * gUi);
        static const char* names[] = {"Auto", "DVB-T2", "DVB-T"};
        char lbl[48];
        const int act = a.engine.activeStandard();
        snprintf(lbl, sizeof lbl, "%s", a.stdMode == 0 ? (a.rx.standard == 1 ? "Auto (DVB-T)" : "Auto (DVB-T2)") : names[a.stdMode]);
        (void)act;
        ImGui::TextDisabled("STD"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(112 * gUi);
        if (ImGui::BeginCombo("##std", lbl)) {
            for (int i = 0; i < 3; i++) if (a.family == 0 && ImGui::Selectable(names[i], a.stdMode == i)) { a.stdMode = i; a.engine.setStandard(i); savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which broadcast standard to decode.\nAuto alternates between DVB-T2 and DVB-T until one locks.");
    }

    }

    if (retune && running) {
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.log("retune " + std::to_string(a.freqMhz) + " MHz");
        a.engine.retune(a.tune);
        a.peak.clear();
        savePrefs(a);
    }

}

// Options of the selected source that are not gain/frequency: the synthetic generator's parameters, or the IQ file.
void sourceOptions(App& a) {
    bool running = a.engine.running();
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic) {
        SynthConfig& sc = a.tune.synth;
        bool ch = false;
        ImGui::TextDisabled("synthetic");
        ImGui::SameLine();
        if (!a.atscMode && ImGui::Checkbox("DVB-T", &sc.dvbt)) ch = true;
        ImGui::SameLine();
        if (a.atscMode) {
            ImGui::TextDisabled("ATSC 8-VSB, 6 MHz");
        } else if (sc.dvbt) {
            static const char* fftN[] = {"2K", "8K"}; static const char* gis[] = {"1/32", "1/16", "1/8", "1/4"};
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM"}; static const char* rates[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
            ImGui::SetNextItemWidth(60 * gUi);
            if (ImGui::BeginCombo("##tfft", fftN[sc.dvbtMode & 1])) { for (int i = 0; i < 2; i++) if (ImGui::Selectable(fftN[i], i == sc.dvbtMode)) { sc.dvbtMode = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::TextDisabled("GI"); ImGui::SameLine(); ImGui::SetNextItemWidth(66 * gUi);
            if (ImGui::BeginCombo("##tgi", gis[sc.dvbtGuard & 3])) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(gis[i], i == sc.dvbtGuard)) { sc.dvbtGuard = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(84 * gUi);
            if (ImGui::BeginCombo("##tmod", mods[sc.dvbtMod % 3])) { for (int i = 0; i < 3; i++) if (ImGui::Selectable(mods[i], i == sc.dvbtMod)) { sc.dvbtMod = i; ch = true; } ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi);
            if (ImGui::BeginCombo("##trate", rates[sc.dvbtRate % 5])) { for (int i = 0; i < 5; i++) if (ImGui::Selectable(rates[i], i == sc.dvbtRate)) { sc.dvbtRate = i; ch = true; } ImGui::EndCombo(); }
        } else {
        static const struct { const char* n; int code; } fm[] = {{"2K", 0}, {"8K", 1}, {"4K", 2}, {"1K", 3}, {"16K", 4}, {"32K", 5}};
        const char* cur = "?";
        for (auto& f : fm) if (f.code == sc.tx.s2field1) cur = f.n;
        ImGui::SetNextItemWidth(60 * gUi);
        if (ImGui::BeginCombo("##sfft", cur)) {
            for (auto& f : fm) if (ImGui::Selectable(f.n, f.code == sc.tx.s2field1)) { sc.tx.s2field1 = f.code; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::TextDisabled("GI"); ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::BeginCombo("##sgi", guardName(sc.tx.giIdx))) {
            for (int g = 0; g < kNumGi; g++) if (ImGui::Selectable(guardName(g), g == sc.tx.giIdx)) { sc.tx.giIdx = g; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("ext", &sc.tx.ext)) ch = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(62 * gUi);
        {
            char ppl[8]; snprintf(ppl, sizeof ppl, "PP%d", sc.tx.pp + 1);
            if (ImGui::BeginCombo("##spp", ppl)) {
                for (int q = 0; q < 8; q++) { snprintf(ppl, sizeof ppl, "PP%d", q + 1); if (ImGui::Selectable(ppl, q == sc.tx.pp)) { sc.tx.pp = q; ch = true; } }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("TR", &sc.tx.tr)) ch = true;
        }
        ImGui::SameLine(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##ssnr", &snr, 0, 40, "%.0f dB")) { sc.snrDb = snr; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
        float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##scfo", &cfo, -40, 40, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("SRO"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float sro = (float)sc.sroPpm; if (ImGui::SliderFloat("##ssro", &sro, -50, 50, "%.0f ppm")) { sc.sroPpm = sro; ch = true; }
        ImGui::SameLine(); ImGui::TextDisabled("echo"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float ec = (float)sc.echoDb; if (ImGui::SliderFloat("##sec", &ec, 0, 20, ec == 0 ? "off" : "-%.0f dB")) { sc.echoDb = ec; ch = true; }
        if (ch && running) a.engine.retune(a.tune);
    }
    if (isFile && !running) {
        ImGui::TextDisabled("file");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open…")) { auto p = openFileDialog(); if (!p.empty()) { a.file.path = p; a.file.format = guessFormat(p); const double r = guessSampleRate(p); if (r > 0) a.file.sampleRate = r; } }
        ImGui::SameLine();
        ImGui::TextUnformatted(a.file.path.empty() ? "(none)" : a.file.path.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        const char* fm[] = {"cs8", "cu8", "cf32"};
        int fi = (int)a.file.format;
        if (ImGui::Combo("##ff", &fi, fm, 3)) a.file.format = (FileFormat)fi;
        ImGui::SameLine();
        ImGui::TextDisabled("rate");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        double msps = a.file.sampleRate / 1e6;
        if (ImGui::InputDouble("##frate", &msps, 0, 0, "%.4f Msps")) a.file.sampleRate = msps * 1e6;
        ImGui::SameLine();
        ImGui::Checkbox("loop", &a.file.loop);
    }
}

void statusBar(App& a) {
    if (a.dabMode) { dabStatus(a); return; }
    if (a.atsc3Mode) { atsc3Status(a); return; }
    if (a.isdbtMode) { isdbtStatus(a); return; }
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const RxTelemetry& rx = a.rx;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    {   // a tinted panel behind the two status lines
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    // ---- line 1: lamps, lock state and signal mode
    {
        int iq = run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0;
        lamp("IQ", iq, (int)Ic::Wave); ImGui::SameLine(0, 12 * gUi);
        const double frameS = rx.frameMs > 0 ? rx.frameMs / 1e3 : 0.5;
        const bool isT = rx.standard == 1;
        const double okFrac = (rx.blocksOk + rx.blocksBad) ? (double)rx.blocksOk / (rx.blocksOk + rx.blocksBad) : 0;
        const int fec = !run || !rx.plpValid || rx.plpFrames == 0 ? 0 : okFrac > 0.995 ? 1 : okFrac > 0.5 ? 2 : 3;
        if (rx.standard == 2) {
            const AtscTelemetry& at = rx.atsc;
            const double okF = (at.rsClean + at.rsCorrected + at.rsFailed) ? (double)(at.rsClean + at.rsCorrected) / (double)(at.rsClean + at.rsCorrected + at.rsFailed) : 0;
            lamp("Pilot", !run ? 0 : at.pilot ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Seg", !run ? 0 : at.segSync ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Field", !run ? 0 : at.fieldSync ? 1 : at.segSync ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Eq", !run ? 0 : at.eqTrained ? 1 : at.fieldSync ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Trellis", !run ? 0 : at.tsOk ? 1 : at.eqTrained ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("RS", !run || at.fields == 0 ? 0 : okF > 0.995 ? 1 : okF > 0.5 ? 2 : 3); ImGui::SameLine(0, 12 * gUi);
        } else if (isT) {
            // DVB-T: cyclic-prefix sync, guard interval, TPS signalling, channel estimate, then Viterbi and Reed-Solomon
            lamp("Sync", !run ? 0 : rx.state >= 1 ? 1 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("GI", !run ? 0 : rx.giIdx >= 0 ? (rx.state == 2 ? 1 : 2) : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("TPS", !run ? 0 : rx.dvbt.tpsOk ? 1 : rx.state == 1 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Chan", !run ? 0 : rx.chValid && rx.dataValid ? 1 : rx.dvbt.tpsOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("Viterbi", !run ? 0 : rx.dvbt.fecSync ? 1 : rx.dvbt.tpsOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
            lamp("RS", fec); ImGui::SameLine(0, 12 * gUi);
        } else {
        lamp("P1", !run || !a.rxSeen ? 0 : (rx.p1.valid && rx.secSinceP1 < std::max(1.0, 3 * frameS) ? 1 : 2)); ImGui::SameLine(0, 12 * gUi);
        lamp("GI", !run ? 0 : rx.state == 2 ? 1 : rx.state == 1 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("L1-pre", !run ? 0 : rx.l1preOk ? 1 : rx.l1preGood > 0 ? 2 : rx.chValid ? 3 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("L1-post", !run ? 0 : rx.l1postOk ? 1 : rx.l1postGood > 0 ? 2 : rx.l1preOk ? 3 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("Frame", !run ? 0 : rx.dataValid ? 1 : rx.l1preOk ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
        lamp("LDPC", fec); ImGui::SameLine(0, 12 * gUi);
        lamp("BCH", fec); ImGui::SameLine(0, 12 * gUi);
        }
        lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : (a.ts.ccErrors > 0 && a.bb.framesLost > 0.02 * (a.bb.frames + 1)) ? 2 : 1, (int)Ic::Layers); ImGui::SameLine(0, 12 * gUi);
        const PlayerStats ps = a.engine.player().stats();
        const bool pl = run && ps.active;
        lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); ImGui::SameLine(0, 12 * gUi);
        lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("|"); ImGui::SameLine(0, 10 * gUi);
    }
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        static const struct { const char* k; Ic ic; } kIcons[] = {
            {"State", Ic::Pulse}, {"Mode", Ic::Layers}, {"FFT", Ic::Grid}, {"GI", Ic::Echo}, {"Pilots", Ic::Target}, {"PLP", Ic::Layers},
            {"CFO", Ic::Wave}, {"SRO", Ic::Clock}, {"SNR", Ic::Signal}, {"MER", Ic::Target}, {"Delay", Ic::Echo}, {"fs", Ic::Gauge},
            {"dropped", Ic::Warning}, {"Channel", Ic::Antenna}, {"Field", Ic::Layers}, {"TPS", Ic::Info}, {"hier", Ic::Layers}, {"Pilot", Ic::Target}};
        ImGui::AlignTextToFramePadding();
        for (auto& e : kIcons) if (!strcmp(e.k, label)) {
            const bool isState = !strcmp(label, "State");
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            iconInline(e.ic, isState ? ImGui::ColorConvertFloat4ToU32(col) : iconDim(), 0.9f);
            ImGui::PopStyleVar();
            ImGui::SameLine(0, 4 * gUi);
            break;
        }
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (a.engine.radioLost()) ro("State", "radio disconnected", ImVec4(0.95f, 0.35f, 0.3f, 1));
    else if (rx.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (rx.state == 1) ro("State", "syncing", ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (run && rx.standard == 2 && rx.state >= 1) {
        ro("Mode", "ATSC 8-VSB");
        ro("Channel", "6 MHz");
        snprintf(b, sizeof b, "%d", rx.atsc.fieldParity); ro("Field", b);
        ro("Pilot", rx.atsc.pilot ? "locked" : "-");
    } else if (run && rx.standard == 1 && rx.state >= 1) {
        ro("Mode", "DVB-T");
        ro("FFT", rx.fftN == 8192 ? "8K" : "2K");
        ro("GI", dvbt::guardName(rx.giIdx));
        if (rx.dvbt.tpsOk) {
            snprintf(b, sizeof b, "%s %s", dvbt::modName(rx.dvbt.mod), dvbt::rateName(rx.dvbt.crHp));
            ro("TPS", b);
            if (rx.dvbt.hier) ro("hier", "yes", ImVec4(0.95f, 0.7f, 0.2f, 1));
        } else ro("TPS", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    } else if (run && rx.state == 2) {
        ro("Mode", s1Name(rx.p1.s1));
        const FftMode* fm = fftModeFromSize(rx.fftN);
        snprintf(b, sizeof b, "%s%s", fm ? fm->name : "?", rx.extCarriers ? " ext" : ""); ro("FFT", b);
        ro("GI", guardName(rx.giIdx));
        if (rx.l1preOk) { snprintf(b, sizeof b, "PP%d", rx.l1pre.pilotPattern + 1); ro("Pilots", b); }
        if (rx.plpValid) {
            static const char* mods[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"}; static const char* rates[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
            snprintf(b, sizeof b, "%s %s%s", rx.plpFec.mod >= 0 && rx.plpFec.mod < 4 ? mods[rx.plpFec.mod] : "?", rx.plpFec.rate >= 0 && rx.plpFec.rate < 8 ? rates[rx.plpFec.rate] : "?", rx.plpFec.rotation ? " rot" : "");
            ro("PLP", b);
        }
    }
    ImGui::NewLine();
    // ---- line 2: numbers and gauges
    if (run && (rx.state == 2 || (rx.standard == 1 && rx.state >= 1))) {
        snprintf(b, sizeof b, "%+.1f Hz", rx.cfoHz); ro("CFO", b);
        if (rx.standard != 1) { snprintf(b, sizeof b, "%+.1f ppm", rx.sroPpm); ro("SRO", b); }
        snprintf(b, sizeof b, "%.1f dB", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb); ro("SNR", b);
        if (rx.plpMerDb > 0 && rx.plpMerDb < 90) { snprintf(b, sizeof b, "%.1f dB", rx.plpMerDb); ro("MER", b); }
        if (!a.mpd.report().echoes.empty()) { snprintf(b, sizeof b, "%.2f us", std::fabs(a.mpd.report().echoes[0].delayUs)); ro("Delay", b); }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? IM_COL32(40, 112, 150, 255) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    ImGui::SameLine(0, 15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Quality"); ImGui::SameLine(0, 5 * gUi);
    {
        const QualityReport& q = a.quality.report();
        const float t = run && q.valid ? (float)q.percent / 100.f : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : IM_COL32(40, 112, 150, 255);
        snprintf(b, sizeof b, run && q.valid ? "%.0f%%  %s" : "-", q.percent, q.label.c_str());
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && run && q.valid) ImGui::SetTooltip("data SNR %.1f dB, needed about %.1f dB (margin %+.1f dB)\nFEC blocks decoded %.1f%%", q.snrDb, q.requiredDb, q.marginDb, q.fecOk * 100);
    }
    ImGui::SameLine(0, 15 * gUi);
    if (run) { snprintf(b, sizeof b, "%llu", (unsigned long long)a.engine.droppedSamples()); ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1)); }
    if (run && a.mpd.report().level != MultipathLevel::Unknown) {
        const MultipathReport& mr = a.mpd.report();
        const ImVec4 col = mr.level == MultipathLevel::None ? ImVec4(0.5f, 0.55f, 0.6f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                         : mr.level == MultipathLevel::Likely ? ImVec4(0.95f, 0.6f, 0.2f, 1) : ImVec4(0.95f, 0.35f, 0.25f, 1);
        ImGui::AlignTextToFramePadding();
        iconInline(Ic::Echo, ImGui::ColorConvertFloat4ToU32(col), 0.9f); ImGui::SameLine(0, 4 * gUi);
        ImGui::TextColored(col, "multipath: %s", multipathName(mr.level));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(mr.headline.c_str());
            for (auto& r : mr.reasons) ImGui::BulletText("%s", r.c_str());
            ImGui::EndTooltip();
        }
        ImGui::SameLine(0, 15 * gUi);
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (a.sweep.active()) ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "gain helper %d/%d ...", std::max(0, a.sweep.current()) + 1, (int)a.sweep.entries().size());
        else if (adc == AdcStatus::Overload) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)");
        else if (adc == AdcStatus::High) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level high");
        else if (adc == AdcStatus::Low) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain");
        else if (adc == AdcStatus::NoSignal) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?");
        else ImGui::NewLine();
    } else ImGui::NewLine();
}

void gainControl(App& a) {
    if (!a.engine.running() || a.devices[a.devIdx].kind == DeviceInfo::File) { a.sweep = GainSweep(); return; }
    const DeviceInfo& cd = a.devices[a.devIdx];
    const bool generic = cd.isGeneric();
    const int gmax = generic ? std::max(1, (int)cd.gainMaxDb) : 0;
    a.agc.setGenericMax(gmax);
    GainSetting g = generic ? genericGain((int)std::lround(a.tune.gainDb), gmax) : GainSetting{a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn};
    bool changed = false;
    const double now = ImGui::GetTime();
    if (a.sweep.active()) {
        GainSweep::Sample sm;
        sm.locked = a.rx.dataValid; sm.snrDb = a.rx.dataSnrDb;
        sm.clip = a.spec.stats.clipFraction; sm.rms = a.spec.stats.rmsDbfs; sm.peak = a.spec.stats.peak;
        changed = a.sweep.update(now, sm, g);
        if (!a.sweep.active()) a.engine.log("gain helper: " + a.sweep.summary());
    } else if (a.agcOn && a.dir.state() != DirectionFinder::State::Measuring) {   // the gain must stay put while a direction is measured
        changed = a.agc.update(now, a.spec.stats, g);
    }
    if (changed) {
        if (generic) a.tune.gainDb = g.vga; else { a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp; }
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.retune(a.tune);
        if (!a.sweep.active() || true) savePrefs(a);
        if (a.agcOn && !a.sweep.active() && generic) a.engine.log("AGC: gain " + std::to_string(g.vga) + " dB");
        else if (a.agcOn && !a.sweep.active()) a.engine.log("AGC: LNA " + std::to_string(g.lna) + " dB, VGA " + std::to_string(g.vga) + " dB, amp " + (g.amp ? "on" : "off"));
    }
}

// DVB <-> ATSC switch under the tuner settings
void standardSwitch(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the order on screen; the numbers are the families (0 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.0, 4 ISDB-T), which are also stored in the settings
    static const int order[5] = {0, 1, 3, 4, 2};
    static const char* names[5] = {"DVB", "ATSC", "ATSC 3.0", "ISDB-T", "DAB / DAB+"};
    static const ImU32 cols[5] = {IM_COL32(52, 92, 108, 255), IM_COL32(150, 100, 30, 255), IM_COL32(150, 70, 40, 255), IM_COL32(120, 70, 140, 255), IM_COL32(40, 130, 96, 255)};
    const float h = ImGui::GetFrameHeight() - 2;
    float segW[5], total = 0;
    for (int i = 0; i < 5; i++) { segW[i] = ImGui::CalcTextSize(names[i]).x + 22; total += segW[i]; }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), IM_COL32(18, 22, 28, 255), 3.f);
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), IM_COL32(52, 60, 72, 255), 3.f);
    float x = p.x, selX = p.x, selW = segW[0];
    int selPos = 0;
    for (int i = 0; i < 5; i++) { if (order[i] == a.family) { selX = x; selW = segW[i]; selPos = i; } x += segW[i]; }
    static float knobX = -1, knobW = 0;
    if (knobX < 0) { knobX = selX - p.x; knobW = selW; }
    knobX += (selX - p.x - knobX) * 0.35f; knobW += (selW - knobW) * 0.35f;
    dl->AddRectFilled(ImVec2(p.x + knobX + 2, p.y + 2), ImVec2(p.x + knobX + knobW - 2, p.y + h - 2), cols[selPos], 3.f);
    x = p.x;
    for (int i = 0; i < 5; i++) {
        const int fam = order[i];
        const char* nm = names[i];
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW[i], h))) {
            if (a.engine.running()) a.engine.log("stop the receiver before switching between DVB, ATSC, ATSC 3.0, ISDB-T and DAB");
            else if (fam != a.family) { setFamily(a, fam); savePrefs(a); }
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 ts = ImGui::CalcTextSize(nm);
        dl->AddText(ImVec2(x + (segW[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fam == a.family ? IM_COL32(255, 255, 255, 255) : hov ? IM_COL32(220, 228, 236, 255) : IM_COL32(140, 152, 166, 255), nm);
        x += segW[i];
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + total + 14, p.y));
    ImGui::AlignTextToFramePadding();
    if (a.family == 1) ImGui::TextDisabled("ATSC 8-VSB, 6 MHz channel (the DVB-only settings are off)");
    else if (a.family == 3) ImGui::TextDisabled("ATSC 3.0 (NextGen TV), 6 MHz channel, ROUTE services");
    else if (a.family == 4) ImGui::TextDisabled("ISDB-T (Japan, Brazil and most of South America), 6 MHz channel, 13 segments");
    else if (a.family == 2) ImGui::TextDisabled("DAB / DAB+ digital radio, Band III channels 5A to 13F");
    else ImGui::TextDisabled("DVB-T2 / DVB-T, detected automatically");
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y));
    if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + total, p.y + h))) ImGui::SetTooltip("DVB (T2 and T, automatic).\nATSC 1.0 (8-VSB: US, Canada, Mexico, South Korea).\nATSC 3.0 (NextGen TV).\nISDB-T (Japan, Brazil and most of South America).\nDAB / DAB+ digital radio.");
}

