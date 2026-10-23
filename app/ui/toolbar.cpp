// The top bar: toolbarParts() draws its groups (source, frequency, tuner, gain, start / stop, decoder options) for the
// classic bar and the side panels, and the tuning helpers (tuneFreq, spectrumTune, the second-mode control) live here.
// The source, rig windows, status bar and mode selection are in toolbar_source.cpp, toolbar_rig.cpp, statusbar.cpp and mode_select.cpp.
#include "../app.h"
#include "touch.h"
#include "../state/presets_app.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/engine/click_tune.h"
#include "dect2/source/antenna_ports.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>
#include "toolbar_internal.h"
#include "../gfx/tune_drag.h"

// the radio picker, the antenna port, rescan, the add-radio windows and the radio warnings
static void toolbarSource(App& a, bool vertical, bool running) {
    ImGui::TextDisabled("%s", tr("SRC")); ImGui::SameLine(0, 5 * gUi);
    // in the side panel the box takes the width left by the refresh button; a long name is cut short with "...", whole on hover
    const float srcW = vertical ? std::max(90.f * gUi, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().ItemSpacing.x) : 190 * gUi;
    const std::string srcName = ellipsize(a.devices[a.devIdx].name, srcW - ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().FramePadding.x);
    ImGui::SetNextItemWidth(srcW);
    ImGui::BeginDisabled(running);
    if (ImGui::BeginCombo("##src", srcName.c_str())) {
        for (int i = 0; i < (int)a.devices.size(); i++)
        {
            const int held = deviceHolder(a.devices[i]);   // a radio another receiver has open cannot be picked here
            const bool other = held != 0 && held != a.rxId;
            const std::string nm = a.devices[i].name + (other ? " (in use by " + deviceHolderText(held) + ")" : std::string());
            ImGui::BeginDisabled(other);
            if (ImGui::Selectable(nm.c_str(), i == a.devIdx) && i != a.devIdx) {
                a.devIdx = i;
                a.tune.biasTee = false;   // antenna power set for another radio (and its antenna) is never carried over
            }
            ImGui::EndDisabled();
            if (a.devices[i].isRadio() && ImGui::BeginPopupContextItem(("##hide" + std::to_string(i)).c_str())) {
                if (ImGui::MenuItem(tr("Hide this radio"))) {
                    auto h = hiddenRadios(); h.push_back(a.devices[i].name); setHiddenRadios(h);
                    a.hiddenRadios = h; a.devIdx = 0; a.devicesDirty = true;
                }
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();
        if (!a.hiddenRadios.empty() && ImGui::BeginMenu(tr("Hidden radios (click to show)"))) {
            for (size_t k = 0; k < a.hiddenRadios.size(); k++)
                if (ImGui::MenuItem(a.hiddenRadios[k].c_str())) {
                    a.hiddenRadios.erase(a.hiddenRadios.begin() + k); setHiddenRadios(a.hiddenRadios); a.devicesDirty = true;
                    break;
                }
            ImGui::EndMenu();
        }
        if (ImGui::Selectable(tr("Add network radio…"))) a.netAddOpen = true;
        {   // off by default (#215): only radios on USB are looked for; a network radio is added by its address
            bool net = networkDiscovery();
            if (ImGui::MenuItem(tr("Search the network for radios"), nullptr, &net)) { setNetworkDiscovery(net); a.devicesDirty = true; savePrefs(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Off: the list shows only radios on USB, and the network radios you added. On: SoapyRemote servers, PlutoSDRs\n(mDNS, pluto.local), network USRPs and other boards on your network are looked for and contacted when the list is built."));
        }
        if (ImGui::Selectable(tr("Group radios…"))) { a.groupOpen = true; a.groupEdit = -1; }
        if (hamlibSupported() && ImGui::Selectable(tr("Add Hamlib rig…"))) a.hamlibAddOpen = true;
        ImGui::EndCombo();
    }
    if (a.devicesDirty) { a.devicesDirty = false; refreshDevices(a); a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (a.devices[a.devIdx].isAudio())
            ImGui::SetTooltip("%s", tr("For WebSDR, a virtual audio cable (VAC), or a radio's speaker or line into the computer. Not a replacement for an SDR when you need megahertz of bandwidth."));
        else if (srcName != a.devices[a.devIdx].name)
            ImGui::SetTooltip("%s", a.devices[a.devIdx].name.c_str());
    }
    {   // the antenna port of a radio with several (one list entry each): the same switch as choosing the entry above
        const auto ports = antennaSiblings(a.devices, a.devIdx);
        if (!ports.empty()) {
            std::string cur;
            for (const auto& p : ports) if (p.devIdx == a.devIdx) cur = p.label;
            ImGui::SameLine(0, 5 * gUi);
            ImGui::SetNextItemWidth(80 * gUi);
            ImGui::BeginDisabled(running);
            if (ImGui::BeginCombo("##antport", cur.c_str())) {
                for (const auto& p : ports) {
                    if (ImGui::Selectable(p.label.c_str(), p.devIdx == a.devIdx) && p.devIdx != a.devIdx) { a.devIdx = p.devIdx; a.tune.biasTee = false; }
                    const std::string h = antennaPortHint(a.devices[(size_t)p.devIdx], p.label);
                    if (!h.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", h.c_str());
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            const std::string h = antennaPortHint(a.devices[(size_t)a.devIdx], cur);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(tr("Antenna port%s%s"), h.empty() ? "" : ": ", h.c_str());
        }
    }
    ImGui::SameLine();
    // not while a scan has the radio open: a radio in use may not be listed (or its limits not be readable), and the choice would move to another one
    ImGui::BeginDisabled(a.scanner.progress().running);
    if (iconFlat(Ic::Refresh, tr("Rescan for devices"))) refreshDevices(a), a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    radioMessagesUi(a, vertical);
    {   // a radio too slow for the mode: said here for every mode (the engine's log line was all there was)
        const std::string rw = rateWarning(a);
        if (!rw.empty() && vertical) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", rw.c_str()); ImGui::PopTextWrapPos(); }
        else if (!rw.empty()) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "(!)");
            if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420 * gUi); ImGui::TextWrapped("%s", rw.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        }
    }
    if (running && a.devices[a.devIdx].kind == DeviceInfo::Hamlib) {   // the CAT link of the rig is down: said here, the receiver keeps running
        const HamlibRigState hs = hamlibRigState();
        if (hs.active && !hs.connected) {
            const std::string msg = std::string(tr("Rig link lost, reconnecting:")) + " " + hs.status;
            if (vertical) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", msg.c_str()); ImGui::PopTextWrapPos(); }
            else {
                ImGui::SameLine(0, 6 * gUi);
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "(rig!)");
                if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420 * gUi); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            }
        }
    }
}

// the frequency box (typed or scrolled) and the sample-rate control beside it
static void toolbarFreq(App& a, bool vertical, bool& retune) {
    if (!vertical) vSeparator();
    ImGui::TextDisabled("%s", tr("FREQ"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.devices[a.devIdx].isAudio()
        ? tr("Dial frequency the decoder should assume (the sound card is not tuned)")
        : tr("Centre frequency"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble("##freq", &a.freqMhz, 0, 0, "%.3f MHz");
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && ImGui::GetIO().MouseWheel != 0) {   // scroll the frequency: 100 kHz a notch, Shift 1 MHz, Ctrl 1 kHz
        ImGuiIO& io = ImGui::GetIO();
        static float wheelAcc = 0;
        wheelAcc += io.MouseWheel;
        const int steps = (int)wheelAcc;
        if (steps != 0) {
            const double stepMhz = io.KeyShift ? 1.0 : io.KeyCtrl ? 0.001 : 0.1;
            a.freqMhz = std::max(0.0, a.freqMhz + steps * stepMhz);
            wheelAcc -= (float)steps;
            retune = true;
        }
        io.MouseWheel = 0;
    }
    if (!std::isfinite(a.freqMhz) || a.freqMhz < 0) a.freqMhz = 0;   // the only guard: no radio takes a negative frequency (any other value is passed on as typed)
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true;
    if (vertical) sampleRateControl(a); else { ImGui::SameLine(0, 10 * gUi); sampleRateControl(a); }   // the sample rate, next to the frequency
}

// the mode's own tuner controls, or the channel bandwidth / DAB channel / FM station box
static void toolbarTuner(App& a, int mask, bool vertical, bool& retune, const ModeUi* mu) {
    if (mu) { if (mu->tuner) { if (!vertical && (mask & TbFreq)) flowNext(10 * gUi); mu->tuner(a, retune); } } else {
    if (!vertical && (mask & TbFreq)) flowNext(10 * gUi);   // one bar: the groups wrap in a narrow window
    ImGui::TextDisabled("%s", tr(a.dabMode ? "CH" : a.fmMode ? "FM" : "BW"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(a.dabMode ? "DAB channel" : a.fmMode ? "FM station" : "Channel bandwidth"));
    ImGui::SameLine(0, 5 * gUi);
    if (a.dabMode) { if (dabChannelCombo(a)) retune = true; }
    else if (a.fmMode) { if (fmFrequencyCombo(a)) retune = true; }
    else {
    ImGui::SetNextItemWidth((a.isdbtMode ? 135 : a.bwAuto ? 125 : 80) * gUi);
    {
        char bl[32];
        snprintf(bl, sizeof bl, a.bwAuto ? "%s (auto)" : "%s", kBw[a.bwIdx].label);
        if (a.atscMode) snprintf(bl, sizeof bl, "6 MHz");
        if (a.isdbtMode) {   // ISDB-T: 6 MHz (Japan, the Americas), 7 or 8 MHz (the Maldives, ISDB-T International), or automatic
            const RxTelemetry& t = a.rx;
            if (a.isdbtBwMhz == 0 && a.engine.running() && t.standard == 5) snprintf(bl, sizeof bl, "%d MHz (auto)", t.isdbt.bwMhz);
            else snprintf(bl, sizeof bl, "%s", isdbtWidthLabel(a.isdbtBwMhz));
            if (ImGui::BeginCombo("##isdbtbw", bl)) {
                for (int w : {6, 7, 8}) if (ImGui::Selectable(isdbtWidthLabel(w), a.isdbtBwMhz == w)) setIsdbtBandwidth(a, w);
                ImGui::Separator();
                if (ImGui::Selectable(tr("Automatic"), a.isdbtBwMhz == 0)) setIsdbtBandwidth(a, 0);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ISDB-T channel width: 6 MHz in Japan, Brazil and the rest of the Americas, 8 MHz in the Maldives (ISDB-T International).\n"
                                                          "The same signal, every time scaled by the width. Automatic measures it in the spectrum and tries each width\nwhile nothing locks; it opens the radio at 10 Msps."));
        } else {
        ImGui::BeginDisabled(a.atscMode);
        if (ImGui::BeginCombo("##bw", bl)) {
            for (int i = 0; i < (int)(sizeof kBw / sizeof *kBw); i++)
                if (ImGui::Selectable(kBw[i].label, i == a.bwIdx && !a.bwAuto)) setDvbBandwidth(a, i);
            ImGui::Separator();
            if (ImGui::Selectable(tr("Automatic"), a.bwAuto)) setDvbBandwidth(a, -1);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Automatic: the width of the signal is measured in the spectrum and the channel bandwidth (5, 6, 7 or 8 MHz) is set for you."));
        }
        if (a.family != 7) { ImGui::SameLine(0, 6 * gUi); if (tvChannelCombo(a)) retune = true; }   // the TV channels of the band plan (a channel sets the frequency)
        // a radio too slow for the channel: next to the width as well (rateWarning: the 6 MHz of ATSC, ATSC 3.0 and ISDB-T, not the DVB width left in kBw)
        if (const std::string rw = rateWarning(a); !rw.empty()) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "(!)");
            if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420 * gUi); ImGui::TextWrapped("%s", rw.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        }
    }
    }
    }
}

// the gain sliders (LNA / VGA / amp, or one gain), AGC, Auto-tune, DC spike and IQ correction, bias-tee
static void toolbarGain(App& a, bool vertical, bool running, bool& retune) {
    const DeviceInfo& curDev = a.devices[a.devIdx];
    const bool isHw = curDev.isRadio();
    const bool generic = curDev.isGeneric();
    if (!a.newUi) vSeparator();   // the new interface starts the gain controls on a second row
    ImGui::BeginDisabled(!isHw);
    if (!vertical) { ImGui::TextDisabled("%s", tr("GAIN")); ImGui::SameLine(0, 5 * gUi); }
    if (generic) {
        // in the side panel the slider gets its own row (on the header's row it pushed AGC and Auto-tune past the panel edge)
        if (vertical) { ImGui::TextDisabled("%s", tr("Gain")); ImGui::SameLine(54 * gUi); } else ImGui::SameLine();
        ImGui::SetNextItemWidth(touch::on() ? -FLT_MIN : 150 * gUi);   // touch: the sheet's full width
        float gdb = (float)a.tune.gainDb;
        ImGui::SliderFloat("##gain", &gdb, (float)curDev.gainMinDb, (float)std::max(curDev.gainMaxDb, curDev.gainMinDb + 1.0), "%.0f dB");
        a.tune.gainDb = std::round(gdb);
        if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    } else {
    ImGui::TextDisabled("%s", tr("LNA"));
    ImGui::SameLine(vertical ? 54 * gUi : 0);
    ImGui::SetNextItemWidth(touch::on() ? -FLT_MIN : (vertical ? 150 : 78) * gUi);
    int lna = a.tune.lnaDb;
    ImGui::SliderInt("##lna", &lna, 0, 40, "%d dB");
    a.tune.lnaDb = (lna + 4) / 8 * 8; // hardware steps are 8 dB
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    if (!vertical) flowNext();
    ImGui::TextDisabled("%s", tr("VGA"));
    ImGui::SameLine(vertical ? 54 * gUi : 0);
    ImGui::SetNextItemWidth(touch::on() ? -FLT_MIN : (vertical ? 150 : 78) * gUi);
    int vga = a.tune.vgaDb;
    ImGui::SliderInt("##vga", &vga, 0, 62, "%d dB");
    a.tune.vgaDb = (vga + 1) / 2 * 2; // 2 dB steps
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    if (!vertical) flowNext();
    if (ImGui::Checkbox(tr("Amp"), &a.tune.ampOn)) retune = true, a.agcOn = false;
    if (a.tune.ampOn) { if (!vertical) flowNext(6 * gUi); agcAmpWarning(a); }   // beside a manual Amp switch that is on: the amp can be damaged by strong signals
    }
    if (!vertical) flowNext(); else if (!generic && !a.tune.ampOn) ImGui::SameLine();   // side panel, generic radio: AGC and Auto-tune on the row below the slider
    // Auto gain (docs/AUTOGAIN.md): Off / Auto / Optimise once / Safe start, its settings, and the live ADC meter strip under it
    autogainControl(a, vertical, running);
    ImGui::EndDisabled();
    if (isHw && running) {
        if (!vertical) flowNext(10 * gUi);
        autogainMeters(a);
    }
    if (!isHw && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Gain controls apply to radios only (not to a recording, a sound card, or the synthetic signal)"));
    if (isHw) {   // sample clean-up for zero-IF radios; applied as the samples come in, so it works while running
        if (!vertical) flowNext(14 * gUi);
        if (vertical) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2 * gUi));
        bool iq = a.engine.iqFix().iq;
        // the DC spike (the radio's own oscillator leaking in): left alone, removed, or kept off the channel by offset tuning
        int dcMode = a.engine.autoOffset() ? 2 : a.engine.iqFix().dc ? 1 : 0;
        static const char* kDcModes[] = {"DC spike: leave", "DC spike: remove", "DC spike: tune beside it"};
        ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(kDcModes[2]).x + ImGui::GetFrameHeight() + 12 * gUi, std::max(80.f, ImGui::GetContentRegionAvail().x)));
        if (ImGui::Combo("##dcmode", &dcMode, kDcModes, 3)) {
            const bool wasOffset = a.engine.autoOffset();
            a.engine.iqFix().dc = dcMode == 1;
            a.engine.setAutoOffset(dcMode == 2);
            savePrefs(a);
            if (wasOffset != (dcMode == 2) && a.engine.running()) startReceiver(a);   // offset tuning is set up when the radio opens
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The spike in the middle of the spectrum is the radio's own oscillator leaking in (strong on the HackRF and RTL-SDR).\n"
                                                      "Remove: filtered out of the samples (not in FM: a station tuned to the centre has its carrier there).\n"
                                                      "Tune beside it: the radio is tuned just beside the channel, so the spike falls outside it, and the channel is moved\n"
                                                      "back digitally. Needs a higher sample rate (DAB about 3.5 Msps); a radio too slow for it removes the spike instead.\n"
                                                      "A line that stays at a round frequency (e.g. 650.000 MHz) when you retune is interference from the radio's clock, not the DC spike."));
        if (a.engine.running() && a.engine.offsetHz() > 0) { ImGui::SameLine(); ImGui::TextDisabled(tr("%+.2f MHz"), -a.engine.offsetHz() / 1e6); }
        if (!vertical) flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("IQ correction"), &iq)) { a.engine.iqFix().iq = iq; savePrefs(a); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Corrects I/Q gain and phase imbalance, which puts a faint mirror image of every signal on the other side of the centre.\nMeasured from the signal itself; now %.2f dB, %.1f degrees. Not applied in DTMB and analog TV (their signals fool the measurement)."), a.engine.iqFix().gainDb(), a.engine.iqFix().phaseDeg());
        if (vertical) ImGui::PopStyleVar();
    }
    if (!curDev.hasBiasTee) a.tune.biasTee = false;   // a radio without antenna power never inherits the setting of another one
    if (const std::string bc = biasContext(a); bc != a.biasCtx) {   // another mode or radio (or the start): its own saved antenna power
        a.biasCtx = bc;
        const bool was = a.tune.biasTee;
        loadBiasTee(a);
        if (a.tune.biasTee != was && a.engine.running()) retune = true;
    }
    if (curDev.hasBiasTee) {
        if (!vertical) flowNext(14 * gUi);
        else ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2 * gUi));
        if (ImGui::Checkbox(tr("Bias-tee (antenna power)"), &a.tune.biasTee)) { retune = true; saveBiasTee(a); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Sends DC power up the antenna cable for an active antenna or LNA (e.g. a GNSS patch antenna). Leave it off with a passive antenna, and never with a DC short in the antenna path (some antennas, splitters or filters): it can damage the radio."));
        if ((a.family == 12 || a.family == 150) && !a.tune.biasTee) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Most GNSS antennas are active and need this on.")); ImGui::PopTextWrapPos(); }
        if (vertical) ImGui::PopStyleVar();
    }
    if (vertical) radioSettingsUi(a, true);   // radio_ui.cpp: the selected radio's own settings (frequency correction, notch filters, ...); a one-line bar has its button elsewhere
}

// the Start / Stop button of the classic interface
static void toolbarStartStop(App& a, bool running) {
    vSeparator();
    if (!running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.42f, 0.28f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.56f, 0.36f, 1));
        const bool startClicked = iconButton(Ic::Play, tr("Start"), IM_COL32(40, 70, 82, 255), IM_COL32(56, 94, 110, 255)) || a.wizStart;
        a.wizStart = false;
        ImGui::PopStyleColor(2);
        if (startClicked) startReceiver(a);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1));
        const bool stopClicked = iconButton(Ic::Stop, tr("Stop"), IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255));
        ImGui::PopStyleColor(2);
        if (stopClicked) a.engine.stop();
    }
    flowNext(8 * gUi);
    memoryToolbarButton(a);   // the quick list of the memories (app/memory)
    flowNext(8 * gUi);
    presetsapp::toolbarButton(a);   // per-mode presets (app/state/presets_app.cpp)
    memoryQuickBar(a);        // the starred memories in one row
}

// the LDPC decoder backend: CPU, GPU or Auto (DVB-T2 and ATSC 3.0)
static void toolbarCompute(App& a, bool vertical, bool running) {
    if (!vertical) vSeparator();
    {
        static const char* modes[] = {"CPU", "GPU", "Auto"};
        char lbl[48];
        const bool gpuNow = a.rx.plpOnGpu;
        snprintf(lbl, sizeof lbl, "%s", a.computeMode == 2 ? (gpuNow ? "Auto (GPU)" : "Auto (CPU)") : modes[a.computeMode]);
        ImGui::TextDisabled("%s", tr("COMPUTE")); ImGui::SameLine(0, 5 * gUi);
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
}

// the DVB standard to decode: Auto, DVB-T2 or DVB-T
static void toolbarStandard(App& a, bool vertical) {
        if (!vertical) vSeparator();
        static const char* names[] = {"Auto", "DVB-T2", "DVB-T"};
        char lbl[48];
        const int act = a.engine.activeStandard();
        snprintf(lbl, sizeof lbl, "%s", a.stdMode == 0 ? (a.rx.standard == 1 ? "Auto (DVB-T)" : "Auto (DVB-T2)") : names[a.stdMode]);
        (void)act;
        ImGui::TextDisabled("%s", tr("STD")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(112 * gUi);
        if (ImGui::BeginCombo("##std", lbl)) {
            for (int i = 0; i < 3; i++) if (a.family == 0 && ImGui::Selectable(names[i], a.stdMode == i)) { a.stdMode = i; a.engine.setStandard(i); savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Which broadcast standard to decode.\nAuto alternates between DVB-T2 and DVB-T until one locks."));
}


// ---- the baseband filter of the receiver families that have one (dsp/baseband_filter.h): a button and its pop-up, the same for every mode ----
namespace {
struct BbfDef { const char* key; double bw, trans, fs; };   // the mode's own bandwidth and transition (for the plot and the slider hints), a rate to draw at
const BbfDef kBbfDefs[] = {{"hfdl", 3400, 2000, 28800}, {"acars", 7200, 1500, 25000}, {"vdl2", 15000, 4000, 80000}, {"airband", 16000, 4000, 48000}, {"ais", 22500, 7500, 48000},
                           {"pager", 20000, 4000, 96000}, {"packet", 8000, 4000, 96000}, {"cw", 100, 80, 1000}, {"general", 3000, 600, 48000}, {"hfdig", 3000, 600, 48000}};
const char* const kBbfAll[] = {"hfdl", "acars", "vdl2", "airband", "ais", "pager", "packet", "cw", "general", "hfdig"};
void bbfSyncImpl() {   // once: the saved filters into the registry the modes read when they design theirs
    static bool done = false;
    if (done) return;
    done = true;
    for (const char* k : kBbfAll) dect2::bbf::set(k, dect2::bbf::fromString(plat::prefs().getS((std::string("bbf.") + k).c_str(), "")));
}
void bbfWidget(App& a, const char* key) {
    (void)a;
    const BbfDef* def = nullptr;
    for (const auto& d : kBbfDefs) if (!std::strcmp(d.key, key)) def = &d;
    if (!def) return;
    using namespace dect2;
    BasebandFilterCfg c = bbf::get(key);
    ImGui::TextDisabled("%s", tr("Baseband filter")); ImGui::SameLine();
    if (ImGui::SmallButton(c.enabled ? "custom##bbf" : "standard##bbf")) ImGui::OpenPopup("bbfpop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The channel filter of this mode: bandwidth, shape, transition width. Standard is the filter as shipped.\nA change reaches HFDL at once, the other modes the next time they start.");
    if (!ImGui::BeginPopup("bbfpop")) return;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Custom filter"), &c.enabled);
    if (c.enabled) {
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo("##bbfpre", "presets")) {
            for (const auto& p : bbf::presets()) if ((!std::strcmp(p.mode, key) || !*p.mode) && ImGui::Selectable(p.name)) { c = p.cfg; ch = true; }
            ImGui::EndCombo();
        }
        float bw = (float)c.bwHz, tr2 = (float)c.transHz, ce = (float)c.centreHz;
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::SliderFloat("##bbfbw", &bw, 0, (float)(def->bw * 3), bw > 0 ? "%.0f Hz wide" : "own width")) { c.bwHz = bw; ch = true; }
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::SliderFloat("##bbftr", &tr2, 0, (float)(def->trans * 3), tr2 > 0 ? "%.0f Hz transition" : "own transition")) { c.transHz = tr2; ch = true; }
        if (!std::strcmp(key, "hfdl")) {
            ImGui::SetNextItemWidth(170 * gUi);
            if (ImGui::SliderFloat("##bbfce", &ce, -1000, 1000, "%+.0f Hz centre")) { c.centreHz = ce; ch = true; }
        }
        int sh = (int)c.shape;
        static const char* shapes[] = {"Blackman", "Kaiser", "Blackman-Harris", "Hann", "Butterworth"};
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::Combo("##bbfsh", &sh, shapes, 5)) { c.shape = (BbShape)sh; ch = true; }
        const BbDesign d = bbf::design(c, def->bw, def->trans, def->fs, 5, 1001);
        float y[128];
        const double span = std::max(def->bw, c.bwHz) * 1.5;
        for (int i = 0; i < 128; i++) y[i] = (float)std::max(-80.0, bbf::responseDb(d, def->fs, -span + 2 * span * i / 127));
        ImGui::PlotLines("##bbfplot", y, 128, 0, "response, 0 .. -80 dB", -80.f, 5.f, ImVec2(240 * gUi, 70 * gUi));
    }
    if (ch) { bbf::set(key, c); plat::prefs().setS((std::string("bbf.") + key).c_str(), bbf::toString(c).c_str()); }
    ImGui::EndPopup();
}
} // namespace
void bbfSyncPrefs() { bbfSyncImpl(); }

void toolbarParts(App& a, int mask, bool vertical) {
    bool running = a.engine.running();
    bool retune = false;
    ImGui::AlignTextToFramePadding();
    if (!a.newUi) { ImGui::TextColored(pal::accent(), "%s", tr("ONAIR")); ImGui::SameLine(0, 16 * gUi); }
    if (mask & TbSource) toolbarSource(a, vertical, running);


    const DeviceInfo& curDev = a.devices[a.devIdx];
    setAdcBits(adcBitsFor(curDev, a.engine.running() ? a.engine.sampleRate() : a.tune.sampleRate));

    if (mask & TbFreq) toolbarFreq(a, vertical, retune);
    const ModeUi* mu = modeUi(a.family);
    if ((mask & TbTuner) && !(mask & TbFreq)) {   // the side panels: the frequency is in the top bar, the sample rate heads the tuner
        sampleRateControl(a);
        if (!vertical) flowNext(10 * gUi);
    }
    if (mask & TbTuner) toolbarTuner(a, mask, vertical, retune, mu);

    if (mask & TbTuner) secondModeControl(a, vertical);
    if (mask & TbTuner) { if (!vertical) flowNext(10 * gUi); freqdbKnownCombo(a, vertical ? -1.f : 170 * gUi); }

    if (mask & TbGain) toolbarGain(a, vertical, running, retune);
    if (a.newUi && (mask & TbGain)) presetsapp::toolbarButton(a);   // the classic bar has it beside the Start button

    if (!a.newUi) toolbarStartStop(a, running);   // the new interface has its Start button in the side rail

    if ((mask & TbDecoder) && (a.family == 0 || a.family == 3)) toolbarCompute(a, vertical, running);   // the LDPC decoder is used by DVB-T2 and ATSC 3.0 only
    if ((mask & TbDecoder) && a.family == 0) toolbarStandard(a, vertical);   // the other families have one standard each: nothing to choose

    if ((mask & TbDecoder) && mu && mu->decoder) {
        if (!vertical) vSeparator();
        mu->decoder(a, retune);
    }
    if (mask & TbDecoder) { bbfSyncImpl(); if (const char* bk = dect2::bbf::keyForFamily(a.family); *bk) { if (!vertical) vSeparator(); bbfWidget(a, bk); } }
    if (!vertical) flowEnd();

    if (running && a.devices[a.devIdx].kind == DeviceInfo::Hamlib) hamlibFollowRig(a);   // the rig's knob moves the frequency box
    else if (!running) a.hamlibRigSeq = 0;
    if (retune && running) {
        const tunedrag::TunerRetune rt = tunedrag::tunerRetune(a.tune.centerHz, a.engine.captureCentreHz(), a.engine.decodeShiftHz(), a.freqMhz * 1e6);
        if (rt.clearShift) a.engine.setDecodeShiftHz(0);   // a new frequency: the radio goes there, the old click shift no longer applies
        const bool moved = std::fabs(a.tune.centerHz - rt.dialHz) > 1;
        a.tune.centerHz = rt.dialHz;
        a.engine.log("retune " + std::to_string(a.freqMhz) + " MHz");
        if ((a.fmMode || a.family >= 6) && moved) a.engine.retuneReset(a.tune);   // forget the old station's name and flush its sound
        else a.engine.retune(a.tune);
        a.peak.clear();
        savePrefs(a);
    }

}

void toolbar(App& a) { toolbarParts(a, TbAll, false); }

static double dialMhzFromAbs(const App& a, double absMhz) {
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() &&
                      (a.devices[a.devIdx].kind == DeviceInfo::File || a.devices[a.devIdx].isAudio());
    return dect2::dialMhzFromClick(absMhz, mt && !file ? mt->tuneOffsetHz : 0.0);   // the axis is the real RF: the clicked signal is the dial (#219)
}

void tuneDecode(App& a, double absMhz) {
    if (a.second.drawing) {
        a.second.mhz = dialMhzFromAbs(a, absMhz);
        if (a.second.modeIdx >= 0) a.engine.setSecondaryHz(kModes[a.second.modeIdx].family + 2, absMhz * 1e6);
        return;
    }
    a.freqMhz = dialMhzFromAbs(a, absMhz);
    if (a.engine.running()) a.engine.setDecodeShiftHz(absMhz * 1e6 - a.engine.captureCentreHz());
    savePrefs(a);
}

double settingsRetuneCentreHz(const App& a) {
    return a.engine.running() && a.engine.decodeShiftHz() != 0 ? a.tune.centerHz : a.freqMhz * 1e6;
}

void spectrumTune(App& a, double absMhz) {
    if (a.tuningMode == 1) {
        if (a.fmMode) { a.freqMhz = std::round(std::max(0.0, dialMhzFromAbs(a, absMhz)) * 10.0) / 10.0; tuneDecode(a, absMhz); }
        else tuneDecode(a, absMhz);
    } else if (a.fmMode) fmTune(a, absMhz);
    else {
        // normal tuning (default): a click inside the captured band only moves the marker; the target button centres the radio on the click
        const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && (a.devices[a.devIdx].kind == DeviceInfo::File || a.devices[a.devIdx].isAudio());
        const double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        if (a.engine.running() && !file && !a.second.drawing && a.family != 55 &&
            !tunedrag::clickPlan(a.centreTuning, false, absMhz, a.engine.captureCentreHz() / 1e6, fs, 0.001, dect2::kMaxTuneMhz).retune) tuneDecode(a, absMhz);
        else tuneFreq(a, dialMhzFromAbs(a, absMhz));
    }
}

void tuneFreq(App& a, double mhz) {
    mhz = std::min(dect2::kMaxTuneMhz, std::max(0.001, mhz));
    if (a.second.drawing) {   // the second mode's screens tune the second mode (a refusal turns it off: secondModeTick)
        a.second.mhz = mhz;
        if (a.second.modeIdx >= 0) a.engine.setSecondaryHz(kModes[a.second.modeIdx].family + 2, mhz * 1e6);
        return;
    }
    a.engine.setDecodeShiftHz(0);
    const bool moved = std::fabs(a.tune.centerHz - mhz * 1e6) > 1;
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    if (a.engine.running()) {
        if ((a.fmMode || a.family >= 6) && moved) a.engine.retuneReset(a.tune);   // forget the old station's name and flush its sound
        else a.engine.retune(a.tune);
        a.peak.clear();
    }
    savePrefs(a);
}

// The tuner panel's "Second mode": a second receiver decoding from the same samples (Engine::setSecondary); its screen is the tab
// "Second: <mode>". The pop-up says at once whether the pair fits in the band the radio captures.
void secondModeControl(App& a, bool vertical) {
    if (!vertical) flowNext(10 * gUi);
    char lbl[128];
    if (a.second.modeIdx >= 0) snprintf(lbl, sizeof lbl, "Second: %s %.3f MHz###second", kModes[a.second.modeIdx].name, a.second.mhz);
    else snprintf(lbl, sizeof lbl, "Second mode###second");
    if (ImGui::SmallButton(lbl)) ImGui::OpenPopup("##secondmode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode a second mode from the same radio samples, in its own tab.\nIts frequency must lie inside the band the radio captures.");
    if (!ImGui::BeginPopup("##secondmode")) return;
    static int pick = -1;
    static double mhz = 0;
    if (ImGui::IsWindowAppearing() && a.second.modeIdx >= 0) { pick = a.second.modeIdx; mhz = a.second.mhz; }
    const float wrap = 380 * gUi;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextDisabled("A second receiver decodes from the same samples as the first mode; its sound stays off unless chosen.");
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("Mode##second", pick >= 0 ? kModes[pick].name : "choose a mode")) {
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.family < 6 || m.family == a.family || !Engine::secondaryAllowed(m.family + 2)) continue;
            if (ImGui::Selectable(m.name, i == pick)) {
                pick = i;
                const SubMode* sm = m.subId ? subModeById(m.subId) : nullptr;
                const ModeTuning* mt = modeTuning(m.family + 2);
                mhz = sm ? sm->defMhz : mt ? mt->defMhz : a.freqMhz;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputDouble("MHz##second", &mhz, 0, 0, "%.4f");
    if (ImGui::Checkbox("Play its sound", &a.second.audio)) a.engine.setSecondarySound(a.second.audio);
    if (pick >= 0) {   // the same check the engine makes, with what the radio runs at (or will)
        const bool run = a.engine.running();
        const ModeTuning* pt = modeTuning(engineStd(a));
        const double centre = run ? a.engine.captureCentreHz() : a.tune.centerHz + (pt && a.devices[a.devIdx].kind != DeviceInfo::File ? pt->tuneOffsetHz : 0);
        const double rate = run ? a.engine.sampleRate() : a.tune.sampleRate;
        const std::string why = Engine::secondaryCheck(engineStd(a), kModes[pick].family + 2, centre, rate, mhz * 1e6);
        if (!why.empty()) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.3f, 1), "%s", why.c_str());
        else ImGui::TextDisabled("Fits: %.3f MHz from the centre of the %.2f Msps capture", (mhz * 1e6 - centre) / 1e6, rate / 1e6);
    }
    ImGui::BeginDisabled(pick < 0);
    if (ImGui::Button("Run")) setSecondMode(a, pick, mhz);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(a.second.modeIdx < 0);
    if (ImGui::Button("Off")) { setSecondMode(a, -1, 0); pick = -1; }
    ImGui::EndDisabled();
    const std::string note = a.engine.secondaryNote();
    if (!note.empty()) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.3f, 1), "%s", note.c_str());
    if (a.engine.secondaryRunning())
        ImGui::TextDisabled("Running at %.2f Msps; %.2f s of signal lost", a.engine.secondaryRate() / 1e6, a.engine.secondaryDropped() / std::max(1.0, a.engine.sampleRate()));
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}
