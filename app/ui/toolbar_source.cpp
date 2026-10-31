// The source side of the top bar: starting the receiver, the sample-rate control, the DVB bandwidth,
// the ADC-bits table and the options of the selected source (test signal, IQ file, sound card, Hamlib rig).
// Split from toolbar.cpp; shared declarations are in app.h and toolbar_internal.h.
#include "../app.h"
#include "../gfx/file_vfo.h"
#include "ui_common.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/antenna_ports.h"
#include "dect2/source/audio_if.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <thread>
#include "toolbar_internal.h"

// The converter resolution at the rate in use, for the ADC level advice and the AGC (gain.h): the 8-bit HackRF and RTL-SDR, the 12-bit
// Airspy, LimeSDR, PlutoSDR, bladeRF, USRP and RFNM, the 14-bit Fobos SDR, the 16-bit HF+ path; the SDRplay ADC gives up bits as its rate rises (API specification:
// 14 bits to 6.048 MHz, 12 to 8.064, 10 to 9.216, 8 above). Unknown radios, files and the test signal count as 8 bits (as before).
int adcBitsFor(const DeviceInfo& d, double rateHz) {
    if (d.kind != DeviceInfo::Native) return 8;
    if (d.board == "sdrplay") return rateHz <= 6.048e6 ? 14 : rateHz <= 8.064e6 ? 12 : rateHz <= 9.216e6 ? 10 : 8;
    if (d.board == "airspy" || d.board == "lime" || d.board == "pluto" || d.board == "bladerf" || d.board == "usrp" || d.board == "rfnm") return 12;
    if (d.board == "airspyhf") return 16;
    if (d.board == "fobos") return 14;   // (the driver scales its samples to full scale 1.0)
    if (d.board == "icr8600") return 16;   // the IC-R8600 I/Q output is 16 bit (the driver scales to full scale 1.0)
    if (d.board == "rx888") return 16;   // the RX888 MkII's converter (the driver scales to full scale 1.0)
    return 8;   // rtlsdr and anything new
}

// The selected radio's sample rate - the one in use while it runs, else the most it lists - when it is below what the mode's receiver works
// with (minSampleRateFor, the limit the engine logs at start, which only reached the log). Empty when the rate is fine or not known: a HackRF
// gives every rate the modes ask for, a SoapySDR radio lists its rates once it has been opened.
// A .sigmf-meta / .sigmf-data / two-channel .wav says its own rate, centre frequency, sample format and where the samples start.
static void applyIqFileInfo(App& a, const std::string& path) {
    a.file.offsetBytes = 0; a.file.endBytes = 0; a.file.bigEndian = false; a.file.centreHz = 0;
    IqFileInfo info; std::string err;
    if (!probeIqFile(path, info, err)) { if (!err.empty()) { a.engine.log(err); a.file.path.clear(); } return; }
    a.file.path = info.dataPath; a.file.format = info.format; a.file.bigEndian = info.bigEndian;
    a.file.offsetBytes = info.offsetBytes; a.file.endBytes = info.endBytes;
    if (info.sampleRate > 0) a.file.sampleRate = info.sampleRate;
    const double nameCentre = info.centerHz > 0 ? 0.0 : filevfo::guessCentreHz(path);   // a header without a frequency: the name may carry it
    if (info.centerHz > 0) a.freqMhz = info.centerHz / 1e6;
    else if (nameCentre > 0) a.freqMhz = nameCentre / 1e6;
    a.engine.log("IQ file: " + std::to_string(info.sampleRate / 1e6) + " Msps, " + std::to_string(info.centerHz / 1e6) + " MHz" + (info.description.empty() ? "" : ", " + info.description));
}

std::string rateWarning(const App& a) {
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!d.isRadio()) return "";
    const bool run = a.engine.running();
    const double rate = run ? a.engine.sampleRate() : d.isGeneric() ? d.maxRateHz : 0;
    const double bw = a.family == 4 ? (a.isdbtBwMhz ? a.isdbtBwMhz : 6) : run && a.family == 0 && a.engine.activeBandwidth() > 0 ? a.engine.activeBandwidth() : kBw[a.bwIdx].mhz;
    double need = minSampleRateFor(engineStd(a), bw);
    if (engineStd(a) == 23) need = hdrMinSampleRate(a.freqMhz);   // HD Radio AM works from 200 ksps
    if (a.family == 8 && (a.atvFm || a.atvWide) && !a.atvNbtv) need = std::max(need, 16e6);   // FM video: a link is about 17 MHz wide; system E 14 MHz
    // a PlutoSDR on its USB cable run above what the cable carries (linkRateFor): why it may break up, and the fix
    if (run && linkCapHz(d, a.tune) > 0 && rate >= need - 1) return linkNote(d, a.tune, rate);
    if (rate <= 0 || need <= 0 || rate >= need - 1) return "";
    char what[64];
    if (a.family == 0) snprintf(what, sizeof what, "the %g MHz DVB-T2 / DVB-T channel", bw);
    else { snprintf(what, sizeof what, "this mode"); for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) snprintf(what, sizeof what, "%s", kModes[i].name); }
    char b[360];
    snprintf(b, sizeof b, "This radio %s %.3g Msps; %s needs at least %.3g Msps, so %s. %s", run ? "runs at" : "gives at most", rate / 1e6, what, need / 1e6,
             a.family == 0 ? "only the spectrum is shown" : "the receiver cannot work with it",
             a.family == 0 && bw > 1.7 ? "Choose a narrower channel (for example 1.7 MHz DVB-T2-Lite) or a faster radio." : "It needs a faster radio.");
    return b;
}

void startReceiver(App& a) {
    if (std::string why; receiverModeBusy(a, &why)) { a.engine.log(why); return; }   // two receivers in one mode share screen state (docs/multi-sdr.md)
    // a running scan has the radio: the receiver opening it too would take it away from the scan (SDRplay) or fail
    if (a.scanner.progress().running && a.devices[a.devIdx].isRadio()) { a.engine.log("a scan is using the radio: stop the scan first"); return; }
    const bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    a.tune.centerHz = a.freqMhz * 1e6;
    fileStartPrep(a);   // a recording keeps its centre; the frequency box is the tune box (VFO)
    applyBandwidth(a);
    if (isFile && a.file.offsetBytes == 0) { const double r = guessSampleRate(a.file.path); if (r > 0) a.file.sampleRate = r; } // the name says the rate (a SigMF / WAV file says it itself)
    if (isFile && a.file.path.empty()) a.engine.log("choose an IQ file first");
    else {
        if (a.devices[a.devIdx].isAudio() || a.devices[a.devIdx].kind == DeviceInfo::Hamlib) applyAudioSettings(a);
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[a.devIdx], a.tune, a.file);
        savePrefs(a);
        a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
    }
}

// A channel width picked while the receiver runs: the receiver follows it, as followBandwidth() does,
// instead of the engine staying on the width it had found while the toolbar shows the new one
static void bandwidthNow(App& a) {
    const double oldRate = a.tune.sampleRate;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    a.engine.setBandwidthAuto(false);
    if (a.devices[a.devIdx].isRadio() && a.tune.sampleRate != oldRate) startReceiver(a);   // 8 vs 10 Msps: the radio is opened again
    else a.engine.setBandwidth(a.tune.bandwidthMhz);
}

// ---- the sample rate of the radio for this mode: Auto (the mode's own rate), one of the rates that suit the radio and the mode, or a typed one
static std::string mspsText(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.3f", hz / 1e6);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s + " Msps";
}
static void manualRatePopup(App& a, const char* id, bool open, const RateLimits& L, double need);
void chooseSampleRate(App& a, double hz) {
    const double prev = savedSampleRate(a);
    saveSampleRate(a, hz);
    a.rateRefusal.clear();
    const bool restart = a.engine.running() && (a.devices[a.devIdx].isRadio() || a.devices[a.devIdx].kind == DeviceInfo::Synthetic);
    const double old = a.tune.sampleRate;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    if (restart && a.tune.sampleRate != old) {   // the radio is opened again at the new rate, as after a change of mode
        a.engine.log("restarting at " + mspsText(a.tune.sampleRate));
        startReceiver(a);
        a.mpd.reset(); a.quality.reset();
        if (!a.engine.running() && hz > 0) {   // 3.4: the driver has the last word: its own message, and the previous setting is back
            const std::string why = a.engine.startError();
            a.rateRefusal = mspsText(hz) + " refused by the radio" + (why.empty() ? std::string() : ": " + why) + "; back to the previous setting";
            a.engine.log(a.rateRefusal);
            saveSampleRate(a, prev);
            applyBandwidth(a);
            startReceiver(a);
        }
    }
}
bool lowerSampleRate(App& a, double& prevChoiceHz, double& newHz) {
    if (!a.engine.running() || a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) return false;
    const DeviceInfo& d = a.devices[a.devIdx];
    const double cur = a.engine.radioRate();
    double best = 0;
    for (const RateEntry& e : rateEntries(rateLimitsOf(d), modeMinSampleRate(a))) if (e.getHz < cur - 1 && e.getHz > best) best = e.getHz;
    if (best <= 0) return false;
    prevChoiceHz = a.chosenRateHz; newHz = best;
    chooseSampleRate(a, best);
    return true;
}
void restoreSampleRate(App& a, double choiceHz) { chooseSampleRate(a, choiceHz); }

void sampleRateControl(App& a) {
    const DeviceInfo& d = a.devices[a.devIdx];
    const bool run = a.engine.running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("RATE"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sample rate of the radio for this mode"));
    ImGui::SameLine(0, 5 * gUi);
    if (d.kind == DeviceInfo::File) {   // the file's own rate (set in the source options); nothing to choose
        ImGui::TextUnformatted((mspsText(run ? a.engine.radioRate() : a.file.sampleRate) + " (file)").c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A recording plays at the rate it was made at: set it with the file."));
        return;
    }
    if (d.isAudio() || d.kind == DeviceInfo::Hamlib) {
        const double hz = run ? a.engine.radioRate() : 48000.0;
        char b[64];
        snprintf(b, sizeof b, "%.0f Hz (%s)", hz, d.kind == DeviceInfo::Hamlib ? tr("Hamlib + audio") : tr("sound card"));
        ImGui::TextUnformatted(b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.kind == DeviceInfo::Hamlib
            ? tr("Hamlib follows the rig's frequency and mode both ways; IQ or audio is captured at the sound card's rate.")
            : tr("The capture rate of the sound card. The engine runs at this rate; there is no radio to tune."));
        return;
    }
    if (!run && a.rateCtx != rateContext(a)) applyBandwidth(a);   // another mode, radio or channel width: its own Auto and its own choice
    // one width wherever the box lands: sized from the space left on the line, it was wider on a line of its own, so in a wrapping
    // row it moved between the lines every frame
    const float w = std::min(150.f * gUi, std::max(110.f * gUi, ImGui::GetWindowWidth() - 2 * ImGui::GetStyle().WindowPadding.x));
    ImGui::SetNextItemWidth(w);
    const bool radio = d.isRadio() || d.kind == DeviceInfo::Synthetic;   // the test signal follows a chosen rate too
    const RateLimits L = rateLimitsOf(d);
    const double need = modeMinSampleRate(a);
    const std::string preview = a.chosenRateHz > 0 ? mspsText(a.chosenRateHz) : std::string(tr("Auto")) + " (" + mspsText(a.autoRateHz > 0 ? a.autoRateHz : a.tune.sampleRate) + ")";
    bool openManual = false;
    ImGui::BeginDisabled(!radio || a.scanner.progress().running);
    if (ImGui::BeginCombo("##srate", preview.c_str())) {
        if (ImGui::Selectable((std::string(tr("Auto")) + " (" + mspsText(a.autoRateHz) + ")").c_str(), a.chosenRateHz == 0) && a.chosenRateHz != 0) chooseSampleRate(a, 0);
        for (const RateEntry& e : rateEntries(L, 0)) {   // (3.4: a rate below the mode's need is listed too: it only warns)
            std::string label = mspsText(e.getHz);
            if (std::fabs(e.askHz - e.getHz) > 1) label += " (for " + mspsText(e.askHz) + ")";
            const bool sel = a.chosenRateHz > 0 && std::fabs(deliveredRate(L, a.chosenRateHz) - e.getHz) <= 1;
            if (ImGui::Selectable(label.c_str(), sel) && !sel) chooseSampleRate(a, e.getHz);
        }
        ImGui::Separator();
        if (ImGui::Selectable(tr("Manual…"))) openManual = true;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420 * gUi);
        if (!radio) ImGui::TextWrapped(tr("The test signal is made at the mode's own rate (%s)."), mspsText(a.tune.sampleRate).c_str());
        else if (a.scanner.progress().running) ImGui::TextWrapped("%s", tr("A scan is using the radio and sets its own rate."));
        else {
            ImGui::TextWrapped("%s", tr("Sample rate of the radio for this mode, remembered for this radio. Auto is the mode's own rate."));
            if (need > 0) ImGui::TextWrapped(tr("This mode needs at least %s."), mspsText(need).c_str());
            if (L.minHz > 0 || L.maxHz > 0) ImGui::TextWrapped(tr("This radio: %s to %s."), L.minHz > 0 ? mspsText(L.minHz).c_str() : "?", L.maxHz > 0 ? mspsText(L.maxHz).c_str() : "?");
            ImGui::TextWrapped("%s", tr("A higher rate shows more spectrum but needs more USB bandwidth and processor time: when the status bar reports samples lost on the radio side, choose a lower rate."));
            if (run && a.engine.radioRate() > 0) ImGui::TextWrapped(tr("The radio runs at %s."), mspsText(a.engine.radioRate()).c_str());
        }
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    // 3.5: the rate the radio really runs at (what its driver confirmed), not the rate after OnAir's own resampling: a chosen rate that is not
    // what the radio runs (a driver that rounds, offset tuning that runs the radio faster, "Decimate in OnAir") is said, with the reason
    if (run && radio && a.tune.sampleRate > 0 && a.engine.radioRate() > 0 && std::fabs(a.engine.radioRate() - a.tune.sampleRate) > a.tune.sampleRate * 1e-3) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), tr("radio runs at %s"), mspsText(a.engine.radioRate()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("The radio is running at %s; %s was asked for. The driver rounded the rate, or OnAir runs the radio faster on purpose (offset tuning away from the DC spike, Decimate in OnAir) and brings it back to the asked rate itself."), mspsText(a.engine.radioRate()).c_str(), mspsText(a.tune.sampleRate).c_str());
    }
    if (!a.rateRefusal.empty()) {   // the radio refused what was typed: its own message, the previous rate is back
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", tr("refused"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.rateRefusal.c_str());
    } else if (a.chosenRateHz > 0 && radio) {   // a chosen rate outside the radio's list or below the decoder's need: warned, never blocked
        const RateCheck wc = checkManualRate(L, need, a.chosenRateHz);
        if (!wc.why.empty()) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "!");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wc.why.c_str());
        }
    }
    manualRatePopup(a, "##srateman", openManual, L, need);
}

// The popup that takes a typed rate in Msps. Any positive rate is used: a rate the radio does not list only gets a warning, and the driver's
// own message is shown if it refuses (chooseSampleRate puts the previous setting back).
static void manualRatePopup(App& a, const char* id, bool open, const RateLimits& L, double need) {
    static double typed = 0;
    static std::string why;
    if (open) { typed = (a.chosenRateHz > 0 ? a.chosenRateHz : a.tune.sampleRate) / 1e6; why.clear(); ImGui::OpenPopup(id); }
    if (ImGui::BeginPopup(id)) {
        ImGui::TextDisabled("%s", tr("Sample rate (Msps)"));
        ImGui::SetNextItemWidth(120 * gUi);
        if (open) ImGui::SetKeyboardFocusHere();
        ImGui::InputDouble("##sratev", &typed, 0, 0, "%.3f");   // InputScalar does not take EnterReturnsTrue (an assertion in Windows builds)
        const bool enter = ImGui::IsItemDeactivatedAfterEdit() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        ImGui::SameLine();
        if (ImGui::Button(tr("Use")) || enter) {
            const double hz = std::round(typed * 1e3) * 1e3;
            const RateCheck c = checkManualRate(L, need, hz);
            if (c.ok) { chooseSampleRate(a, hz); ImGui::CloseCurrentPopup(); }   // any positive rate goes to the driver; a warning is shown beside the control
            else why = c.why;
        }
        if (!why.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", why.c_str());
        else if (typed > 0) { const RateCheck c = checkManualRate(L, need, std::round(typed * 1e3) * 1e3); if (!c.why.empty()) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "%s", c.why.c_str()); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// The sample-rate half of the Advanced row (rx_width.cpp): the mode's standard rate as text and a Custom tick that opens the list of rates the radio
// lists and a free entry. The choice is the same one the RATE box of the top bar makes (saved per mode and radio).
bool sampleRateAdvanced(App& a) {
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!(d.isRadio() || d.kind == DeviceInfo::Synthetic)) return false;   // a recording, a sound card: their own rate
    const RateLimits L = rateLimitsOf(d);
    const double need = modeMinSampleRate(a);
    static std::map<std::string, double> lastCustom;   // the custom rate of a mode while its tick is off
    const std::string lk = rateModeId(a) + "|" + std::to_string(a.devIdx);
    bool custom = a.chosenRateHz > 0, openManual = false, changed = false;
    flowNext(14 * gUi);
    char sb[96];
    snprintf(sb, sizeof sb, "%s: %s (%s)", tr("Sample rate"), mspsText(a.autoRateHz > 0 ? a.autoRateHz : a.tune.sampleRate).c_str(), tr("standard"));
    ImGui::TextDisabled("%s", custom ? tr("Sample rate") : sb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The radio's sample rate for this mode, remembered for this radio. The standard rate is the mode's own; tick Custom to choose any rate: the ones the radio lists, or a typed one that goes to the driver as it is. A rate the radio does not list, or below what the decoder needs, only gets a warning."));
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("Custom##srcustom"), &custom)) {
        changed = true;
        if (custom) chooseSampleRate(a, lastCustom.count(lk) ? lastCustom[lk] : (a.autoRateHz > 0 ? a.autoRateHz : a.tune.sampleRate));
        else { lastCustom[lk] = a.chosenRateHz; chooseSampleRate(a, 0); }
    }
    if (custom) {
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::BeginCombo("##srateadv", mspsText(a.chosenRateHz).c_str())) {
            for (const RateEntry& e : rateEntries(L, 0)) {   // all the radio's rates, none hidden for being below the mode's need (that only warns)
                std::string label = mspsText(e.getHz);
                if (std::fabs(e.askHz - e.getHz) > 1) label += " (for " + mspsText(e.askHz) + ")";
                const bool sel = std::fabs(deliveredRate(L, a.chosenRateHz) - e.getHz) <= 1;
                if (ImGui::Selectable(label.c_str(), sel) && !sel) { chooseSampleRate(a, e.getHz); changed = true; }
            }
            ImGui::Separator();
            if (ImGui::Selectable(tr("Other…"))) openManual = true;
            ImGui::EndCombo();
        }
        const RateCheck wc = checkManualRate(L, need, a.chosenRateHz);
        if (!wc.why.empty()) {
            ImGui::SameLine(0, 6 * gUi);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), "!");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wc.why.c_str());
        }
    }
    if (!a.rateRefusal.empty()) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", tr("refused"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.rateRefusal.c_str());
    }
    manualRatePopup(a, "##srateman2", openManual, L, need);
    return changed;
}


void setDvbBandwidth(App& a, int idx) {
    if (idx < 0) a.bwAuto = true;   // followBandwidth() hands the detection back to the engine
    else { a.bwIdx = idx; a.bwAuto = false; if (a.engine.running()) bandwidthNow(a); }
    savePrefs(a);
}

bool sourceOptionsOpen(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return false;
    const DeviceInfo::Kind k = a.devices[a.devIdx].kind;
    return k == DeviceInfo::Synthetic || (k == DeviceInfo::File && !a.engine.running()) || k == DeviceInfo::Audio || k == DeviceInfo::Hamlib;
}

// the synthetic generator: noise, offset and signal-shape controls (per mode)
static void synthSourceOptions(App& a, bool running) {
    SynthConfig& sc = a.tune.synth;
    bool ch = false;
    ImGui::TextDisabled("%s", tr("synthetic"));
    flowNext();   // the options wrap onto more lines in a narrow window
    if (const ModeUi* mu = modeUi(a.family)) {   // a mode added after FM: its own test signal and options
        bool c2 = false;
        if (mu->synth) mu->synth(a, c2);
        else {   // the mode has no options of its own: noise and carrier offset, which every test signal follows
            ImGui::TextDisabled(tr("%s test signal"), curSub(a) ? curSub(a)->name : modeTuning(a.family + 2)->name);
            flowNext(14 * gUi); ImGui::TextDisabled("%s", tr("SNR")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
            float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##ssnr", &snr, 5, 45, "%.0f dB")) { sc.snrDb = snr; c2 = true; }
            flowNext(); ImGui::TextDisabled("%s", tr("CFO")); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
            float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##scfo", &cfo, -20, 20, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; c2 = true; }
        }
        flowEnd();
        if (c2 && running) a.engine.retune(a.tune);
        return;
    }
    if (a.family >= 2 && a.family <= 5) {   // DAB, ATSC 3.0, ISDB-T and FM: fixed signals, with noise and carrier offset to play with
        ImGui::TextDisabled(a.family == 2 ? "DAB+ ensemble OnAir DAB, 4 services (tones and a melody)" : a.family == 3 ? "ATSC 3.0 test card, QPSK 8/15" : a.family == 4 ? "ISDB-T mode 3, test programme" : "FM stereo with RDS, 1 kHz left and 3 kHz right");
        flowNext(14 * gUi); ImGui::TextDisabled("%s", tr("SNR")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##ssnr", &snr, 5, 45, "%.0f dB")) { sc.snrDb = snr; ch = true; }
        flowNext(); ImGui::TextDisabled("%s", tr("CFO")); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
        float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##scfo", &cfo, -20, 20, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; ch = true; }
        flowEnd();
        if (ch && running) a.engine.retune(a.tune);
        return;
    }
    if (a.family == 0) { if (ImGui::Checkbox(tr("DVB-T"), &sc.dvbt)) ch = true; flowNext(); }
    if (a.family == 1) {
        ImGui::TextDisabled("%s", tr("ATSC 8-VSB, 6 MHz"));
    } else if (a.family == 2) {
        ImGui::TextDisabled("%s", tr("DAB ensemble, transmission mode I"));
    } else if (sc.dvbt) {
        static const char* fftN[] = {"2K", "8K"}; static const char* gis[] = {"1/32", "1/16", "1/8", "1/4"};
        static const char* mods[] = {"QPSK", "16-QAM", "64-QAM"}; static const char* rates[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
        ImGui::SetNextItemWidth(60 * gUi);
        if (ImGui::BeginCombo("##tfft", fftN[sc.dvbtMode & 1])) { for (int i = 0; i < 2; i++) if (ImGui::Selectable(fftN[i], i == sc.dvbtMode)) { sc.dvbtMode = i; ch = true; } ImGui::EndCombo(); }
        flowNext(); ImGui::TextDisabled("%s", tr("GI")); ImGui::SameLine(); ImGui::SetNextItemWidth(66 * gUi);
        if (ImGui::BeginCombo("##tgi", gis[sc.dvbtGuard & 3])) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(gis[i], i == sc.dvbtGuard)) { sc.dvbtGuard = i; ch = true; } ImGui::EndCombo(); }
        flowNext(); ImGui::SetNextItemWidth(84 * gUi);
        if (ImGui::BeginCombo("##tmod", mods[sc.dvbtMod % 3])) { for (int i = 0; i < 3; i++) if (ImGui::Selectable(mods[i], i == sc.dvbtMod)) { sc.dvbtMod = i; ch = true; } ImGui::EndCombo(); }
        flowNext(); ImGui::SetNextItemWidth(60 * gUi);
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
    flowNext(); ImGui::TextDisabled("%s", tr("GI")); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::BeginCombo("##sgi", guardName(sc.tx.giIdx))) {
        for (int g = 0; g < kNumGi; g++) if (ImGui::Selectable(guardName(g), g == sc.tx.giIdx)) { sc.tx.giIdx = g; ch = true; }
        ImGui::EndCombo();
    }
    flowNext();
    if (ImGui::Checkbox(tr("ext"), &sc.tx.ext)) ch = true;
    flowNext(); ImGui::SetNextItemWidth(62 * gUi);
    {
        char ppl[8]; snprintf(ppl, sizeof ppl, "PP%d", sc.tx.pp + 1);
        if (ImGui::BeginCombo("##spp", ppl)) {
            for (int q = 0; q < 8; q++) { snprintf(ppl, sizeof ppl, "PP%d", q + 1); if (ImGui::Selectable(ppl, q == sc.tx.pp)) { sc.tx.pp = q; ch = true; } }
            ImGui::EndCombo();
        }
    }
    flowNext();
    if (ImGui::Checkbox(tr("TR"), &sc.tx.tr)) ch = true;
    }
    flowNext(a.family == 1 || a.family == 2 ? 14 * gUi : -1); ImGui::TextDisabled("%s", tr("SNR")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##ssnr", &snr, 0, 40, "%.0f dB")) { sc.snrDb = snr; ch = true; }
    flowNext(); ImGui::TextDisabled("%s", tr("CFO")); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##scfo", &cfo, -40, 40, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; ch = true; }
    flowNext(); ImGui::TextDisabled("%s", tr("SRO")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float sro = (float)sc.sroPpm; if (ImGui::SliderFloat("##ssro", &sro, -50, 50, "%.0f ppm")) { sc.sroPpm = sro; ch = true; }
    flowNext(); ImGui::TextDisabled("%s", tr("echo")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float ec = (float)sc.echoDb; if (ImGui::SliderFloat("##sec", &ec, 0, 20, ec == 0 ? "off" : "-%.0f dB")) { sc.echoDb = ec; ch = true; }
    flowEnd();
    if (ch && running) a.engine.retune(a.tune);
}

// "File info": the recording's true centre frequency, rate and format, for a file whose header lacks them (a raw .cs8 / .cu8 / .cf32 has none), with
// auto-detect from the SigMF / BLUE / WAV header or the name. The tune box moves across the whole recorded span around this centre (file_vfo.h).
static void fileInfoPopup(App& a) {
    if (ImGui::SmallButton(tr("File info"))) { a.fileInfoOpen = true; ImGui::OpenPopup("##fileinfo"); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The centre frequency and the rate of the recording. The spectrum stays on the centre; the tune box moves across the recorded band."));
    if (!ImGui::BeginPopup("##fileinfo")) { a.fileInfoOpen = false; return; }
    static double centre = 0, msps = 0;
    static bool primed = false;
    if (a.fileInfoOpen) { centre = fileCentreMhz(a); msps = a.file.sampleRate / 1e6; a.fileInfoOpen = false; primed = true; }
    if (!primed) { centre = fileCentreMhz(a); msps = a.file.sampleRate / 1e6; primed = true; }
    ImGui::TextDisabled("%s", tr("Centre frequency (MHz)"));
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputDouble("##ficentre", &centre, 0, 0, "%.6f");
    if (ImGui::IsItemDeactivatedAfterEdit() && centre > 0) fileSetCentre(a, centre);
    ImGui::TextDisabled("%s", tr("Sample rate (Msps)"));
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputDouble("##firate", &msps, 0, 0, "%.4f");
    const char* fm[] = {"cs8", "cu8", "cf32", "ci16", "cu16"};
    int fi = (int)a.file.format;
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::Combo("##fiformat", &fi, fm, 5)) a.file.format = (FileFormat)fi;
    if (ImGui::Button(tr("Auto-detect"))) {
        const std::string p = a.file.path;
        a.file.format = guessFormat(p);
        const double r = guessSampleRate(p); if (r > 0) a.file.sampleRate = r;
        const double c = filevfo::guessCentreHz(p);
        applyIqFileInfo(a, p);   // the header, when there is one, wins over the name
        if (a.file.centreHz <= 0 && c > 0 && !a.engine.running()) a.freqMhz = c / 1e6;
        if (c > 0 && a.engine.running()) fileSetCentre(a, c);
        centre = fileCentreMhz(a); msps = a.file.sampleRate / 1e6;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Reads the rate, format and centre from the SigMF / BLUE / WAV header, else from the file name (for example 100.5MHz_2Msps)."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Apply"))) {
        if (msps > 0) a.file.sampleRate = msps * 1e6;
        if (centre > 0) fileSetCentre(a, centre);
        if (a.engine.running()) startReceiver(a);   // the rate and the format are read when the file is opened
        ImGui::CloseCurrentPopup();
    }
    bool nt = a.engine.dcNotch();
    if (ImGui::Checkbox(tr("Ignore DC spike"), &nt)) { a.engine.setDcNotch(nt); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A notch at the recording's centre in front of the demodulator: the SDR's own DC spike / LO leakage does not reach the signal. The spectrum still shows the spike."));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// an IQ file: Open, the shown path, sample format, rate and loop
static void fileSourceOptions(App& a) {
    ImGui::TextDisabled("%s", tr("file"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Open…"))) { auto p = openFileDialog(); if (!p.empty()) { a.file.path = p; a.file.format = guessFormat(p); const double r = guessSampleRate(p); if (r > 0) a.file.sampleRate = r; applyIqFileInfo(a, p); } }
    ImGui::SameLine();
    {   // a long path keeps its end (the file name) in a narrow window; the whole of it on hover
        std::string shown = a.file.path.empty() ? "(none)" : a.file.path;
        const float room = std::max(80.f * gUi, ImGui::GetContentRegionAvail().x - 8 * gUi);
        while (shown.size() > 4 && ImGui::CalcTextSize(shown.c_str()).x > room) shown = "..." + shown.substr(std::min(shown.size(), (size_t)4));
        ImGui::TextUnformatted(shown.c_str());
        if (shown != a.file.path && !a.file.path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.file.path.c_str());
    }
    flowNext();
    ImGui::SetNextItemWidth(70 * gUi);
    const char* fm[] = {"cs8", "cu8", "cf32", "ci16", "cu16"};
    int fi = (int)a.file.format;
    if (ImGui::Combo("##ff", &fi, fm, 5)) a.file.format = (FileFormat)fi;
    flowNext();
    ImGui::TextDisabled("%s", tr("rate"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    double msps = a.file.sampleRate / 1e6;
    if (ImGui::InputDouble("##frate", &msps, 0, 0, "%.4f Msps")) a.file.sampleRate = msps * 1e6;
    flowNext();
    ImGui::Checkbox(tr("loop"), &a.file.loop);
    flowNext();
    fileInfoPopup(a);
    flowEnd();
}

// the "Hamlib CAT" label with its help tooltip
static void hamlibSourceNote() {
    ImGui::TextDisabled("%s", tr("Hamlib CAT"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        wrapTooltip(380 * gUi, tr("Frequency and mode go to the rig over the serial port. Set a sound card id when adding the rig for IQ or a speaker tap; leave it empty for CAT-only (silence IQ)."));
    }
    flowEnd();
}

// The sound-card settings into the tune settings the source reads (keys: audio_if.h)
void applyAudioSettings(App& a) {
    auto& r = a.tune.radio;
    r["audio_iq"] = std::to_string(a.audioIqMode);
    r["audio_chan"] = std::to_string(a.audioChan);
    r["audio_if_hz"] = std::to_string(a.audioIfHz);
    r["audio_invert"] = a.audioInvert ? "1" : "0";
    r["audio_dc"] = a.audioDc ? "1" : "0";
    r["audio_gain_db"] = std::to_string(a.audioGainDb);
    r["audio_rate"] = std::to_string(a.audioRate);
    a.engine.setSpectrumShiftHz(-audioViewOffsetHz(a));   // the audio-range view moves only the picture
}

double audioViewOffsetHz(const App& a) {
    if (a.devices.empty() || a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isAudio() || !a.audioAfView) return 0;
    TuneSettings t;
    auto& r = t.radio;
    r["audio_iq"] = std::to_string(a.audioIqMode);
    r["audio_if_hz"] = std::to_string(a.audioIfHz);
    r["audio_invert"] = a.audioInvert ? "1" : "0";
    r["audio_rate"] = std::to_string(a.audioRate);
    return audioAfViewOffsetHz(audioIfConfigOf(t));
}

// a sound-card source: the I/Q mode (stereo IQ, USB, LSB, real mono) with its help, then the IF / AF options
static void audioSourceOptions(App& a, bool running) {
    ImGui::TextDisabled("%s", tr("sound card"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        wrapTooltip(380 * gUi, tr("For WebSDR, a virtual audio cable (VAC), or a radio's speaker, line or IF output into the computer. Not a replacement for an SDR when you need megahertz of bandwidth. The frequency field is the dial the decoder should assume (the radio frequency of the IF centre); the sound card is not tuned."));
    }
    auto changed = [&] { applyAudioSettings(a); savePrefs(a); if (running) a.engine.retune(a.tune); };
    flowNext();
    ImGui::SetNextItemWidth(130 * gUi);
    const char* am[] = {tr("I/Q stereo"), tr("USB audio"), tr("LSB audio"), tr("Real (mono)")};
    if (ImGui::Combo("##aiq", &a.audioIqMode, am, 4)) changed();
    if (ImGui::IsItemHovered()) {
        wrapTooltip(380 * gUi, tr("I/Q stereo: left is I, right is Q (HDSDR / WebSDR VAC). USB audio: real audio to analytic IQ for FT8, RTTY, CW, SSB from a speaker tap or a receiver's audio. LSB audio: the same, mirrored. Real (mono): real samples (an AF tap, an IF such as 12 kHz) made analytic, with the sideband switch below."));
    }
    if (a.audioIqMode != 0) {   // a real signal: which channel
        flowNext();
        ImGui::SetNextItemWidth(110 * gUi);
        const char* ch[] = {tr("Left"), tr("Right"), tr("Left + right")};
        if (ImGui::Combo("##achan", &a.audioChan, ch, 3)) changed();
    }
    flowNext();
    ImGui::TextDisabled("%s", tr("IF centre"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95 * gUi);
    if (ImGui::InputDouble("##aif", &a.audioIfHz, 0, 0, "%.0f Hz")) { a.audioIfHz = std::max(-96000.0, std::min(96000.0, a.audioIfHz)); changed(); }
    if (ImGui::IsItemHovered()) {
        wrapTooltip(380 * gUi, tr("The audio frequency of the signal's centre in the sound card, for example 12000 for a radio's 12 kHz IF output, or 0 for plain audio. It is shifted down to 0 Hz, so the decoder sees the signal at the dial frequency above."));
    }
    if (a.audioIqMode != 0) {   // the audio range across the screen, and the IF centre found from the signal
        flowNext();
        if (ImGui::Checkbox(tr("Audio range"), &a.audioAfView)) changed();
        if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Show the sound card's whole audio range, 0 Hz to half the sample rate, across the spectrum, with the frequency scale in audio Hz from the dial. Off: the IF centre in the middle (the decoders always get it there)."));
        if (running) {
            flowNext();
            if (ImGui::SmallButton(tr("Find centre"))) {
                TuneSettings t;
                t.radio["audio_iq"] = std::to_string(a.audioIqMode);
                t.radio["audio_if_hz"] = std::to_string(a.audioIfHz);
                t.radio["audio_invert"] = a.audioInvert ? "1" : "0";
                t.radio["audio_rate"] = std::to_string(a.audioRate);
                const AudioIfConfig cfg = audioIfConfigOf(t);
                double c = 0, w = 0;
                if (!a.spec.dbfs.empty() && audioDetectCentre(a.spec.dbfs.data(), (int)a.spec.dbfs.size(), a.engine.sampleRate(), audioViewOffsetHz(a), cfg, c, w)) {
                    a.audioIfHz = std::floor(c + 0.5);
                    changed();
                    a.engine.log("sound card: signal " + std::to_string((int)w) + " Hz wide, centre at " + std::to_string((int)a.audioIfHz) + " Hz");
                } else a.engine.log("sound card: no signal stands out of the noise");
            }
            if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Sets the IF centre to the middle of the strongest signal on the screen (a fax or FSK signal: halfway between its tones)."));
        }
    }
    flowNext();
    if (ImGui::Checkbox(tr("Swap sideband"), &a.audioInvert)) changed();
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Mirror the spectrum: for a receiver whose output is the other sideband."));
    flowNext();
    if (ImGui::Checkbox(tr("Remove DC"), &a.audioDc)) changed();
    flowNext();
    ImGui::SetNextItemWidth(90 * gUi);
    float g = (float)a.audioGainDb;
    if (ImGui::SliderFloat("##again", &g, -20.f, 40.f, "%.0f dB")) { a.audioGainDb = g; changed(); }
    flowNext();
    int n = 0;
    const int* rates = audioRateChoices(n);
    int cur = 0;
    for (int i = 0; i < n; i++) if (rates[i] == a.audioRate) cur = i;
    char label[24];
    snprintf(label, sizeof label, "%g kHz", rates[cur] / 1000.0);
    ImGui::SetNextItemWidth(95 * gUi);
    if (ImGui::BeginCombo("##arate", label)) {
        for (int i = 0; i < n; i++) {
            char l[24];
            snprintf(l, sizeof l, "%g kHz", rates[i] / 1000.0);
            if (ImGui::Selectable(l, i == cur)) { a.audioRate = rates[i]; applyAudioSettings(a); savePrefs(a); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("The sound card's sample rate, 8 to 192 kHz. A wider rate shows more of an IF. It applies when the receiver is started."));
    if (running) {   // the input level: the bar fills to full scale, red when it clips
        flowNext();
        const float lv = audioSourceLevel();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, lv > 0.95f ? ImVec4(0.9f, 0.25f, 0.2f, 1) : ImVec4(0.3f, 0.7f, 0.4f, 1));
        ImGui::ProgressBar(std::min(1.f, lv), ImVec2(70 * gUi, 0), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (audioSourceClipping()) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1), "%s", tr("Clipping: lower the input level"));
        else ImGui::TextDisabled("%s", tr("Level"));
        if (a.audioIqMode == 0 && audioSourceLayoutHint() == 1) {   // the same signal on both channels is audio, not an I/Q pair
            flowNext();
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.3f, 1), "%s", tr("Left and right are identical: this is audio, not I/Q."));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Use Real (mono)"))) { a.audioIqMode = 3; a.audioChan = 2; changed(); }
        }
    }
    flowEnd();
}

void sourceOptions(App& a) {
    bool running = a.engine.running();
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    const bool isAudio = a.devices[a.devIdx].isAudio();
    const bool isHamlib = a.devices[a.devIdx].kind == DeviceInfo::Hamlib;
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic) synthSourceOptions(a, running);
    if (isFile && !running) fileSourceOptions(a);
    else if (isFile) { ImGui::TextDisabled("%s", tr("file")); ImGui::SameLine(); fileInfoPopup(a); flowEnd(); }
    if (isHamlib) hamlibSourceNote();
    if (isAudio || isHamlib) audioSourceOptions(a, running);
}
