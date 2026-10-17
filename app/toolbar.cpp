// the top bar, the source options, the status bar, gain control and the standard switch
#include "app.h"
#include "dect2/rate_choice.h"
#include "dect2/antenna_ports.h"
#include "dect2/hdr_rx.h"
#include "dect2/hamlib_link.h"
#include "dect2/iq_file.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>

// The converter resolution at the rate in use, for the ADC level advice and the AGC (gain.h): the 8-bit HackRF and RTL-SDR, the 12-bit
// Airspy, LimeSDR, PlutoSDR, bladeRF, USRP and RFNM, the 14-bit Fobos SDR, the 16-bit HF+ path; the SDRplay ADC gives up bits as its rate rises (API specification:
// 14 bits to 6.048 MHz, 12 to 8.064, 10 to 9.216, 8 above). Unknown radios, files and the test signal count as 8 bits (as before).
int adcBitsFor(const DeviceInfo& d, double rateHz) {
    if (d.kind != DeviceInfo::Native) return 8;
    if (d.board == "sdrplay") return rateHz <= 6.048e6 ? 14 : rateHz <= 8.064e6 ? 12 : rateHz <= 9.216e6 ? 10 : 8;
    if (d.board == "airspy" || d.board == "lime" || d.board == "pluto" || d.board == "bladerf" || d.board == "usrp" || d.board == "rfnm") return 12;
    if (d.board == "airspyhf") return 16;
    if (d.board == "fobos") return 14;   // (the driver scales its samples to full scale 1.0)
    if (d.board == "rx888") return 16;   // the RX888 MkII's converter (the driver scales to full scale 1.0)
    return 8;   // rtlsdr and anything new
}

// The selected radio's sample rate - the one in use while it runs, else the most it lists - when it is below what the mode's receiver works
// with (minSampleRateFor, the limit the engine logs at start, which only reached the log). Empty when the rate is fine or not known: a HackRF
// gives every rate the modes ask for, a SoapySDR radio lists its rates once it has been opened.
// A .sigmf-meta / .sigmf-data / two-channel .wav says its own rate, centre frequency, sample format and where the samples start.
static void applyIqFileInfo(App& a, const std::string& path) {
    a.file.offsetBytes = 0; a.file.endBytes = 0; a.file.bigEndian = false;
    IqFileInfo info; std::string err;
    if (!probeIqFile(path, info, err)) { if (!err.empty()) { a.engine.log(err); a.file.path.clear(); } return; }
    a.file.path = info.dataPath; a.file.format = info.format; a.file.bigEndian = info.bigEndian;
    a.file.offsetBytes = info.offsetBytes; a.file.endBytes = info.endBytes;
    if (info.sampleRate > 0) a.file.sampleRate = info.sampleRate;
    if (info.centerHz > 0) a.freqMhz = info.centerHz / 1e6;
    a.engine.log("IQ file: " + std::to_string(info.sampleRate / 1e6) + " Msps, " + std::to_string(info.centerHz / 1e6) + " MHz" + (info.description.empty() ? "" : ", " + info.description));
}

static std::string rateWarning(const App& a) {
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
    applyBandwidth(a);
    if (isFile && a.file.offsetBytes == 0) { const double r = guessSampleRate(a.file.path); if (r > 0) a.file.sampleRate = r; } // the name says the rate (a SigMF / WAV file says it itself)
    if (isFile && a.file.path.empty()) a.engine.log("choose an IQ file first");
    else {
        if (a.devices[a.devIdx].isAudio() || a.devices[a.devIdx].kind == DeviceInfo::Hamlib) a.tune.radio["audio_iq"] = std::to_string(a.audioIqMode);
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
static void chooseSampleRate(App& a, double hz) {
    saveSampleRate(a, hz);
    const bool restart = a.engine.running() && a.devices[a.devIdx].isRadio();
    const double old = a.tune.sampleRate;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    if (restart && a.tune.sampleRate != old) {   // the radio is opened again at the new rate, as after a change of mode
        a.engine.log("restarting at " + mspsText(a.tune.sampleRate));
        startReceiver(a);
        a.mpd.reset(); a.quality.reset();
    }
}
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
    const bool radio = d.isRadio();
    const RateLimits L = rateLimitsOf(d);
    const double need = modeMinSampleRate(a);
    const std::string preview = a.chosenRateHz > 0 ? mspsText(a.chosenRateHz) : std::string(tr("Auto")) + " (" + mspsText(a.autoRateHz > 0 ? a.autoRateHz : a.tune.sampleRate) + ")";
    bool openManual = false;
    ImGui::BeginDisabled(!radio || a.scanner.progress().running);
    if (ImGui::BeginCombo("##srate", preview.c_str())) {
        if (ImGui::Selectable((std::string(tr("Auto")) + " (" + mspsText(a.autoRateHz) + ")").c_str(), a.chosenRateHz == 0) && a.chosenRateHz != 0) chooseSampleRate(a, 0);
        for (const RateEntry& e : rateEntries(L, need)) {
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
    if (run && radio && a.tune.sampleRate > 0 && std::fabs(a.engine.sampleRate() - a.tune.sampleRate) > a.tune.sampleRate * 1e-3) {   // the radio rounded it
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.28f, 1), tr("runs at %s"), mspsText(a.engine.sampleRate()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("The radio gave %s instead of the %s asked for."), mspsText(a.engine.sampleRate()).c_str(), mspsText(a.tune.sampleRate).c_str());
    }
    static double typed = 0;
    static std::string why;
    if (openManual) { typed = (a.chosenRateHz > 0 ? a.chosenRateHz : a.tune.sampleRate) / 1e6; why.clear(); ImGui::OpenPopup("##srateman"); }
    if (ImGui::BeginPopup("##srateman")) {
        ImGui::TextDisabled("%s", tr("Sample rate (Msps)"));
        ImGui::SetNextItemWidth(120 * gUi);
        if (openManual) ImGui::SetKeyboardFocusHere();
        ImGui::InputDouble("##sratev", &typed, 0, 0, "%.3f");   // InputScalar does not take EnterReturnsTrue (an assertion in Windows builds)
        const bool enter = ImGui::IsItemDeactivatedAfterEdit() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        ImGui::SameLine();
        if (ImGui::Button(tr("Use")) || enter) {
            const RateCheck c = checkManualRate(L, need, std::round(typed * 1e3) * 1e3);
            if (c.ok) { chooseSampleRate(a, std::round(typed * 1e3) * 1e3); ImGui::CloseCurrentPopup(); }
            else why = c.why;
        }
        if (!why.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", why.c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void setDvbBandwidth(App& a, int idx) {
    if (idx < 0) a.bwAuto = true;   // followBandwidth() hands the detection back to the engine
    else { a.bwIdx = idx; a.bwAuto = false; if (a.engine.running()) bandwidthNow(a); }
    savePrefs(a);
}

// ---- radios on the network (issues #28, #54, #68): a PlutoSDR by IP, an rtl_tcp server, a SoapyRemote server, a SpyServer, an SDR++ server, a Hermes Lite 2

static bool netTypeSearches(int t) { return t == NetRadio::HermesLite2 || t == NetRadio::RedPitaya; }   // found by a discovery broadcast

// looks for Hermes Lite 2 (or Red Pitaya) radios, by the type chosen, on a worker thread (the broadcast waits 1.5 s for answers); netRadioWindow() takes the result
static void startNetDiscovery(App& a) {
    if (a.netDiscovery) return;
    auto d = std::make_shared<App::NetDiscovery>();
    a.netDiscovery = d;
    a.netFound.clear();
    a.netSearched = false;
    const bool rp = a.netAddType == NetRadio::RedPitaya;
    std::thread([d, rp] { d->found = rp ? discoverRedPitaya(1500) : discoverHermesLite2(1500); d->done = true; }).detach();
}

void netRadioWindow(App& a) {
    // a connection test that finished: add the radio and pick it, or say why not
    if (a.netAddProbe && a.netAddProbe->done) {
        auto pr = a.netAddProbe;
        a.netAddProbe.reset();
        if (pr->ok) {
            if (std::find(a.netRadios.begin(), a.netRadios.end(), pr->radio) == a.netRadios.end()) a.netRadios.push_back(pr->radio);
            savePrefs(a);
            if (!a.engine.running()) {
                refreshDevices(a);
                const std::string name = netRadioDeviceName(pr->radio);
                for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].name == name) { a.devIdx = i; a.tune.biasTee = false; break; }
            }
            a.netAddErr.clear();
            a.netAddOpen = false;
        } else a.netAddErr = pr->err;
    }
    // a search that finished: the address of the first radio found (one not in use by another program first), unless one was typed
    if (a.netDiscovery && a.netDiscovery->done) {
        a.netFound = a.netDiscovery->found;
        a.netDiscovery.reset();
        a.netSearched = true;
        std::stable_sort(a.netFound.begin(), a.netFound.end(), [](const FoundNetRadio& x, const FoundNetRadio& y) { return !x.busy && y.busy; });
        if (netTypeSearches(a.netAddType) && !a.netFound.empty() && a.netAddHost[0] == 0) {
            snprintf(a.netAddHost, sizeof a.netAddHost, "%s", a.netFound[0].host.c_str());
            a.netAddPort = netRadioDefaultPort((NetRadio::Type)a.netAddType);
        }
    }
    if (!a.netAddOpen) return;
    if (netTypeSearches(a.netAddType) && !a.netSearched && !a.netDiscovery) startNetDiscovery(a);   // the window opened with that type
    ImGui::SetNextWindowSize(ImVec2(420 * gUi, 0), ImGuiCond_Appearing);
    if (a.setupOpen) {   // opened from the setup wizard: in front of it (the wizard's panel is centred and does not move)
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        // not while a popup is open: focusing this window closes every popup over it, its own Type list included
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(trw("Network radios"), &a.netAddOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    const bool busy = a.netAddProbe != nullptr;
    ImGui::BeginDisabled(busy);
    static const char* kTypes[] = {"PlutoSDR (by IP)", "rtl_tcp", "SoapyRemote", "SpyServer", "Hermes Lite 2", "Red Pitaya", "SDR++ server"};
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::Combo("Type", &a.netAddType, kTypes, IM_ARRAYSIZE(kTypes))) {
        const auto t = (NetRadio::Type)a.netAddType;
        a.netAddPort = netRadioDefaultPort(t);
        const std::string h = a.netAddHost;
        const bool found = std::any_of(a.netFound.begin(), a.netFound.end(), [&](const FoundNetRadio& f) { return f.host == h; });
        if (h.empty() || h == netRadioDefaultHost(NetRadio::Pluto) || found) snprintf(a.netAddHost, sizeof a.netAddHost, "%s", netRadioDefaultHost(t));
        a.netAddErr.clear();
        if (netTypeSearches(t)) { a.netAddHost[0] = 0; startNetDiscovery(a); }   // its address comes from the answers
    }
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint(tr("Address"), a.netAddType == NetRadio::Pluto ? "192.168.2.1" : netTypeSearches(a.netAddType) ? "192.168.1.20" : "192.168.1.20 or raspberrypi.local", a.netAddHost, sizeof a.netAddHost);
    if (netTypeSearches(a.netAddType)) {
        const bool rpType = a.netAddType == NetRadio::RedPitaya;
        ImGui::SameLine();
        ImGui::BeginDisabled(a.netDiscovery != nullptr);
        if (ImGui::Button(tr("Search"))) { a.netAddHost[0] = 0; startNetDiscovery(a); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", rpType ? tr("Look for Red Pitaya boards on this computer's networks again") : tr("Look for Hermes Lite 2 radios on this computer's networks again"));
        if (a.netDiscovery) ImGui::TextDisabled("%s", rpType ? tr("searching for Red Pitaya boards…") : tr("searching for Hermes Lite 2 radios…"));
        else if (a.netSearched && a.netFound.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", rpType ? tr("No Red Pitaya answered on this computer's networks (is its SDR transceiver app running?): type its address.") : tr("No Hermes Lite 2 answered on this computer's networks: type its address."));
        else if (a.netFound.size() > 1 || (a.netFound.size() == 1 && a.netFound[0].host != a.netAddHost)) {
            ImGui::TextDisabled("%s", tr("Found:"));
            for (size_t i = 0; i < a.netFound.size(); i++) {
                const auto& f = a.netFound[i];
                const std::string label = f.host + "  (" + f.detail + (f.busy ? ", in use" : "") + ")##hpsdr" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), f.host == a.netAddHost)) {
                    snprintf(a.netAddHost, sizeof a.netAddHost, "%s", f.host.c_str());
                    a.netAddPort = netRadioDefaultPort((NetRadio::Type)a.netAddType);
                }
            }
        } else if (a.netFound.size() == 1) {
            const auto& f = a.netFound[0];
            ImGui::TextDisabled(tr("Found %s (%s)"), f.host.c_str(), f.detail.c_str());
            if (f.busy) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("It is streaming to another program: stop it there first."));
        }
    }
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputInt(tr("Port"), &a.netAddPort, 0, 0);
    a.netAddPort = std::max(0, std::min(65535, a.netAddPort));
    if (a.netAddType == NetRadio::Pluto) {
        ImGui::SameLine(); ImGui::TextDisabled("%s", tr("0 = libiio's default"));
        ImGui::TextDisabled("%s", tr("Type the PlutoSDR's IP (USB gadget 192.168.2.1, or its Ethernet address) and Connect. It is then in the Radio list."));
    }
    if (a.netAddType == NetRadio::SdrppServer)
        ImGui::TextDisabled("%s", tr("SDR++ running as a server (sdrpp --server). It takes one client at a time: close SDR++'s own client first. "
                                     "Its source's settings (gain mode, bias tee, AGC, ...) show up under Radio settings once connected."));
    if (a.netAddType == NetRadio::SoapyRemote && !soapySupported()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("This build has no SoapySDR, which SoapyRemote needs."));
    if (a.netAddType == NetRadio::SoapyRemote) {
        ImGui::InputTextWithHint(tr("Remote driver"), "mirisdr  (empty: list what the server offers)", a.netAddRemote, sizeof a.netAddRemote);
        ImGui::TextDisabled("%s", tr("The radio's SoapySDR driver on the server, or a key=value list: driver=sdrplay,serial=1."));
    }
    if (ImGui::Button(tr("Connect"))) {
        NetRadio r;
        if (a.netAddType == NetRadio::SoapyRemote) r.remote = a.netAddRemote;
        r.type = (NetRadio::Type)a.netAddType;
        r.host = a.netAddHost;
        r.port = a.netAddPort;
        auto pr = std::make_shared<App::NetProbe>();
        pr->radio = r;
        a.netAddProbe = pr;
        a.netAddErr.clear();
        std::thread([pr] { std::string e; pr->ok = probeNetRadio(pr->radio, 3000, e); pr->err = e; pr->done = true; }).detach();   // never blocks the UI
    }
    ImGui::EndDisabled();
    if (busy) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("connecting…")); }
    if (!a.netAddErr.empty()) { ImGui::PushTextWrapPos(400 * gUi); ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), tr("Cannot connect: %s"), a.netAddErr.c_str()); ImGui::PopTextWrapPos(); }
    if (!a.netRadios.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Added radios (listed with the others)"));
        int drop = -1;
        ImGui::BeginDisabled(a.engine.running());
        for (int i = 0; i < (int)a.netRadios.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::SmallButton(tr("Remove"))) drop = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(netRadioDeviceName(a.netRadios[i]).c_str());
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (drop >= 0) {
            a.netRadios.erase(a.netRadios.begin() + drop);
            savePrefs(a);
            refreshDevices(a);
            a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1);
        }
    }
    ImGui::End();
}

// The Hamlib rig window: a searchable model list (Hamlib's own), the serial ports found on this computer, a baud rate from the model's caps.
static const RigModelInfo* hamlibModelNow(App& a) {
    const int i = findRigModel(a.hamlibModels, a.hamlibAddModel);
    return i >= 0 ? &a.hamlibModels[(size_t)i] : nullptr;
}

static void hamlibPickModel(App& a, const RigModelInfo& m) {
    a.hamlibAddModel = m.id;
    a.hamlibAddBaud = 0;   // the new model's own default
    if (m.network && !looksLikeNetworkRigPath(a.hamlibAddPath)) snprintf(a.hamlibAddPath, sizeof a.hamlibAddPath, "%s", "localhost:4532");
    if (!m.network && looksLikeNetworkRigPath(a.hamlibAddPath)) a.hamlibAddPath[0] = 0;   // a host:port is no serial port
    savePrefs(a);
}

void hamlibRigWindow(App& a) {
    if (!a.hamlibAddOpen) { a.hamlibWasOpen = false; return; }
    ImGui::SetNextWindowSize(ImVec2(460 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Hamlib rigs"), &a.hamlibAddOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    if (!hamlibSupported()) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("This build has no Hamlib. Rebuild with -DDECT2_HAVE_HAMLIB=ON after installing Hamlib."));
        ImGui::End();
        return;
    }
    if (!a.hamlibWasOpen) {   // just opened: load the model list once and look for the serial ports
        a.hamlibWasOpen = true;
        if (a.hamlibModels.empty()) a.hamlibModels = hamlibModelList();
        a.hamlibPorts = listSerialPorts();
        a.hamlibModelQuery[0] = 0;
    }
    ImGui::PushTextWrapPos(440 * gUi);
    ImGui::TextDisabled("%s", tr("A radio controlled over CAT (an Icom IC-R8600, an IC-7300, a rigctld on the network): OnAir and the rig follow each other's frequency and mode. IQ or audio comes from a sound card. OnAir never transmits through this."));
    ImGui::PopTextWrapPos();

    // ---- rig model: search by name, maker or number, or type the number ----
    const RigModelInfo* cur = hamlibModelNow(a);
    const std::string curLabel = cur ? rigModelLabel(*cur) : tr("Model") + std::string(" ") + std::to_string(a.hamlibAddModel) + " (id " + std::to_string(a.hamlibAddModel) + ")";
    ImGui::SetNextItemWidth(330 * gUi);
    if (ImGui::BeginCombo(tr("Rig"), curLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); a.hamlibModelQuery[0] = 0; }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##rigsearch", tr("Search: r8600, icom, 3079 …"), a.hamlibModelQuery, sizeof a.hamlibModelQuery);
        const auto hits = searchRigModels(a.hamlibModels, a.hamlibModelQuery);
        ImGui::TextDisabled("%d / %d", (int)hits.size(), (int)a.hamlibModels.size());
        ImGui::BeginChild("##rigrows", ImVec2(0, 260 * gUi));
        ImGuiListClipper clip;
        clip.Begin((int)hits.size());
        while (clip.Step())
            for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
                const RigModelInfo& m = a.hamlibModels[(size_t)hits[(size_t)k]];
                const std::string row = rigModelLabel(m) + "   " + rigStatusName(m.status) + (m.network ? "   network" : "");
                if (ImGui::Selectable(row.c_str(), m.id == a.hamlibAddModel)) { hamlibPickModel(a, m); ImGui::CloseCurrentPopup(); }
            }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click, then type part of the name (r8600, icom, ft-991) or the Hamlib number (3079) to filter the list."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    int idIn = a.hamlibAddModel;
    if (ImGui::InputInt("##rigid", &idIn, 0, 0) && idIn > 0 && idIn != a.hamlibAddModel) {
        const int i = findRigModel(a.hamlibModels, idIn);
        if (i >= 0) hamlibPickModel(a, a.hamlibModels[(size_t)i]);
        else { a.hamlibAddModel = idIn; a.hamlibAddBaud = 0; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The Hamlib model number (rigctl -l), typed directly. IC-R8600 is 3079, IC-7300 is 3073, 2 is a rigctld on the network."));
    cur = hamlibModelNow(a);
    if (!cur && !a.hamlibModels.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", tr("This Hamlib has no model with that number."));
    if (cur && cur->status != 3) ImGui::TextDisabled("%s", (std::string(tr("Driver status")) + ": " + rigStatusName(cur->status)).c_str());

    // ---- connection: detected serial ports, or host:port for a rigctld ----
    const bool netModel = cur && cur->network;
    ImGui::SetNextItemWidth(330 * gUi);
    const char* prev = a.hamlibAddPath[0] ? a.hamlibAddPath : tr("choose a port");
    if (ImGui::BeginCombo(tr("Connection"), prev)) {
        if (a.hamlibPorts.empty()) ImGui::TextDisabled("%s", tr("No serial ports found: plug the rig in and press Refresh."));
        for (const auto& p : a.hamlibPorts)
            if (ImGui::Selectable(p.display.c_str(), p.path == a.hamlibAddPath)) {
                snprintf(a.hamlibAddPath, sizeof a.hamlibAddPath, "%s", p.path.c_str());
                savePrefs(a);
            }
        ImGui::Separator();
        if (ImGui::Selectable(tr("Network (rigctld) …"))) {
            if (!looksLikeNetworkRigPath(a.hamlibAddPath)) snprintf(a.hamlibAddPath, sizeof a.hamlibAddPath, "%s", "localhost:4532");
            const int i = findRigModel(a.hamlibModels, 2);
            if (i >= 0) hamlibPickModel(a, a.hamlibModels[(size_t)i]);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh"))) a.hamlibPorts = listSerialPorts();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Look for serial ports again (after plugging the rig in)."));
    ImGui::SetNextItemWidth(330 * gUi);
    ImGui::InputTextWithHint(tr("Port or address"), "/dev/cu.usbserial…, COM3 or localhost:4532", a.hamlibAddPath, sizeof a.hamlibAddPath);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Type it by hand if the list does not show your port. host:port is for a rigctld (model 2) on this or another computer."));
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    const bool netPath = looksLikeNetworkRigPath(a.hamlibAddPath);
    if (netPath && cur && !cur->network && a.hamlibAddModel != 2)
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("A host:port address needs model 2 (rigctld), not this serial rig."));

    // ---- baud rate from the model ----
    if (cur && !cur->network && !netPath) {
        const auto bauds = rigBaudChoices(*cur);
        const int def = rigDefaultBaud(*cur);
        char bl[48];
        if (a.hamlibAddBaud > 0) snprintf(bl, sizeof bl, "%d", a.hamlibAddBaud);
        else if (def > 0) snprintf(bl, sizeof bl, "%s (%d)", tr("Default"), def);
        else snprintf(bl, sizeof bl, "%s", tr("Default"));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::BeginCombo(tr("Baud rate"), bl)) {
            if (ImGui::Selectable(def > 0 ? (std::string(tr("Default")) + " (" + std::to_string(def) + ")").c_str() : tr("Default"), a.hamlibAddBaud == 0)) { a.hamlibAddBaud = 0; savePrefs(a); }
            for (int b : bauds)
                if (ImGui::Selectable(std::to_string(b).c_str(), a.hamlibAddBaud == b)) { a.hamlibAddBaud = b; savePrefs(a); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", (std::string(tr("The speed in the rig's own menu must match (Icom: CI-V baud rate). Data bits ")) + std::to_string(cur->dataBits) + ", " + tr("stop bits ") + std::to_string(cur->stopBits) + tr(" are set from the model.")).c_str());
    }
    ImGui::SetNextItemWidth(330 * gUi);
    ImGui::InputTextWithHint(tr("Label"), tr("optional"), a.hamlibAddLabel, sizeof a.hamlibAddLabel);
    ImGui::SetNextItemWidth(330 * gUi);
    ImGui::InputTextWithHint(tr("Sound card id"), tr("empty = CAT only (silence IQ)"), a.hamlibAddAudio, sizeof a.hamlibAddAudio);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The same device id as in “Sound card: …” in the source list. Use a VAC or line input for WebSDR-style IQ."));
    static bool hamlibPathMissing = false;
    if (ImGui::Button(tr("Add"))) {
        HamlibRig r;
        r.model = a.hamlibAddModel;
        r.path = a.hamlibAddPath;
        r.label = a.hamlibAddLabel;
        r.audioId = a.hamlibAddAudio;
        r.baud = netPath || (cur && cur->network) ? 0 : a.hamlibAddBaud;
        if (r.label.empty() && cur) {
            std::string tail = r.path;
            const size_t sl = tail.find_last_of("/\\");
            if (sl != std::string::npos) tail = tail.substr(sl + 1);
            r.label = cur->mfg + " " + cur->model + " (" + tail + ")";
        }
        if (r.path.empty()) hamlibPathMissing = true;
        else {
            hamlibPathMissing = false;
            const auto it = std::find(a.hamlibRigs.begin(), a.hamlibRigs.end(), r);
            if (it == a.hamlibRigs.end()) a.hamlibRigs.push_back(r);
            savePrefs(a);
            if (!a.engine.running()) {
                refreshDevices(a);
                const std::string name = hamlibRigDeviceName(r);
                for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].name == name) { a.devIdx = i; a.tune.biasTee = false; break; }
            }
            a.hamlibAddOpen = false;
        }
    }
    if (hamlibPathMissing) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", tr("Choose the rig's port or type its address."));
    if (!a.hamlibRigs.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Added rigs (listed with the others)"));
        int drop = -1;
        ImGui::BeginDisabled(a.engine.running());
        for (int i = 0; i < (int)a.hamlibRigs.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::SmallButton(tr("Remove"))) drop = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(hamlibRigDeviceName(a.hamlibRigs[i]).c_str());
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (drop >= 0) {
            a.hamlibRigs.erase(a.hamlibRigs.begin() + drop);
            savePrefs(a);
            refreshDevices(a);
            a.devIdx = std::min(a.devIdx, (int)a.devices.size() - 1);
        }
    }
    {   // the live link, while a Hamlib receiver runs
        const HamlibRigState st = hamlibRigState();
        if (st.active) {
            ImGui::Separator();
            if (st.connected) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s %.6f MHz %s", tr("Rig connected:"), st.hz / 1e6, st.mode.c_str());
            else ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s %s", tr("Rig link lost, reconnecting:"), st.status.c_str());
        }
    }
    ImGui::End();
}

// While a Hamlib receiver runs: what was turned on the rig (its dial, its mode) is taken over into the frequency box and the mode setting.
static void hamlibFollowRig(App& a) {
    const HamlibRigState st = hamlibRigState();
    if (!st.active) { a.hamlibRigSeq = 0; a.hamlibLinkWas = true; return; }
    if (st.connected != a.hamlibLinkWas) {
        a.hamlibLinkWas = st.connected;
        a.engine.log(st.connected ? "Hamlib rig: connected again" : "Hamlib rig: link lost (" + st.status + "), reconnecting");
    }
    if (st.seq == a.hamlibRigSeq) return;
    a.hamlibRigSeq = st.seq;
    bool changed = false;
    if (st.hz > 0 && std::fabs(st.hz - a.freqMhz * 1e6) > 1.0) { a.freqMhz = st.hz / 1e6; a.tune.centerHz = st.hz; changed = true; }
    for (const auto& m : hamlibModeChoices())
        if (hamlibSameMode(m, st.mode)) {
            std::string& cur = a.tune.radio["hamlib_mode"];
            if (!hamlibSameMode(cur.empty() ? "USB" : cur, m)) { cur = m; changed = true; }
            break;
        }
    if (changed) {
        a.engine.log("Hamlib rig: " + std::to_string(st.hz / 1e6) + " MHz " + st.mode);
        a.engine.retune(a.tune);
        a.peak.clear();
        savePrefs(a);
    }
}

void toolbarParts(App& a, int mask, bool vertical) {
    bool running = a.engine.running();
    bool retune = false;
    ImGui::AlignTextToFramePadding();
    if (!a.newUi) { ImGui::TextColored(pal::accent(), "%s", tr("ONAIR")); ImGui::SameLine(0, 16 * gUi); }
    if (mask & TbSource) {
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
        }
        ImGui::Separator();
        if (ImGui::Selectable(tr("Add network radio…"))) a.netAddOpen = true;
        if (hamlibSupported() && ImGui::Selectable(tr("Add Hamlib rig…"))) a.hamlibAddOpen = true;
        ImGui::EndCombo();
    }
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

    bool isHw = a.devices[a.devIdx].isRadio();
    const DeviceInfo& curDev = a.devices[a.devIdx];
    const bool generic = curDev.isGeneric();
    setAdcBits(adcBitsFor(curDev, a.engine.running() ? a.engine.sampleRate() : a.tune.sampleRate));
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;

    if (mask & TbFreq) {
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
    const ModeUi* mu = modeUi(a.family);
    if ((mask & TbTuner) && !(mask & TbFreq)) {   // the side panels: the frequency is in the top bar, the sample rate heads the tuner
        sampleRateControl(a);
        if (!vertical) flowNext(10 * gUi);
    }
    if (mask & TbTuner) {
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

    if (mask & TbTuner) secondModeControl(a, vertical);

    if (mask & TbGain) {
    if (!a.newUi) vSeparator();   // the new interface starts the gain controls on a second row
    ImGui::BeginDisabled(!isHw);
    if (!vertical) { ImGui::TextDisabled("%s", tr("GAIN")); ImGui::SameLine(0, 5 * gUi); }
    if (generic) {
        // in the side panel the slider gets its own row (on the header's row it pushed AGC and Auto-tune past the panel edge)
        if (vertical) { ImGui::TextDisabled("%s", tr("Gain")); ImGui::SameLine(54 * gUi); } else ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * gUi);
        float gdb = (float)a.tune.gainDb;
        ImGui::SliderFloat("##gain", &gdb, (float)curDev.gainMinDb, (float)std::max(curDev.gainMaxDb, curDev.gainMinDb + 1.0), "%.0f dB");
        a.tune.gainDb = std::round(gdb);
        if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    } else {
    ImGui::TextDisabled("%s", tr("LNA"));
    ImGui::SameLine(vertical ? 54 * gUi : 0);
    ImGui::SetNextItemWidth((vertical ? 150 : 78) * gUi);
    int lna = a.tune.lnaDb;
    ImGui::SliderInt("##lna", &lna, 0, 40, "%d dB");
    a.tune.lnaDb = (lna + 4) / 8 * 8; // hardware steps are 8 dB
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    if (!vertical) flowNext();
    ImGui::TextDisabled("%s", tr("VGA"));
    ImGui::SameLine(vertical ? 54 * gUi : 0);
    ImGui::SetNextItemWidth((vertical ? 150 : 78) * gUi);
    int vga = a.tune.vgaDb;
    ImGui::SliderInt("##vga", &vga, 0, 62, "%d dB");
    a.tune.vgaDb = (vga + 1) / 2 * 2; // 2 dB steps
    if (ImGui::IsItemDeactivatedAfterEdit()) retune = true, a.agcOn = false;
    if (!vertical) flowNext();
    if (ImGui::Checkbox(tr("Amp"), &a.tune.ampOn)) retune = true, a.agcOn = false;
    }
    if (!vertical) flowNext(); else if (!generic) ImGui::SameLine();   // side panel, generic radio: AGC and Auto-tune on the row below the slider
    if (ImGui::Checkbox(tr("AGC"), &a.agcOn)) { a.agc.reset(); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Keep the ADC level in a healthy window (about -16 dBFS rms, no clipping) by adjusting LNA, VGA and amp.\nTouching a gain control turns it off."));
    if (vertical) ImGui::SameLine(0, 14 * gUi); else flowNext();
    if (a.sweep.active()) {
        if (ImGui::Button(tr("Stop tune"))) { a.sweep = GainSweep(); }
    } else if (ImGui::Button(tr("Auto-tune")) && running) {
        int rfMax = 0;   // a radio with an RF gain stage (SDRplay RFGR): the sweep covers its states too
        if (generic) {
            for (const auto& rs : curDev.settings)
                if (rs.key == "rfgr" && rs.values.size() > 2)
                    for (size_t i = rs.values.size(); i-- > 0;)
                        if (rs.values[i] != "auto") { rfMax = atoi(rs.values[i].c_str()); break; }
            if (rfMax == 0 && curDev.board == "sdrplay") rfMax = 9;   // known before the radio has been opened once
        }
        a.sweep.start(ImGui::GetTime(), generic ? (int)curDev.gainMaxDb : 0, rfMax);
        a.agcOn = false;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Gain helper: tries LNA/VGA/amp combinations for about 40 s and keeps the one with the best SNR that does not clip.\nNeeds a signal the receiver can lock to."));
    gainOptButtons(a, running);
    ImGui::EndDisabled();
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
        if (a.family == 12 && !a.tune.biasTee) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Most GNSS antennas are active and need this on.")); ImGui::PopTextWrapPos(); }
        if (vertical) ImGui::PopStyleVar();
    }
    if (vertical) radioSettingsUi(a, true);   // radio_ui.cpp: the selected radio's own settings (frequency correction, notch filters, ...); a one-line bar has its button elsewhere
    }

    if (!a.newUi) {   // the new interface has its Start button in the side rail
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
    }

    if ((mask & TbDecoder) && (a.family == 0 || a.family == 3)) {   // the LDPC decoder is used by DVB-T2 and ATSC 3.0 only
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
    if ((mask & TbDecoder) && a.family == 0) {   // the other families have one standard each: nothing to choose
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

    if ((mask & TbDecoder) && mu && mu->decoder) {
        if (!vertical) vSeparator();
        mu->decoder(a, retune);
    }
    if (!vertical) flowEnd();

    if (running && a.devices[a.devIdx].kind == DeviceInfo::Hamlib) hamlibFollowRig(a);   // the rig's knob moves the frequency box
    else if (!running) a.hamlibRigSeq = 0;
    if (retune && running) {
        const bool moved = std::fabs(a.tune.centerHz - a.freqMhz * 1e6) > 1;
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.log("retune " + std::to_string(a.freqMhz) + " MHz");
        if ((a.fmMode || a.family >= 6) && moved) a.engine.retuneReset(a.tune);   // forget the old station's name and flush its sound
        else a.engine.retune(a.tune);
        a.peak.clear();
        savePrefs(a);
    }

}

// Options of the selected source that are not gain/frequency: the synthetic generator's parameters, or the IQ file.

void toolbar(App& a) { toolbarParts(a, TbAll, false); }

static double dialMhzFromAbs(const App& a, double absMhz) {
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() &&
                      (a.devices[a.devIdx].kind == DeviceInfo::File || a.devices[a.devIdx].isAudio());
    return absMhz - (mt && !file ? mt->tuneOffsetHz / 1e6 : 0.0);
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

void spectrumTune(App& a, double absMhz) {
    if (a.tuningMode == 1) {
        if (a.fmMode) { a.freqMhz = std::round(std::max(0.0, dialMhzFromAbs(a, absMhz)) * 10.0) / 10.0; tuneDecode(a, absMhz); }
        else tuneDecode(a, absMhz);
    } else if (a.fmMode) fmTune(a, absMhz);
    else tuneFreq(a, dialMhzFromAbs(a, absMhz));
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

bool sourceOptionsOpen(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return false;
    const DeviceInfo::Kind k = a.devices[a.devIdx].kind;
    return k == DeviceInfo::Synthetic || (k == DeviceInfo::File && !a.engine.running()) || k == DeviceInfo::Audio || k == DeviceInfo::Hamlib;
}

void sourceOptions(App& a) {
    bool running = a.engine.running();
    bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    const bool isAudio = a.devices[a.devIdx].isAudio();
    const bool isHamlib = a.devices[a.devIdx].kind == DeviceInfo::Hamlib;
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic) {
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
    if (isFile && !running) {
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
        flowEnd();
    }
    if (isHamlib) {
        ImGui::TextDisabled("%s", tr("Hamlib CAT"));
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(380 * gUi);
            ImGui::TextUnformatted(tr("Frequency and mode go to the rig over the serial port. Set a sound card id when adding the rig for IQ or a speaker tap; leave it empty for CAT-only (silence IQ)."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        flowEnd();
    }
    if (isAudio || isHamlib) {
        ImGui::TextDisabled("%s", tr("sound card"));
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(380 * gUi);
            ImGui::TextUnformatted(tr("For WebSDR, a virtual audio cable (VAC), or a radio's speaker or line into the computer. Not a replacement for an SDR when you need megahertz of bandwidth. The frequency field is the dial the decoder should assume; the sound card is not tuned. There is no gain or antenna power."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        flowNext();
        ImGui::SetNextItemWidth(130 * gUi);
        const char* am[] = {tr("I/Q stereo"), tr("USB audio"), tr("LSB audio"), tr("Mono real")};
        if (ImGui::Combo("##aiq", &a.audioIqMode, am, 4)) {
            a.tune.radio["audio_iq"] = std::to_string(a.audioIqMode);
            savePrefs(a);
            if (running) { std::string e; a.engine.retune(a.tune); (void)e; }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(380 * gUi);
            ImGui::TextUnformatted(tr("I/Q stereo: left is I, right is Q (HDSDR / WebSDR VAC). USB audio: real audio to analytic IQ for FT8, RTTY, CW, SSB from a speaker tap. LSB audio: the same, conjugate. Mono real: I is the sample, Q is 0 (real sampling, not FM discriminator audio)."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        flowEnd();
    }
}

void statusBar(App& a) {
    // a radio on the network: the samples that did not arrive (the connection dropped and came back, or the receiver was too slow)
    if (a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::Native &&
        (a.devices[a.devIdx].board == "rtltcp" || a.devices[a.devIdx].board == "spyserver" || a.devices[a.devIdx].board == "sdrpp")) {
        const uint64_t lost = a.engine.droppedSamples();
        if (lost) ImGui::TextColored(pal::warnAmber(), tr("Network radio: %llu samples lost (underruns)"), (unsigned long long)lost);
    }
    // flowEnd(): the last group of the status line wraps too
    if (const ModeUi* mu = modeUi(a.family)) if (mu->status) { mu->status(a); flowEnd(); return; }
    if (a.dabMode) { dabStatus(a); flowEnd(); return; }
    if (a.fmMode) { fmStatus(a); flowEnd(); return; }
    if (a.atsc3Mode) { atsc3Status(a); flowEnd(); return; }
    if (a.isdbtMode) { isdbtStatus(a); flowEnd(); return; }
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const RxTelemetry& rx = a.rx;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    // ---- line 1: lamps, lock state and signal mode
    {
        int iq = run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0;
        lamp("IQ", iq, (int)Ic::Wave); flowNext(12 * gUi);
        const double frameS = rx.frameMs > 0 ? rx.frameMs / 1e3 : 0.5;
        const bool isT = rx.standard == 1;
        const double okFrac = (rx.blocksOk + rx.blocksBad) ? (double)rx.blocksOk / (rx.blocksOk + rx.blocksBad) : 0;
        const int fec = !run || !rx.plpValid || rx.plpFrames == 0 ? 0 : okFrac > 0.995 ? 1 : okFrac > 0.5 ? 2 : 3;
        if (rx.standard == 2) {
            const AtscTelemetry& at = rx.atsc;
            const double okF = (at.rsClean + at.rsCorrected + at.rsFailed) ? (double)(at.rsClean + at.rsCorrected) / (double)(at.rsClean + at.rsCorrected + at.rsFailed) : 0;
            lamp("Pilot", !run ? 0 : at.pilot ? 1 : 0); flowNext(12 * gUi);
            lamp("Seg", !run ? 0 : at.segSync ? 1 : 0); flowNext(12 * gUi);
            lamp("Field", !run ? 0 : at.fieldSync ? 1 : at.segSync ? 2 : 0); flowNext(12 * gUi);
            lamp("Eq", !run ? 0 : at.eqTrained ? 1 : at.fieldSync ? 2 : 0); flowNext(12 * gUi);
            lamp("Trellis", !run ? 0 : at.tsOk ? 1 : at.eqTrained ? 2 : 0); flowNext(12 * gUi);
            lamp("RS", !run || at.fields == 0 ? 0 : okF > 0.995 ? 1 : okF > 0.5 ? 2 : 3); flowNext(12 * gUi);
        } else if (isT) {
            // DVB-T: cyclic-prefix sync, guard interval, TPS signalling, channel estimate, then Viterbi and Reed-Solomon
            lamp("Sync", !run ? 0 : rx.state >= 1 ? 1 : 0); flowNext(12 * gUi);
            lamp("GI", !run ? 0 : rx.giIdx >= 0 ? (rx.state == 2 ? 1 : 2) : 0); flowNext(12 * gUi);
            lamp("TPS", !run ? 0 : rx.dvbt.tpsOk ? 1 : rx.state == 1 ? 2 : 0); flowNext(12 * gUi);
            lamp("Chan", !run ? 0 : rx.chValid && rx.dataValid ? 1 : rx.dvbt.tpsOk ? 2 : 0); flowNext(12 * gUi);
            lamp("Viterbi", !run ? 0 : rx.dvbt.fecSync ? 1 : rx.dvbt.tpsOk ? 2 : 0); flowNext(12 * gUi);
            lamp("RS", fec); flowNext(12 * gUi);
        } else {
        lamp("P1", !run || !a.rxSeen ? 0 : (rx.p1.valid && rx.secSinceP1 < std::max(1.0, 3 * frameS) ? 1 : 2)); flowNext(12 * gUi);
        lamp("GI", !run ? 0 : rx.state == 2 ? 1 : rx.state == 1 ? 2 : 0); flowNext(12 * gUi);
        lamp("L1-pre", !run ? 0 : rx.l1preOk ? 1 : rx.l1preGood > 0 ? 2 : rx.chValid ? 3 : 0); flowNext(12 * gUi);
        lamp("L1-post", !run ? 0 : rx.l1postOk ? 1 : rx.l1postGood > 0 ? 2 : rx.l1preOk ? 3 : 0); flowNext(12 * gUi);
        lamp("Frame", !run ? 0 : rx.dataValid ? 1 : rx.l1preOk ? 2 : 0); flowNext(12 * gUi);
        lamp("LDPC", fec); flowNext(12 * gUi);
        lamp("BCH", fec); flowNext(12 * gUi);
        }
        lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : (a.ts.ccErrors > 0 && a.bb.framesLost > 0.02 * (a.bb.frames + 1)) ? 2 : 1, (int)Ic::Layers); flowNext(12 * gUi);
        const PlayerStats ps = a.engine.player().stats();
        const bool pl = run && ps.active;
        lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); flowNext(12 * gUi);
        lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
        ImGui::TextDisabled("|"); flowNext(10 * gUi);
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
        ImGui::TextDisabled("%s", tr(label)); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro(N_("State"), a.scanner.progress().running ? tr("channel scan has the radio") : tr("stopped"), ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (a.engine.radioLost()) ro("State", tr("radio disconnected"), ImVec4(0.95f, 0.35f, 0.3f, 1));
    else if (rx.state == 2) ro("State", tr("Locked"), ImVec4(0.35f, 0.90f, 0.45f, 1));
    else if (rx.state == 1) ro("State", tr("syncing"), ImVec4(0.95f, 0.75f, 0.2f, 1));
    else ro("State", tr("searching"), ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (run && rx.standard == 2 && rx.state >= 1) {
        ro(N_("Mode"), "ATSC 8-VSB");
        ro(N_("Channel"), "6 MHz");
        snprintf(b, sizeof b, "%d", rx.atsc.fieldParity); ro("Field", b);
        ro("Pilot", tr(rx.atsc.pilot ? "locked" : "-"));
    } else if (run && rx.standard == 1 && rx.state >= 1) {
        ro("Mode", "DVB-T");
        ro("FFT", rx.fftN == 8192 ? "8K" : "2K");
        ro("GI", dvbt::guardName(rx.giIdx));
        if (rx.dvbt.tpsOk) {
            snprintf(b, sizeof b, "%s %s", dvbt::modName(rx.dvbt.mod), dvbt::rateName(rx.dvbt.crHp));
            ro("TPS", b);
            if (rx.dvbt.hier) ro("hier", tr("yes"), ImVec4(0.95f, 0.7f, 0.2f, 1));
        } else ro("TPS", tr("searching"), ImVec4(0.6f, 0.64f, 0.68f, 1));
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
    flowBreak();
    // ---- line 2: numbers and gauges
    if (run && (rx.state == 2 || (rx.standard == 1 && rx.state >= 1))) {
        snprintf(b, sizeof b, "%+.1f Hz", rx.cfoHz); ro("CFO", b);
        if (rx.standard != 1) { snprintf(b, sizeof b, "%+.1f ppm", rx.sroPpm); ro("SRO", b); }
        snprintf(b, sizeof b, "%.1f dB", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb); ro("SNR", b);
        if (rx.plpMerDb > 0 && rx.plpMerDb < 90) { snprintf(b, sizeof b, "%.1f dB", rx.plpMerDb); ro("MER", b); }
        if (!a.mpd.report().echoes.empty()) { snprintf(b, sizeof b, "%.2f us", std::fabs(a.mpd.report().echoes[0].delayUs)); ro(N_("Delay"), b); }
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("Level")); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f"), adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("%s", tr("Quality")); ImGui::SameLine(0, 5 * gUi);
    {
        const QualityReport& q = a.quality.report();
        const float t = run && q.valid ? (float)q.percent / 100.f : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, run && q.valid ? "%.0f%%  %s" : "-", q.percent, q.label.c_str());
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && run && q.valid) ImGui::SetTooltip(tr("data SNR %.1f dB, needed about %.1f dB (margin %+.1f dB)\nFEC blocks decoded %.1f%%"), q.snrDb, q.requiredDb, q.marginDb, q.fecOk * 100);
    }
    flowNext(15 * gUi);
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro(N_("dropped"), lossText(l), lossColour(l)); lossTooltip(l); }
    if (run && a.mpd.report().level != MultipathLevel::Unknown) {
        const MultipathReport& mr = a.mpd.report();
        const ImVec4 col = mr.level == MultipathLevel::None ? ImVec4(0.5f, 0.55f, 0.6f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                         : mr.level == MultipathLevel::Likely ? ImVec4(0.95f, 0.6f, 0.2f, 1) : ImVec4(0.95f, 0.35f, 0.25f, 1);
        ImGui::AlignTextToFramePadding();
        iconInline(Ic::Echo, ImGui::ColorConvertFloat4ToU32(col), 0.9f); ImGui::SameLine(0, 4 * gUi);
        ImGui::TextColored(col, tr("multipath: %s"), multipathName(mr.level));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(mr.headline.c_str());
            for (auto& r : mr.reasons) ImGui::BulletText("%s", r.c_str());
            ImGui::EndTooltip();
        }
        flowNext(15 * gUi);
    }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (a.sweep.active()) ImGui::TextColored(pal::heading(), tr("gain helper %d/%d ..."), std::max(0, a.sweep.current()) + 1, (int)a.sweep.entries().size());
        else if (gainOptStatus(a)) {}
        else if (adc == AdcStatus::Overload) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), tr("ADC OVERLOAD - %s"), tr(a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"));
        else if (adc == AdcStatus::High) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC level high"));
        else if (adc == AdcStatus::Low) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC level low - raise the gain"));
        else if (adc == AdcStatus::NoSignal) ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "%s", tr("ADC sees almost nothing - antenna / gain?"));
        else flowBreak();
    } else flowBreak();
    flowEnd();
}

void gainControl(App& a) {
    gainOptTick(a);
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
        if (changed && a.sweep.rf() >= 0) a.tune.radio["rfgr"] = std::to_string(a.sweep.rf());
        if (!a.sweep.active()) a.engine.log("gain helper: " + a.sweep.summary() + (a.sweep.rf() >= 0 ? ", RF state " + std::to_string(a.sweep.rf()) : std::string()));
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

// The receiver modes. Adding a mode is one row here (and its family number in app.h / setFamily).
// The families are also stored in the settings: 0 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.0, 4 ISDB-T, 5 FM, 6 DVB-S/S2, 7 DTMB, 8 analog TV, 9 DMR, 10 DRM, 11 ADS-B, 12 GNSS, 13 radiosonde, 14 AIS, 15 marine, 16 ACARS, 17 Inmarsat-C, 18 Inmarsat Aero, 19 Iridium, 20 mesh,
// 21 HD Radio, 22 CDR, 23 pagers, 24 APRS / packet, 25 HF digital, 26 airband, 27 VDL Mode 2, 28 ISM sensors, 30 VOR / ILS, 31 M17, 32 cell towers, 33 D-STAR, 34 CW, 35 HFDL, 36 PSK31, 37 JS8Call, 38 NXDN, 39 GSM-R, 42 spectrum analyser, 40 DVB-C, 45 dPMR, 44 Orbcomm, 43 Mode S uplink, 46 TETRA, 49 HF ALE, 47 MAC, 48 radio astronomy, 50 P25, 51 Bluetooth LE, 52 ionospheric sounders, 53 QO-100, 54 Wi-Fi networks, 55 general receiver.
// Within a group the rows are shown in this order; the keys 1-9 pick the modes in the same order.
const ModeDef kModes[] = {
    {0, "DVB",        0, IM_COL32(52, 92, 108, 255),  "DVB-T2 / DVB-T, detected automatically",                                    "DVB-T2 and DVB-T, automatic (Europe, Middle East, Africa, Asia, Australia)", N_("T2 and T, automatic"),   ImVec4(0.36f, 0.74f, 0.86f, 1)},
    {1, "ATSC",       0, IM_COL32(150, 100, 30, 255), "ATSC 8-VSB, 6 MHz channel (the DVB-only settings are off)",                 "ATSC 1.0, 8-VSB (US, Canada, Mexico, South Korea)",                           N_("8-VSB, North America"),  ImVec4(0.95f, 0.66f, 0.26f, 1)},
    {3, "ATSC 3.0",   0, IM_COL32(150, 70, 40, 255),  "ATSC 3.0 (NextGen TV), 6 MHz channel, ROUTE services",                      "ATSC 3.0, NextGen TV",                                                        N_("NextGen TV"),            ImVec4(0.95f, 0.50f, 0.36f, 1)},
    {4, "ISDB-T",     0, IM_COL32(120, 70, 140, 255), "ISDB-T (Japan, Brazil and most of South America; 8 MHz channels in the Maldives), 6, 7 or 8 MHz channel, 13 segments", "ISDB-T (Japan, Brazil, South America)",                                  N_("Japan, Brazil"),         ImVec4(0.72f, 0.56f, 0.92f, 1)},
    {6, "DVB-S/S2", 0, IM_COL32(52, 96, 150, 255), "DVB-S, DVB-S2 and DVB-S2X satellite TV (L-band IF through an LNB)", "DVB-S / S2 / S2X satellite television, needs a dish and an LNB", N_("Satellite"), ImVec4(0.42f, 0.64f, 0.96f, 1)},
    {7, "DTMB", 0, IM_COL32(150, 62, 74, 255), "DTMB / DTMB-A digital TV (China, Hong Kong, Cuba), 8 or 6 MHz channel", "DTMB digital terrestrial television (China, Hong Kong, Macau, Cuba, Pakistan)", N_("China, Hong Kong, Cuba"), ImVec4(0.92f, 0.46f, 0.50f, 1)},
    {40, "DVB-C", 0, IM_COL32(70, 110, 120, 255), "DVB-C cable TV (EN 300 429 / J.83 annex A and C): 16 to 256-QAM, symbol rate measured or set; needs a cable feed", "DVB-C digital cable television, needs the cable network as the input (not an antenna)", N_("Cable"), ImVec4(0.45f, 0.78f, 0.84f, 1), nullptr, "dvbc dvb-c cable qam j.83 annex catv tv"},
    {8, "Analog TV", 0, IM_COL32(100, 102, 112, 255), "Analogue TV: PAL, SECAM and NTSC with FM sound", "Analogue television (PAL, SECAM, NTSC)", N_("PAL, SECAM, NTSC"), ImVec4(0.76f, 0.79f, 0.84f, 1)},
    {47, "MAC", 0, IM_COL32(96, 84, 128, 255), "MAC television (D2-MAC, D-MAC): the 1980s-90s European satellite TV, FM, about 27 MHz wide (20 Msps sees most of it)", "MAC/packet satellite TV: picture, 4:3 / 16:9, channel name, digital sound, EuroCrypt detected", N_("D2-MAC, D-MAC satellite"), ImVec4(0.70f, 0.62f, 0.92f, 1), nullptr, "mac d2-mac d2mac d-mac dmac b-mac c-mac multiplexed analogue components eurocrypt satellite tv tdf tv-sat astra filmnet canal+ nordic packet"},
    {2, "DAB / DAB+", 1, IM_COL32(40, 130, 96, 255),  "DAB / DAB+ digital radio, Band III channels 5A to 13F",                     "DAB and DAB+ digital radio",                                                  N_("Digital radio"),         ImVec4(0.36f, 0.82f, 0.58f, 1)},
    {5, "FM",         1, IM_COL32(140, 90, 100, 255), "FM broadcast radio (87.5 - 108 MHz), stereo and RDS",                       "FM broadcast radio with stereo and RDS",                                      N_("Stereo and RDS"),        ImVec4(0.95f, 0.52f, 0.62f, 1)},
    {10, "DRM", 1, IM_COL32(48, 112, 128, 255), "DRM30 and DRM+ digital radio (shortwave, medium wave, VHF)", "DRM digital radio: DRM30 below 30 MHz, DRM+ in the VHF bands", N_("SW, MW, VHF"), ImVec4(0.46f, 0.82f, 0.82f, 1)},
    {21, "HD Radio", 1, IM_COL32(130, 96, 50, 255), "HD Radio (NRSC-5) digital sidebands of FM and AM stations (USA)", "HD Radio: the digital part of hybrid FM and AM stations", N_("USA, FM and AM"), ImVec4(0.92f, 0.72f, 0.40f, 1)},
    {22, "CDR", 1, IM_COL32(140, 60, 60, 255), "CDR (China Digital Radio) OFDM digital radio in the FM band", "CDR: China's digital radio in the FM band", N_("China digital radio"), ImVec4(0.94f, 0.48f, 0.44f, 1)},
    {11, "ADS-B", 2, IM_COL32(58, 98, 160, 255), "ADS-B / Mode S: aircraft on 1090 MHz", "ADS-B aircraft position and identity reports", N_("Aircraft, 1090 MHz"), ImVec4(0.52f, 0.74f, 0.98f, 1)},
    {43, "Mode S uplink", 2, IM_COL32(64, 92, 140, 255), "Mode S uplink: the 1030 MHz interrogations of radars and ACAS (Mode A/C, intermode, UF0 to UF24); needs 8 Msps", "Mode S uplink: interrogators, rotation periods, aircraft asked for registers, TCAS coordination", N_("Interrogations, 1030 MHz"), ImVec4(0.56f, 0.70f, 0.96f, 1), nullptr, "mode s uplink 1030 interrogation interrogator radar ssr all-call roll-call tcas acas uf11 comm-b bds sidelobe"},
    {16, "ACARS", 2, IM_COL32(110, 100, 60, 255), "ACARS aircraft data link messages around 131 MHz", "ACARS: short messages between aircraft and the ground", N_("Aircraft messages, VHF"), ImVec4(0.88f, 0.80f, 0.50f, 1)},
    {27, "VDL2", 2, IM_COL32(96, 104, 64, 255), "VDL Mode 2: the aircraft data link on 136.725 to 136.975 MHz, every channel at once", "VDL Mode 2: ACARS and ATN messages between aircraft and ground stations, positions on a map", N_("Data link, 136.9 MHz"), ImVec4(0.80f, 0.84f, 0.50f, 1), nullptr, "vdl vdl2 avlc acars atn cpdlc ads-c x.25 clnp datalink"},
    {82, "VDL4", 2, IM_COL32(88, 108, 72, 255), "VDL Mode 4: STDMA GFSK data link on 118 to 137 MHz (synthetic + spec; receive only)", "VDL Mode 4: synchronization and DLS bursts, CPR positions when decoded in the clear", N_("STDMA link, 136.9 MHz"), ImVec4(0.76f, 0.86f, 0.52f, 1), nullptr, "vdl vdl4 stdma gfsk sync burst dls cpr ads-b datalink experimental"},
    {35, "HFDL", 2, IM_COL32(104, 88, 60, 255), "HFDL: the HF aircraft data link (ARINC 635), 3 to 22 MHz, every channel in the band at once", "HFDL: ACARS, squitters and positions of aircraft over oceans and remote areas, on HF", N_("Data link, 8.9 MHz"), ImVec4(0.90f, 0.74f, 0.48f, 1), nullptr, "hfdl hf data link acars aircraft arinc 635 squitter oceanic"},
    {26, "Airband", 2, IM_COL32(70, 96, 130, 255), "Airband AM voice, 118 to 137 MHz: several 25 and 8.33 kHz channels at once", "Airband: tower, ground and ATIS voice on several channels at once", N_("AM voice, 118-137 MHz"), ImVec4(0.58f, 0.72f, 0.92f, 1)},
    {30, "VOR", 2, IM_COL32(60, 104, 120, 255), "VOR navigation beacons, 108 to 118 MHz: the radial, the identification and a position from two stations", "VOR: your bearing from a navigation beacon, its Morse identification and voice", N_("Radial, 108-118 MHz"), ImVec4(0.46f, 0.78f, 0.84f, 1), "vor", "vor dvor navaid radial bearing beacon navigation"},
    {30, "ILS", 2, IM_COL32(66, 92, 126, 255), "ILS approach guidance: localizer 108.10 - 111.95 MHz, glide slope 329 - 335 MHz, marker beacons on 75 MHz", "ILS: left / right of the runway centreline and above / below the glide path", N_("Localizer and glide slope"), ImVec4(0.54f, 0.70f, 0.94f, 1), "ils", "ils localizer glideslope glide slope marker approach landing"},
    {29, "NDB", 2, IM_COL32(120, 96, 48, 255), "NDB (non-directional beacon) on long wave, 190-535 kHz: the Morse ident of an aeronautical beacon (a keyed 400 Hz or 1020 Hz tone, or a keyed carrier, about 7 WPM). Receive only. Not the HF beacon list", "NDB: the Morse identification of a long-wave navigation beacon", N_("Ident, 190-535 kHz"), ImVec4(0.90f, 0.74f, 0.42f, 1), nullptr, "ndb non-directional beacon navaid morse ident long wave lw lf beacon band 190 535 khz 400 hz 1020 hz locator"},
    {56, "Drone Remote ID", 2, IM_COL32(70, 104, 112, 255), "Drone Remote ID (ASTM F3411 / ASD-STAN EN 4709-002 Open Drone ID) on 2.4 GHz: Bluetooth LE advertising and Wi-Fi beacons at once; serial number or session ID, UA type, position, height, speed and track, operator position and ID, on a table and a map (receive only: Remote ID is a public broadcast meant for anyone to read; nothing is transmitted)", "Drone Remote ID: drones and their operators, from the identification every drone must broadcast", N_("Drones, 2.4 GHz"), ImVec4(0.52f, 0.82f, 0.86f, 1), nullptr, "drone remote id rid uas uav open drone id opendroneid astm f3411 en 4709 asd-stan dji operator serial bluetooth wifi beacon 2.4 ghz faa easa"},
    {14, "AIS", 3, IM_COL32(40, 110, 140, 255), "AIS ship transponders on 161.975 and 162.025 MHz", "AIS: ship positions and identities", N_("Ships, 162 MHz"), ImVec4(0.40f, 0.76f, 0.90f, 1)},
    {63, "VDES", 3, IM_COL32(30, 100, 130, 255), "VDES (VHF Data Exchange System, ITU-R M.2092): ASM messages on 161.975 / 162.025 MHz decoded (GMSK 9.6 kbit/s, AIS framing, CRC checked); VDE-TER bursts on the 157.2 and 161.8 MHz channels detected with bandwidth, level and modulation class (frames not decoded)", "VDES: ASM frames decoded, VDE-TER bursts logged and classified", N_("Maritime, VHF"), ImVec4(0.46f, 0.80f, 0.94f, 1), nullptr, "vdes vde-ter vde ter asm m.2092 maritime vhf data exchange 157.2 161.8 161.975 162.025 pi/4-qpsk 8psk 16qam ais"},
    {15, "NAVTEX", 3, IM_COL32(48, 100, 120, 255), "NAVTEX navigational and weather warnings to ships, 518 kHz (SITOR-B)", "NAVTEX: safety messages for ships on 518 and 490 kHz", N_("Ship warnings, 518 kHz"), ImVec4(0.44f, 0.72f, 0.84f, 1), "navtex", "navtex marine sitor"},
    {15, "DSC", 3, IM_COL32(56, 92, 132, 255), "DSC digital selective calling: distress alerts and calls, MF/HF and VHF channel 70", "DSC: digital selective calling between ships and coast stations", N_("Ship calls, 2187.5 kHz"), ImVec4(0.50f, 0.66f, 0.92f, 1), "dsc", "dsc marine distress"},
    {15, "Weather fax", 3, IM_COL32(64, 108, 108, 255), "HF weather fax (radiofax): weather charts for ships on upper sideband", "Weather fax: HF radiofax charts for ships", N_("Weather charts, HF"), ImVec4(0.52f, 0.80f, 0.78f, 1), "wefax", "wefax weather fax radiofax hf fax marine"},
    {12, "GNSS", 4, IM_COL32(130, 112, 52, 255), "GNSS satellites (GPS, GLONASS, BeiDou, Galileo) around 1575 MHz, needs an active antenna", "GNSS: satellite tracking and position fix", N_("GPS, GLONASS, BeiDou"), ImVec4(0.90f, 0.78f, 0.42f, 1)},
    {17, "Inmarsat-C", 4, IM_COL32(100, 80, 140, 255), "Inmarsat-C satellite messages (EGC safety and news broadcasts) near 1537.7 MHz", "Inmarsat-C: satellite safety and news broadcasts, needs an L-band antenna", N_("EGC, L-band"), ImVec4(0.74f, 0.64f, 0.94f, 1)},
    {18, "Inmarsat Aero", 4, IM_COL32(120, 76, 120, 255), "Inmarsat Aero satellite data link: P channels at 600 and 1200 bit/s near 1545 MHz, and 10500 bit/s near 1546 MHz", "Inmarsat Aero: aircraft satellite data link, needs an L-band antenna", N_("Aero, L-band"), ImVec4(0.88f, 0.62f, 0.88f, 1)},
    {19, "Iridium", 4, IM_COL32(140, 76, 60, 255), "Iridium satellite bursts around 1622 MHz", "Iridium satellite downlink bursts, needs an L-band antenna", N_("Satellites, 1622 MHz"), ImVec4(0.94f, 0.60f, 0.46f, 1)},
    {59, "Meteor-M LRPT", 4, IM_COL32(64, 104, 132, 255), "Meteor-M N2-3 / N2-4 weather pictures on 137.1 / 137.9 MHz (LRPT: OQPSK or QPSK, 72 or 80 ksym/s): the MSU-MR channels and a colour composite grow line by line during the pass, saved as PNG", "Meteor-M LRPT: weather satellite pictures as the satellite passes", N_("Weather pictures, 137 MHz"), ImVec4(0.52f, 0.78f, 0.96f, 1), nullptr, "meteor meteor-m m2 n2-3 n2-4 lrpt msu-mr weather satellite picture image 137 mhz 137.1 137.9 oqpsk qpsk apt noaa cloud"},
    {60, "GOES HRIT", 4, IM_COL32(58, 96, 140, 255), "GOES-16 / 18 / 19 HRIT on 1694.1 MHz (BPSK 927 ksym/s, Viterbi, Reed-Solomon, CCSDS packets, LRIT files): full disk and mesoscale images (Rice, plain or JPEG) grow as their lines arrive, EMWIN weather text and admin messages, saved to a folder. Needs a dish (about 1 m, or a grid) with an LNA at the feed", "GOES HRIT: geostationary weather images and EMWIN text (needs a dish and an LNA)", N_("Weather images, 1694 MHz"), ImVec4(0.48f, 0.72f, 0.98f, 1), nullptr, "goes goes-16 goes-18 goes-19 goes-r hrit lrit emwin full disk mesoscale abi weather satellite geostationary image 1694 1694.1 l-band dish lna noaa"},
    {80, "Elektro-L", 4, IM_COL32(64, 92, 132, 255), "Elektro-L N3 / N4 / N5 L-band weather: HRIT and LRIT on 1691 MHz (BPSK, Viterbi, Reed-Solomon, CCSDS / xRIT, the same product path as GOES) and GGAK space-weather PCM/PM on 1693 MHz at 5 ksym/s. Pick the spacecraft and the link. Needs a dish with an LNA. Receive only", "Elektro-L: geostationary weather images (HRIT / LRIT) and GGAK (needs a dish and an LNA)", N_("Weather images, 1691 MHz"), ImVec4(0.50f, 0.70f, 0.94f, 1), nullptr, "elektro elektro-l elektrol n3 n4 n5 roscosmos hrit lrit ggak 1691 1693 l-band weather satellite geostationary dish lna"},
    {83, "UAT", 2, IM_COL32(48, 108, 148, 255), "UAT on 978 MHz: ADS-B position and identity reports, FIS-B weather text products and TIS-B traffic relay (DO-282). Typical 1 Msps capture. Receive only", "UAT 978 MHz: ADS-B, FIS-B weather and TIS-B relay", N_("Aircraft, 978 MHz"), ImVec4(0.48f, 0.72f, 0.96f, 1), nullptr, "uat 978 uat978 ads-b fis-b tis-b do-282 universal access transceiver weather metar taf nexrad"},
    {64, "HRPT / AHRPT", 9, IM_COL32(52, 98, 124, 255), "HRPT / AHRPT weather satellite pictures near 1.7 GHz: MetOp-B / MetOp-C AHRPT (QPSK 2.33 Msym/s, 1701.3 MHz) with the AVHRR channels growing line by line, FY-3 AHRPT (about 2.8 Msym/s, frames counted only) and the retired NOAA HRPT (BPSK, Manchester, 665.4 kbit/s). Needs a dish (about 1 m or more, tracking the satellite) with an LNA at the feed and a fast radio (4 Msps or more: HackRF, Airspy, SDRplay, Pluto; not an RTL-SDR)", "HRPT / AHRPT: MetOp AVHRR pictures at 1.7 GHz (needs a dish and a fast radio)", N_("Weather pictures, 1.7 GHz"), ImVec4(0.42f, 0.74f, 0.92f, 1), nullptr, "hrpt ahrpt metop metop-b metop-c avhrr fy-3 fengyun noaa noaa-15 noaa-18 noaa-19 weather satellite picture image 1701 1701.3 1698 1707 1702.5 l-band dish lna qpsk bpsk manchester polar orbit"},
    {44, "Orbcomm", 4, IM_COL32(70, 118, 96, 255), "Orbcomm subscriber downlinks, 137.2 - 137.8 MHz: 4800 bit/s SDPSK, two downlinks at once, satellites and their ephemeris", "Orbcomm satellite downlinks: satellite IDs, positions, channel assignments, messages", N_("Satellites, 137 MHz"), ImVec4(0.50f, 0.86f, 0.66f, 1), nullptr, "orbcomm orb sdpsk 137 mhz leo satellite iot ephemeris doppler"},
    {53, "QO-100", 4, IM_COL32(120, 84, 52, 255), "QO-100 (Es'hail-2) geostationary amateur satellite, 10489.5 - 10500 MHz through a Ku LNB: beacon lock corrects the LNB's error and drift, narrowband SSB listening, wideband DATV hand-over to DVB-S/S2", "QO-100: the amateur satellite's transponders with exact frequencies from its beacons", N_("Es'hail-2, 10.49 GHz"), ImVec4(0.94f, 0.68f, 0.42f, 1), nullptr, "qo100 qo-100 eshail es'hail-2 oscar 100 geostationary amateur satellite lnb ku 10489 739 ssb datv beacon bullseye"},
    {66, "FLARM", 2, IM_COL32(70, 120, 150, 255), "FLARM: glider and light-aircraft collision-avoidance broadcasts on 868.2 and 868.4 MHz (Europe), received like the Open Glider Network does. Old-protocol positions are decoded (device ID, type, altitude, speed, track, climb) and shown in a table and on the live map with distance and bearing from your receiver; the newer protocol is only counted. Receive only; aircraft that set the no-track or stealth flag are only counted, never shown or saved", "FLARM aircraft: gliders and light aircraft on 868 MHz", N_("Gliders, 868.2 / 868.4 MHz"), ImVec4(0.55f, 0.78f, 0.9f, 1), nullptr, "flarm glider gliders soaring light aircraft collision avoidance ogn open glider network 868 868.2 868.4 sailplane"},
    {68, "DJI drones", 2, IM_COL32(70, 112, 120, 255), "DJI drones on 2.4 and 5.8 GHz, receive only: wideband OFDM burst trains are classified by their shape (OcuSync-like, Lightbridge-like, unknown wideband OFDM) from the width, burst period, duty cycle and cyclic-prefix correlation, with a confidence and a clear note that this is a guess from the signal shape; DJI DroneID (the OcuSync 2 broadcast: Zadoff-Chu roots 600 and 147, turbo code, CRC-24) is decoded to the drone's serial number, position, home point and pilot position as sent in the clear. Needs 16 Msps or more (HackRF at 20 Msps; Pluto, bladeRF, USRP). The encrypted video link is never decoded", "DJI drones: OcuSync / Lightbridge-like signals by shape, and DroneID packets decoded", N_("Drones, 2.4 / 5.8 GHz"), ImVec4(0.52f, 0.84f, 0.80f, 1), nullptr, "dji drone drones ocusync lightbridge droneid drone id mavic mini phantom inspire dronesecurity zadoff chu turbo ofdm 2.4 ghz 5.8 ghz uav uas fpv video link hackrf pluto iqengine"},
    {61, "CubeSat telemetry", 4, IM_COL32(96, 104, 140, 255), "CubeSat telemetry from amateur satellites on 2 m / 70 cm (145.8 - 146 and 435 - 438 MHz): AX.25 at 1200 baud AFSK and 9600 baud G3RUH, GOMspace AX100 Mode 5 (ASM + Golay + Reed-Solomon) and Mode 6, CCSDS Reed-Solomon frames; callsigns, hex and ASCII dumps, satellite names from an editable table, KISS out to SatNOGS-style tools; the pass planner starts it with Doppler", "CubeSat telemetry: frames from amateur satellites, named and dumped", N_("Satellites, 2 m / 70 cm"), ImVec4(0.62f, 0.68f, 0.94f, 1), nullptr, "cubesat cube sat satellite telemetry amateur ax.25 ax25 g3ruh 9600 afsk 1200 gmsk ax100 gomspace mode 5 mode 6 golay reed solomon ccsds csp satnogs kiss beacon 435 437 145.8 70 cm 2 m doppler"},
    {62, "Globalstar", 4, IM_COL32(110, 96, 140, 255), "Globalstar L-band downlink, 1610 - 1626.5 MHz: activity monitor only (receive only, the spread and encrypted content is not decoded): power, occupancy and Doppler of the 13 CDMA channels, an event log, and a pass overlay from the pass planner's TLEs", "Globalstar: channel activity and Doppler (content is not decoded)", N_("Satellites, 1.6 GHz"), ImVec4(0.72f, 0.64f, 0.94f, 1), nullptr, "globalstar gstar satellite phone iot l-band 1610 1618 1626 cdma doppler occupancy activity spot"},
    {9, "DMR", 5, IM_COL32(70, 120, 70, 255), "DMR two-slot digital voice and data, 12.5 kHz channel", "DMR (Digital Mobile Radio), two-slot TDMA", N_("Digital voice"), ImVec4(0.62f, 0.84f, 0.46f, 1)},
    {38, "NXDN", 5, IM_COL32(60, 110, 110, 255), "NXDN digital two-way radio: NXDN48 (6.25 kHz) and NXDN96 (12.5 kHz), conventional and trunked", "NXDN: unit IDs, talkgroups, status and short data, site information", N_("Digital voice, 6.25 / 12.5 kHz"), ImVec4(0.50f, 0.84f, 0.84f, 1), nullptr, "nxdn nxdn48 nxdn96 kenwood nexedge icom idas digital voice two-way radio trunking ambe"},
    {50, "P25", 5, IM_COL32(90, 90, 130, 255), "APCO Project 25 Phase 1 (TIA-102) digital two-way radio, C4FM, 12.5 kHz: conventional channels and trunked control channels", "P25: talkgroups, unit IDs, trunking sites and channels, voice grants (receive only; encrypted calls are only reported)", N_("Digital voice, 12.5 kHz"), ImVec4(0.66f, 0.66f, 0.94f, 1), nullptr, "p25 apco project 25 tia-102 phase 1 c4fm imbe public safety police fire ems trunking talkgroup control channel wacn nac op25 dsd"},
    {45, "dPMR / PMR446", 5, IM_COL32(70, 100, 130, 255), "dPMR (ETSI TS 102 490) digital two-way radio, 6.25 kHz: PMR446 digital channels 1 to 16 (446.1 - 446.2 MHz) and the licensed bands", "dPMR: own and called IDs, colour codes, status and short messages, PMR446 channel scan", N_("Digital voice, 6.25 kHz"), ImVec4(0.56f, 0.76f, 0.94f, 1), nullptr, "dpmr pmr446 pmr 446 digital pmr walkie talkie icom kenwood dpmr446 two-way radio colour color code ambe"},
    {46, "TETRA", 5, IM_COL32(96, 84, 128, 255), "TETRA base stations (ETSI EN 300 392): network code, colour code, location area, carriers, neighbour cells, services and whether encryption is used (receive only, public broadcast, nothing decrypted)", "TETRA: the public broadcast information of TETRA cells", N_("Network info, 380 - 470 MHz"), ImVec4(0.74f, 0.66f, 0.96f, 1), nullptr, "tetra trunked radio emergency services police pmr mcc mnc colour color code bsch bnch sysinfo base station network info"},
    {79, "TETRAPOL", 5, IM_COL32(88, 76, 120, 255), "TETRAPOL (not TETRA) base stations, GMSK 8 kbit/s on 10 / 12.5 kHz UHF channels (PAS 0001-2): D_SYSTEM_INFO gives country, system, cell, location area and scramble. Receive only, no voice, nothing decrypted", "TETRAPOL: control-channel system information (receive only)", N_("Network info, UHF"), ImVec4(0.70f, 0.62f, 0.92f, 1), nullptr, "tetrapol tetra-pol pas 0001 gmsk 8 kbit control channel system info police france uhf"},
    {32, "Cell towers", 6, IM_COL32(70, 96, 130, 255), "LTE cells around you: operator, band, EARFCN, PCI, eNB and sector, TAC, neighbours (receive only, public broadcast)", "Cell towers: the public broadcast information of LTE cells", N_("LTE, band scan"), ImVec4(0.52f, 0.70f, 0.94f, 1), nullptr, "cell tower lte 4g enb earfcn pci operator"},
    {57, "5G NR cells", 6, IM_COL32(64, 104, 140, 255), "5G NR cells around you from their SS/PBCH blocks on the sync raster (GSCN) of n1, n3, n7, n8, n20, n28, n38, n40, n41, n77, n78: PCI, SSB index / beam, SS-RSRP / RSRQ / SINR, MIB (SFN, subcarrier spacing, k_SSB, CORESET#0, barred) (receive only, public broadcast)", "5G NR cells: the public broadcast of NR cells (PSS / SSS / PBCH, MIB)", N_("5G NR, GSCN scan"), ImVec4(0.56f, 0.74f, 0.96f, 1), nullptr, "5g nr new radio gnb cell tower ssb pss sss pbch mib gscn pci beam n78 n77 n28 n20 3.5 ghz sa nsa"},
    {69, "NB-IoT cells", 6, IM_COL32(60, 112, 124, 255), "LTE NB-IoT cells around you (180 kHz carriers, in-band, guard band or standalone) from their NPSS / NSSS, NPBCH (MIB-NB) and SIB1-NB: PCI, operating mode, antenna ports, SFN, operator (MCC / MNC), tracking area, cell identity, band, level; band scan over the 100 kHz raster. Receive only: the public broadcast only, nothing addressed to a device is decoded", "NB-IoT cells: the public broadcast information of narrowband IoT carriers (receive only)", N_("NB-IoT, band scan"), ImVec4(0.46f, 0.80f, 0.86f, 1), nullptr, "nb-iot nbiot narrowband iot lte-m cellular iot npss nsss npbch mib-nb sib1-nb pci operator tac band 20 8 3 800 900 1800 mhz 4g"},
    {39, "GSM-R", 6, IM_COL32(110, 84, 70, 255), "Railway GSM base stations (GSM-R, 921 - 925 MHz): cell identity, location area, operator, channel lists (receive only, public broadcast)", "GSM-R: the public broadcast information of railway GSM cells", N_("Railway, 921 - 925 MHz"), ImVec4(0.90f, 0.70f, 0.52f, 1), nullptr, "gsm-r gsmr gsm railway train rail bcch arfcn bsic lac etcs"},
    {81, "LBJ", 6, IM_COL32(118, 78, 62, 255), "LBJ (Chinese railway train proximity warning) on about 821.2375 MHz: FM voice channel with 1200 bit/s FSK data for train number, kilometre marker, locomotive ID, direction and speed (receive only)", "LBJ: railway train warning broadcasts (821 MHz)", N_("Train warning, 821 MHz"), ImVec4(0.92f, 0.68f, 0.48f, 1), nullptr, "lbj chinese railway train warning 821 821.2375 cr-lbj locomotive km marker kilometre 列车接近预警"},
    {23, "POCSAG", 7, IM_COL32(110, 110, 50, 255), "POCSAG paging messages (512, 1200 and 2400 bd) on a 25 kHz channel", "POCSAG pager messages", N_("Pagers, 512-2400 bd"), ImVec4(0.84f, 0.84f, 0.44f, 1), "pocsag", "pocsag pager paging"},
    {23, "FLEX", 7, IM_COL32(120, 96, 44, 255), "FLEX paging messages (1600, 3200 and 6400 bit/s) on a 25 kHz channel", "FLEX pager messages", N_("Pagers, 1600-6400"), ImVec4(0.90f, 0.74f, 0.40f, 1), "flex", "flex pager paging"},
    {28, "ISM sensors", 8, IM_COL32(96, 110, 60, 255), "Everyday 433.92, 868 and 915 MHz devices: weather stations, tyre pressure sensors, remotes, energy and water meters, Z-Wave (passive; S0 and S2 stay encrypted)", "ISM sensors: weather stations, TPMS, remotes and meters around you (the rtl_433 idea)", N_("Sensors, 433 / 868 MHz"), ImVec4(0.78f, 0.86f, 0.48f, 1), nullptr, "rtl_433 weather station thermometer tpms tyre tire remote doorbell energy meter m-bus 433 868 915 z-wave zwave home automation"},
    {20, "LoRaWAN", 8, IM_COL32(130, 110, 40, 255), "LoRaWAN monitor, EU868 (868.1 / 868.3 / 868.5 MHz) or US915, SF7 - SF12: message types, DevAddr and network, frame counters, MAC commands, join requests (only the clear headers; payloads stay encrypted)", "LoRaWAN: IoT devices around you, from the headers every gateway receives", N_("IoT, 868.1 MHz"), ImVec4(0.96f, 0.84f, 0.40f, 1), "lorawan", "lorawan lora wan iot ttn the things network helium chirpstack gateway devaddr deveui join 868 915 sensor"},
    {20, "Meshtastic", 8, IM_COL32(50, 120, 100, 255), "Meshtastic LoRa mesh messages, positions and telemetry (868 and 915 MHz bands)", "Meshtastic: LoRa mesh network nodes and messages", N_("LoRa mesh, 869.525 MHz"), ImVec4(0.46f, 0.84f, 0.70f, 1), "meshtastic", "meshtastic mesh lora"},
    {20, "MeshCore", 8, IM_COL32(140, 96, 48, 255), "MeshCore LoRa mesh adverts and messages (868 and 915 MHz bands)", "MeshCore: LoRa mesh network nodes and messages", N_("LoRa mesh, 869.618 MHz"), ImVec4(0.96f, 0.68f, 0.34f, 1), "meshcore", "meshcore mesh lora"},
    {51, "Bluetooth LE", 8, IM_COL32(52, 96, 150, 255), "Bluetooth Low Energy advertising on channels 37, 38 and 39 (2402 / 2426 / 2480 MHz): device names, vendors, services, iBeacon and Eddystone, like a scanner app (receive only: never connects or pairs)", "Bluetooth LE: the advertising packets devices broadcast openly", N_("Advertising, 2.4 GHz"), ImVec4(0.48f, 0.70f, 0.98f, 1), nullptr, "bluetooth ble le low energy advertising beacon ibeacon eddystone nrf connect scanner 2.4 ghz 2402 exposure notification"},
    {65, "Zigbee / 802.15.4", 8, IM_COL32(60, 120, 100, 255), "Zigbee and IEEE 802.15.4 on the 2.4 GHz channels 11 - 26 (O-QPSK, 2 Mchip/s, 250 kbit/s): networks (PAN ID, extended PAN ID, coordinator), devices with vendor from the address, MAC / NWK / APS headers, beacons and commands, a live frame log; channel scan. Receive only: encrypted payloads are not decrypted (the network key is not obtainable), only frames sent in the clear are shown. Needs a fast radio (4 Msps or more), not an RTL-SDR", "Zigbee / 802.15.4: networks and devices from the headers every sniffer sees (receive only, no decryption; needs a fast 2.4 GHz radio)", N_("IoT, 2.4 GHz"), ImVec4(0.46f, 0.84f, 0.70f, 1), nullptr, "zigbee 802.15.4 ieee 802154 thread zigbee3 zha hue ikea tradfri aqara xiaomi lumi smartthings pan coordinator router o-qpsk 2405 2.4 ghz iot sensor smart home mesh channel 11 15 20 25 26 beacon"},
    {70, "Wi-Fi HaLow (802.11ah)", 8, IM_COL32(70, 128, 112, 255), "Wi-Fi HaLow (802.11ah) networks in the sub-GHz bands, 863 - 868 MHz in Europe and 902 - 928 MHz in the US: S1G beacons at 1, 2 and 4 MHz channel widths give the SSID, BSSID, vendor, channel, bandwidth, MCS, level and the security type the network announces (information only). EU and US channel presets and a channel scan. Receive only: nothing is sent or joined and nothing is decrypted. A 1 or 2 MHz channel works from 2.4 Msps (an RTL-SDR), 4 MHz needs a radio with 4 Msps or more", "Wi-Fi HaLow / 802.11ah: the long-range sub-GHz networks around you from their beacons (receive only, no decryption)", N_("IoT, sub-GHz"), ImVec4(0.50f, 0.84f, 0.72f, 1), nullptr, "wifi halow 802.11ah s1g sub-ghz sub ghz 868 915 863 902 928 long range iot sensor camera ssid bssid network beacon 1 mhz 2 mhz 4 mhz wi-fi"},
    {74, "Datawell Buoy HF Link", 12, IM_COL32(52, 92, 120, 255), "Datawell Waverider wave-buoy telemetry on HF, 25.5 - 35.5 MHz (up to 45 MHz on request), heard as a USB signal. The 2FSK format of the DWR-MkIII, DWR-G and WR-SG buoys (81.92 bit/s) is decoded from Datawell's published reference manual: heave / north / west displacements, wave height and period, water temperature, GPS position and the 64 band directional spectrum, with the buoy's error-correcting code repairing bit errors. The 4FSK format of the DWR4 (163.84 bit/s) is not public: only the carrier, its tones and measurements of the bit stream are shown. Listed channel presets. Receive only", "Datawell Buoy HF Link: wave heights, periods and spectra from wave buoys on HF (receive only)", N_("Wave buoys, HF"), ImVec4(0.52f, 0.76f, 0.90f, 1), nullptr, "datawell waverider buoy wave buoy dwr dwr4 mkiii wr-sg hxv hva fsk 2fsk 4fsk hf link 29 mhz 30 mhz 35 mhz telemetry ocean waves wave height significant"},
    {72, "CODAR / HF ocean radar", 12, IM_COL32(56, 104, 128, 255), "CODAR SeaSonde, WERA and similar coastal HF ocean radars: the FMCW sweeps in the 4.4 - 4.65, 5.25, 12 - 14, 24.45 - 24.65 and 41.5 - 42.5 MHz bands, found as moving lines in the spectrum: centre, sweep width, rate, direction, repetition and level, a table of the radars heard with first / last heard and SNR history, a chirp waterfall with the sweeps marked, the band and time each was heard (propagation, no position claimed), band presets and a scan over the radar bands. The Morse call sign some radars send is decoded from the keyed sweep width and always labelled unverified. Receive only: broadcast radar, nothing is decrypted", "CODAR / HF ocean radar: the coastal radar chirps on the HF bands (receive only)", N_("Radar chirps, HF"), ImVec4(0.56f, 0.80f, 0.92f, 1), nullptr, "codar seasonde wera hf radar ocean radar oceanographic fmcw chirp sweep sweeps current surface 4 mhz 13 mhz 25 mhz 42 mhz call sign morse cw id ilfm"},
    {75, "Multitone Paging", 7, IM_COL32(104, 96, 56, 255), "Multitone Electronics (UK) pagers, Mk IV to VII, in hospitals and industrial sites, 25 - 470 MHz, narrowband FM with FFSK: a detector, not a decoder, because the format is not public. Per transmission: the carrier, the FM deviation, the steady tones and spectral lines of the audio and, when the audio is two-level FSK, the measured baud rate, the bit stream in hex and how many 32-bit words pass the POCSAG BCH(31,21) check, with the POCSAG sync word flagged. Everything is labelled unverified; no message text is shown. Receive only: pager messages are private, nothing is decrypted or transmitted", "Multitone Paging: tone and FFSK pagers for hospitals and sites, detector only (receive only)", N_("Pagers, tones and FFSK"), ImVec4(0.80f, 0.74f, 0.44f, 1), nullptr, "multitone paging pager hospital tone sequence ffsk fsk pocsag bch mk iv v vi vii uk"},
    {76, "NOAA Weather Radio SAME", 9, IM_COL32(176, 96, 64, 255), "NOAA Weather Radio (US and Canada) SAME / EAS alerts on the seven narrow-FM weather channels, 162.400 - 162.550 MHz: the 520.83 baud AFSK header sent three times is voted bit by bit and every field checked, then shown in plain English: originator, event (tornado warning, severe thunderstorm warning, required weekly test and the rest of the EAS table), the locations with the partial-county direction, valid for, issue time and sender call sign, how many of the three repeats agreed, the 1050 Hz attention tone, an alert history and a map of the states and provinces named (approximate). County names come only from a county table you load (none are built in). Receive only: the text of an alert is a public broadcast", "NOAA Weather Radio SAME: weather and emergency alerts, decoded and explained (receive only)", N_("Weather alerts, 162.55 MHz"), ImVec4(0.96f, 0.68f, 0.50f, 1), nullptr, "same eas noaa weather radio nwr wx alert alerts warning watch tornado severe thunderstorm flood hurricane required weekly test rwt 162.400 162.425 162.450 162.475 162.500 162.525 162.550 attention tone 1050 emergency alert system canada weatheradio"},
    {77, "ANT+ sensors", 8, IM_COL32(96, 140, 72, 255), "ANT+ sensors on 2457 MHz (and other ANT networks on their own channel): heart rate straps and watches, bike speed, cadence and power sensors, footpods, environment sensors, as a watch or bike computer hears them: 1 Mbit/s GFSK frames with the device number, type and the data page, the heart rate with the beat count and R-R interval plotted over time, power, cadence, speed, temperature, manufacturer, serial and battery pages. Receive only: nothing is sent, paired or acknowledged, and only the open broadcasts are shown. Needs a radio that reaches 2.4 GHz at 4 Msps (HackRF, Pluto, LimeSDR, BladeRF, USRP)."},
    {78, "DECT cordless phones", 8, IM_COL32(120, 88, 140, 255), "DECT cordless phones and base stations on the ten carriers 1881.792 - 1897.344 MHz (Europe; 1.152 Mbit/s GFSK, 10 ms frames of 24 slots): every base station from the beacon it sends every frame, with its RFPI (the manufacturer code and system number inside it), level and the slots it uses, the system information tails and a burst log with base and handset bursts. Activity and signalling only: calls are encrypted and the voice is not decoded, nothing is transmitted. Needs a radio that reaches 1.9 GHz at 5 Msps or more (HackRF, Pluto, LimeSDR, BladeRF, USRP, SDRplay)."},
    {84, "Wi-SUN FAN", 8, IM_COL32(70, 112, 140, 255), "Wi-SUN FAN smart utility networks (smart meters, street lights) (IEEE 802.15.4g FSK at 50, 100 and 150 kbit/s) with the band plans of every Wi-SUN region from 470 MHz to 2.4 GHz: a whole band at once (the nodes hop over every channel, all of 915 - 928 MHz at 16 Msps), networks with their name and border router, nodes with vendor, role, routing cost and schedules, links with the levels acks report, a frame log and a hop chart. Receive only: nothing is sent or joined; PAN configurations and data are encrypted and stay so. Needs 16 Msps or more for the whole band (a few channels from 2 Msps)", "Wi-SUN FAN: smart meter and street light mesh networks, nodes and links (receive only, no decryption)", N_("IoT, sub-GHz"), ImVec4(0.48f, 0.74f, 0.90f, 1), nullptr, "wi-sun wisun fan smart meter smart grid utility street light 802.15.4g sun fsk 915 920 921 928 anz australia new zealand mesh rpl 6lowpan pan border router eapol landis itron"},
    {71, "LoRa (raw)", 8, IM_COL32(120, 104, 40, 255), "Plain LoRa between two radios (point to point, not LoRaWAN, not a mesh): every frame as bytes (hex and text) with SNR, level and carrier offset. You set the spreading factor (SF5 - SF12), bandwidth (125 / 250 / 500 kHz), coding rate and CRC (implicit header), header mode and sync word, or leave the spreading factor and bandwidth on Auto and every combination is searched. EU868, US915 and 433 MHz presets. Receive only: nothing is sent, decrypted or interpreted", "LoRa (raw): point-to-point LoRa frames as hex and text, SF / bandwidth set or searched (receive only, no decryption)", N_("IoT, sub-GHz"), ImVec4(0.86f, 0.76f, 0.38f, 1), nullptr, "lora raw p2p point to point sx127x sx126x sx1276 sx1262 rfm95 ra-02 chirp spread spectrum css 433 868 915 eu868 us915 sf7 sf12 spreading factor implicit header sync word 0x12 arduino esp32 heltec ttgo"},
    {54, "Wi-Fi networks", 8, IM_COL32(48, 110, 120, 255), "Wi-Fi access points on 2.4 GHz channels 1 - 13 from their beacons (802.11b DSSS, 1 Mbit/s): SSID, BSSID and vendor, channel, security (open, WEP, WPA, WPA2, WPA3), rates, beacon interval and level, like a phone's Wi-Fi list (passive: beacons only, never joins or transmits)", "Wi-Fi networks: the access points around you, from the beacons they broadcast", N_("Beacons, 2.4 GHz"), ImVec4(0.46f, 0.84f, 0.90f, 1), nullptr, "wifi wi-fi wlan 802.11 802.11b access point ap ssid bssid beacon probe response wpa wpa2 wpa3 wep scanner 2.4 ghz channel dsss"},
    {13, "Radiosonde", 9, IM_COL32(60, 112, 150, 255), "Weather balloon radiosondes, 400 to 406 MHz (RS41, DFM, M10 and others)", "Radiosonde weather balloons: position, altitude and weather data", N_("Balloons, 403 MHz"), ImVec4(0.46f, 0.74f, 0.94f, 1)},
    {58, "Time signals", 9, IM_COL32(90, 100, 70, 255), "Time and frequency stations: DCF77 (77.5 kHz), MSF and WWVB (60 kHz), JJY (40 / 60 kHz), WWV / WWVH (2.5 - 20 MHz): the time, date, summer time and leap second flags, the seconds tick, and how far the computer clock is off after the path delay", "Time signals: the time stations and your computer clock against them", N_("LF and HF time codes"), ImVec4(0.74f, 0.82f, 0.56f, 1), nullptr, "time signal clock dcf77 msf wwvb jjy wwv wwvh atomic clock radio clock time code lf 77.5 khz 60 khz 40 khz utc leap second dst summer time ntp offset"},
    {52, "Ionospheric sounders", 9, IM_COL32(110, 70, 120, 255), "HF chirp sounders (ionosondes and oblique sounders, 0 - 30 MHz at 100 - 500 kHz/s): sweeps found by dechirping, their rate, period and start offset, an ionogram of group range against frequency, a timeline, PNG / CSV export; no decoder", "Ionospheric sounders: chirp ionosondes and the ionogram they give", N_("Chirp sweeps, 0 - 30 MHz"), ImVec4(0.86f, 0.62f, 0.94f, 1), nullptr, "ionosonde ionospheric sounder chirp chirpsounder oblique sounding ionogram fmcw sweep hf propagation virtual height group range f layer muf codar"},
    {48, "Radio astronomy", 9, IM_COL32(60, 70, 130, 255), "Radio astronomy: the 21 cm hydrogen line (1420.4058 MHz) and the OH lines; long integration, on - off reference, baseline fit, radial velocity, drift-scan waterfall, CSV export; no decoder", "Radio astronomy: the hydrogen line of the Milky Way", N_("Hydrogen line, 1420 MHz"), ImVec4(0.66f, 0.66f, 0.98f, 1), nullptr, "radio astronomy hydrogen line 21 cm h1 hi neutral hydrogen oh maser milky way galaxy radial velocity doppler lsr telescope dish horn integration"},
    {25, "FT8", 10, IM_COL32(110, 70, 120, 255), "FT8 weak-signal contacts in 15 s slots, 14.074 MHz", "FT8: weak-signal amateur contacts", N_("WSJT, 14.074 MHz"), ImVec4(0.86f, 0.58f, 0.92f, 1), "ft8", "ft8 wsjt hf digital"},
    {25, "FT4", 10, IM_COL32(120, 76, 96, 255), "FT4 contest mode in 7.5 s slots, 14.080 MHz", "FT4: fast weak-signal amateur contacts", N_("WSJT, 14.080 MHz"), ImVec4(0.92f, 0.60f, 0.70f, 1), "ft4", "ft4 wsjt hf digital"},
    {25, "FT2", 10, IM_COL32(96, 76, 130, 255), "FT2 in 3.75 s slots, 14.084 MHz", "FT2: the fastest of the FT modes", N_("WSJT, 14.084 MHz"), ImVec4(0.66f, 0.58f, 0.96f, 1), "ft2", "ft2 wsjt hf digital"},
    {25, "WSPR", 10, IM_COL32(80, 96, 120, 255), "WSPR propagation beacons in 2 minute slots, 14.0956 MHz", "WSPR: weak-signal propagation reports", N_("Beacons, 14.0956 MHz"), ImVec4(0.58f, 0.72f, 0.86f, 1), "wspr", "wspr wsjt hf digital beacon propagation"},
    {25, "JT65", 10, IM_COL32(150, 90, 80, 255), "JT65 weak-signal contacts (65-FSK, Reed-Solomon, 60 s periods, sub-modes A, B, C), 14.076 MHz", "JT65: weak-signal amateur contacts and EME", N_("WSJT, 14.076 MHz"), ImVec4(0.96f, 0.62f, 0.56f, 1), "jt65", "jt65 wsjt hf digital eme"},
    {25, "JT9", 10, IM_COL32(70, 120, 115, 255), "JT9 weak-signal contacts (9-FSK, convolutional code, 60 s periods, 15 Hz wide), 14.078 MHz", "JT9: narrow weak-signal amateur contacts", N_("WSJT, 14.078 MHz"), ImVec4(0.56f, 0.90f, 0.84f, 1), "jt9", "jt9 wsjt hf digital"},
    {25, "Q65", 10, IM_COL32(140, 120, 60, 255), "Q65 (A to E, 15 s to 5 min periods) for EME, scatter and weak-signal work: Q-ary repeat-accumulate code, 65-FSK", "Q65: weak-signal, EME and scatter contacts", N_("WSJT, 14.100 MHz"), ImVec4(0.98f, 0.86f, 0.52f, 1), "q65", "q65 wsjt hf digital eme scatter"},
    {25, "FST4", 10, IM_COL32(86, 90, 130, 255), "FST4 and FST4W (15 s to 30 min periods, 4-GFSK, LDPC (240,101) / (240,74)) for LF, MF and HF weak-signal contacts and beacons", "FST4: slow weak-signal contacts and FST4W beacons", N_("WSJT, 1.8366 MHz"), ImVec4(0.70f, 0.75f, 0.98f, 1), "fst4", "fst4 fst4w wsjt hf digital beacon lf mf"},
    {25, "MSK144", 10, IM_COL32(120, 80, 100, 255), "MSK144 meteor scatter (72 ms frames, 2000 baud, 15 s periods): decodes the short bursts of meteor trails", "MSK144: meteor scatter bursts", N_("Meteor scatter, 28.145 MHz"), ImVec4(0.92f, 0.66f, 0.82f, 1), "msk144", "msk144 wsjt meteor scatter"},
    {36, "PSK31", 10, IM_COL32(60, 100, 120, 255), "PSK31 / PSK63 keyboard chat (BPSK and QPSK): every signal in the 3 kHz passband at once, 14.070 MHz", "PSK31: keyboard chat, every signal in the passband", N_("Keyboard chat, 14.070 MHz"), ImVec4(0.50f, 0.80f, 0.92f, 1), nullptr, "psk31 psk63 bpsk qpsk psk varicode keyboard chat hf amateur digital"},
    {37, "JS8Call", 10, IM_COL32(90, 80, 130, 255), "JS8Call keyboard messaging (8-FSK, normal / fast / turbo slots): heartbeats, directed messages, relays, 14.078 MHz", "JS8Call: weak-signal messages, heartbeats and relays", N_("Messaging, 14.078 MHz"), ImVec4(0.70f, 0.64f, 0.96f, 1), nullptr, "js8 js8call ft8 weak signal messaging heartbeat relay grid hf amateur digital"},
    {25, "RTTY", 10, IM_COL32(100, 80, 130, 255), "RTTY teletype (45.45 bd, 170 Hz shift) on upper sideband, 1 to 30 MHz", "RTTY: radio teletype text", N_("Teletype, 14.080 MHz"), ImVec4(0.72f, 0.62f, 0.94f, 1), "rtty", "rtty hf digital teletype"},
    {34, "CW", 10, IM_COL32(110, 96, 60, 255), "CW (Morse) skimmer: every signal in a 14 kHz window, 5 to 50 WPM, on the HF CW segments", "CW: Morse code from every signal in the window, speed and callsigns", N_("Morse, 14.020 MHz"), ImVec4(0.92f, 0.80f, 0.52f, 1), nullptr, "cw morse skimmer hf amateur telegraphy"},
    {25, "SSTV", 10, IM_COL32(130, 70, 100, 255), "SSTV slow-scan TV pictures (Martin, Scottie, Robot, PD) on upper sideband", "SSTV: slow-scan television pictures", N_("Pictures, 14.230 MHz"), ImVec4(0.94f, 0.56f, 0.76f, 1), "sstv", "sstv hf digital pictures"},
    {25, "HamDRM", 10, IM_COL32(120, 84, 70, 255), "HamDRM / EasyPal digital SSTV: pictures as files over DRM-style OFDM in 2.3 or 2.5 kHz of upper sideband, 14.233 MHz", "HamDRM: digital SSTV pictures (EasyPal, QSSTV DRM)", N_("Digital SSTV, 14.233 MHz"), ImVec4(0.92f, 0.66f, 0.56f, 1), "hamdrm", "hamdrm easypal qsstv drm digital sstv dsstv pictures jpeg hf"},
    {25, "Olivia", 10, IM_COL32(86, 100, 130, 255), "Olivia MFSK keyboard mode with Walsh-Hadamard coding, weak-signal text, 14.1065 MHz", "Olivia: text that gets through noise", "Keyboard, 14.1065 MHz", ImVec4(0.62, 0.72, 0.94, 1), "olivia", "olivia mfsk keyboard hf digital text"},
    {25, "Contestia", 10, IM_COL32(96, 96, 130, 255), "Contestia (Olivia with 6-bit characters, upper case, faster), 14.1095 MHz", "Contestia: Olivia's faster cousin", "Keyboard, 14.1095 MHz", ImVec4(0.70, 0.70, 0.96, 1), "contestia", "contestia olivia keyboard hf digital text"},
    {25, "MFSK", 10, IM_COL32(90, 112, 100, 255), "MFSK4 to MFSK64 keyboard modes (MFSK16 is the usual one) with forward error correction, 14.0785 MHz", "MFSK: error-corrected keyboard text", "Keyboard, 14.0785 MHz", ImVec4(0.66, 0.86, 0.72, 1), "mfsk", "mfsk mfsk16 mfsk32 keyboard hf digital text"},
    {25, "THOR", 10, IM_COL32(120, 92, 70, 255), "THOR keyboard modes (4 to 100) with incremental frequency keying and FEC, 14.077 MHz", "THOR: robust keyboard text", "Keyboard, 14.077 MHz", ImVec4(0.94, 0.76, 0.58, 1), "thor", "thor keyboard hf digital text"},
    {25, "DominoEX", 10, IM_COL32(110, 84, 100, 255), "DominoEX keyboard modes (4 to 22) with incremental frequency keying, 14.0715 MHz", "DominoEX: keyboard text that ignores tuning", "Keyboard, 14.0715 MHz", ImVec4(0.90, 0.68, 0.86, 1), "dominoex", "dominoex domino keyboard hf digital text"},
    {25, "Hellschreiber", 10, IM_COL32(100, 100, 84, 255), "Hellschreiber (Feld Hell, Slow Hell, Hell 80, HellX5): text as a picture of dots, 14.063 MHz", "Hell: text drawn as a picture", "Keyboard, 14.063 MHz", ImVec4(0.86, 0.86, 0.66, 1), "hell", "hell hellschreiber feld slow hell80 keyboard hf digital picture"},
    {25, "FreeDV", 11, IM_COL32(70, 100, 130, 255), "FreeDV digital voice (700D, 700E, 1600) on upper sideband", "FreeDV: open digital voice on HF", N_("Digital voice, 14.236 MHz"), ImVec4(0.52f, 0.72f, 0.94f, 1), "freedv", "freedv hf digital voice"},
    {31, "M17", 11, IM_COL32(60, 110, 90, 255), "M17 open digital voice and data (4FSK, 9 kHz), 2 m and 70 cm", "M17: open digital voice, text messages and positions on VHF / UHF", N_("Digital voice, 433.475 MHz"), ImVec4(0.48f, 0.86f, 0.66f, 1), nullptr, "m17 digital voice codec2 amateur vhf uhf"},
    {33, "D-STAR", 11, IM_COL32(70, 90, 130, 255), "D-STAR digital voice (GMSK 4800 bit/s, 6.25 kHz), 2 m and 70 cm", "D-STAR: callsigns, repeaters, messages and D-PRS positions on VHF / UHF", N_("Digital voice, 145.375 MHz"), ImVec4(0.56f, 0.70f, 0.96f, 1), nullptr, "dstar d-star dv digital voice ambe icom jarl d-prs amateur vhf uhf repeater"},
    {24, "APRS (AFSK)", 11, IM_COL32(90, 110, 60, 255), "APRS and AX.25 packet radio (1200 bd AFSK), 144.800 MHz in Europe; LoRa APRS is its own entry", "APRS and AX.25 packet radio, 1200 bd AFSK on VHF (not LoRa APRS)", N_("AFSK, 144.8 MHz"), ImVec4(0.70f, 0.86f, 0.46f, 1), nullptr, "aprs packet ax25 afsk"},
    {20, "LoRa APRS", 11, IM_COL32(50, 110, 120, 255), "LoRa APRS on 70 cm, 433.775 MHz in Europe (SF12, 125 kHz)", "LoRa APRS: APRS stations over LoRa on 433 MHz", N_("LoRa, 433.775 MHz"), ImVec4(0.55f, 0.70f, 0.98f, 1), "loraaprs", "lora aprs 433"},
    {20, "MeshCom", 11, IM_COL32(110, 70, 120, 255), "MeshCom LoRa nodes on 70 cm, 433.175 MHz in Europe", "MeshCom: amateur LoRa mesh messages and positions on 433 MHz", N_("LoRa, 433.175 MHz"), ImVec4(0.86f, 0.58f, 0.92f, 1), "meshcom", "meshcom lora mesh 433"},
    {49, "HF ALE", 12, IM_COL32(100, 90, 70, 255), "HF ALE (2G, MIL-STD-188-141 / FED-STD-1045): 8-FSK link establishment calls, soundings, AMD messages and LQA on the HFN channels, 14.109 MHz", "HF ALE: who calls whom, soundings and AMD messages", N_("Link establishment, 14.109 MHz"), ImVec4(0.90f, 0.76f, 0.56f, 1), nullptr, "ale 2g hf automatic link establishment 188-141 fed-std-1045 hfn sounding amd lqa amateur digital"},
    {67, "HF beacons", 12, IM_COL32(110, 96, 64, 255), "HF beacon propagation monitor: which NCDXF/IARU International Beacon Project beacons (18 beacons on 14.100, 18.110, 21.150, 24.930 and 28.200 MHz, 10 s slots), Australian Long Dashes and Russian single-letter beacons can be heard from here, by band, with SNR, distance and bearing, a great-circle map and 24 hour history; an editable beacon list; clicking a beacon tunes and listens", "HF beacons: what is open from here, band by band", N_("NCDXF/IARU, Long Dashes, single letters"), ImVec4(0.86f, 0.78f, 0.52f, 1), nullptr, "hf beacons beacon propagation ncdxf iaru ibp long dashes australia single letter russian marker priyom maximum usable open band 20m 15m 10m skip dx"},
    {73, "Signal workbench", 13, IM_COL32(88, 104, 150, 255), "Open a recording (WAV, MP3 or other audio, or an IQ file: .cs8 .cu8 .cf32 .sigmf) and take the signal apart without a radio: waveform and spectrogram with cursors, peaks, bandwidth, SNR, symbol rate, bursts and a modulation guess, then demodulate a chosen band (AM, FM, SSB, CW, ASK, FSK, AFSK, PSK, MSK) and read the bits", "Signal workbench: analyse, demodulate and read any recording", N_("Files: audio and IQ"), ImVec4(0.62f, 0.72f, 0.96f, 1), nullptr, "signal workbench analyser analyzer inspectrum sigdigger universal radio hacker urh recording file wav mp3 iq cs8 cu8 cf32 sigmf demodulate bits eye constellation baud symbol rate fsk psk ask afsk"},
    {55, "General receiver", 13, IM_COL32(64, 104, 112, 255), "Listen anywhere in the band: NFM, WFM (mono), AM, USB, LSB and CW, click the spectrum to tune, filter width, squelch, AGC, de-emphasis, S-meter and audio recording to WAV", "General receiver: NFM, WFM, AM, USB, LSB and CW anywhere", N_("NFM / AM / SSB / CW"), ImVec4(0.52f, 0.82f, 0.86f, 1), nullptr, "general receiver scanner listen nfm fm wfm am usb lsb ssb cw morse bfo squelch agc s-meter record wav 2m 70cm pmr446 cb shortwave broadcast ham amateur sdr++ gqrx"},
    {41, "Spectrum explorer", 13, IM_COL32(70, 100, 120, 255), "Spectrum explorer: look around the band, measure carriers (bandwidth, power, SNR, symbol rate), sweep a range, LNB offsets, bookmarks; no decoder", "Spectrum explorer: look, measure and sweep", N_("Spectrum, waterfall, sweep"), ImVec4(0.55f, 0.80f, 0.95f, 1), nullptr, "spectrum analyzer analyser sweep waterfall measure bandwidth lnb survey scan"},
    {42, "Spectrum analyser", 13, IM_COL32(70, 100, 140, 255), "Wide sweeping spectrum analyser (1 - 6000 MHz in about a second with a HackRF): live, peak-hold and averaged traces, waterfall, markers, band overlay, CSV export", "Spectrum analyser: fast wide sweeps with markers and band labels", N_("Sweep, 1 - 6000 MHz"), ImVec4(0.55f, 0.75f, 0.98f, 1), nullptr, "spectrum analyser analyzer sweep hackrf_sweep wideband scan peak hold waterfall rf survey"},
};
const int kNumModes = (int)(sizeof kModes / sizeof *kModes);
const char* const kGroupNames[kNumGroups] = {N_("TV"), N_("BROADCAST RADIO"), N_("AVIATION"), N_("MARITIME"), N_("SATELLITE & NAVIGATION"), N_("TWO-WAY RADIO"), N_("CELLULAR & RAIL"), N_("PAGERS"), N_("IoT & SENSORS"), N_("WEATHER & SCIENCE"), N_("AMATEUR - DIGITAL"), N_("AMATEUR - VOICE & PACKET"), N_("HF UTILITY"), N_("GENERAL & TOOLS")};
// a thin accent per group heading in the mode lists (UI only: nothing stores a group number)
const ImU32 kGroupCols[kNumGroups] = {
    IM_COL32(92, 188, 220, 255), IM_COL32(92, 210, 148, 255), IM_COL32(132, 160, 240, 255), IM_COL32(80, 170, 210, 255),
    IM_COL32(186, 150, 236, 255), IM_COL32(150, 206, 104, 255), IM_COL32(226, 168, 112, 255), IM_COL32(214, 206, 96, 255),
    IM_COL32(110, 214, 196, 255), IM_COL32(120, 150, 230, 255), IM_COL32(218, 130, 214, 255), IM_COL32(236, 128, 140, 255),
    IM_COL32(204, 176, 132, 255), IM_COL32(150, 162, 178, 255)};
float gSwitchWidth = 420;   // width of the mode selector as drawn (the guided tour points at it)

void selectMode(App& a, int fam, const char* subId) {
    if (txViewFlag()) { txSelectMode(a, fam, subId); return; }
    const int sub = subIndexOf(subId);
    if (fam == a.family && (sub == a.sub || !sub)) return;
    // Switch in one action: stop the old decoder before updating the mode, then
    // restart using the new mode\x27s own frequency, sample rate and bandwidth.
    const bool wasRunning = a.engine.running();
    if (wasRunning) a.engine.stop();
    setMode(a, fam, sub);
    savePrefs(a);
    if (wasRunning) {
        a.engine.log("switching receiver mode");
        startReceiver(a);
    }
}
bool modeSelected(const App& a, const ModeDef& m) {
    if (txViewFlag()) return txModeHighlighted(m);
    return m.family == a.family && (!m.subId || subIndexOf(m.subId) == a.sub);
}
bool modeMatches(const ModeDef& m, const char* q) {
    if (!q || !*q) return true;
    auto has = [&](const char* s) {
        if (!s) return false;
        std::string h(s), n(q);
        for (auto& c : h) c = (char)tolower((unsigned char)c);
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        return h.find(n) != std::string::npos;
    };
    return has(m.name) || has(m.sub) || has(tr(m.sub)) || has(m.blurb) || has(m.keys);
}
// a search box at the top of a mode drop-down; returns the query
const char* modeSearchBox() {
    static char q[32] = "";
    if (ImGui::IsWindowAppearing()) { q[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modeq", tr("search (FT8, LoRa, APRS...)"), q, sizeof q);
    return q;
}

// Mode selector: the modes in their groups (kGroupNames) as one segmented control each; a drop-down when the window is too narrow for them all
void standardSwitch(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() - 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ModeDef* cur = &kModes[0];
    for (const auto& m : kModes) if (modeSelected(a, m)) cur = &m;

    std::vector<float> segW((size_t)kNumModes), segX((size_t)kNumModes);   // one per mode, however many there are
    float capX[kNumGroups] = {}, grpL[kNumGroups] = {}, grpR[kNumGroups] = {}, capW[kNumGroups];
    for (int g = 0; g < kNumGroups; g++) capW[g] = ImGui::CalcTextSize(tr(kGroupNames[g])).x;
    float x = p.x;
    for (int g = 0; g < kNumGroups; g++) {
        capX[g] = x; x += capW[g] + 8;
        grpL[g] = x;
        for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g) { segW[i] = ImGui::CalcTextSize(kModes[i].name).x + 22; segX[i] = x; x += segW[i]; }
        grpR[g] = x;
        x += 18;
    }
    const float total = x - 18 - p.x;
    const float room = ImGui::GetContentRegionAvail().x - 150 * gUi;   // keep the right-hand buttons clear
    if (total > room || getenv("DECT2_NARROW")) {   // too narrow: one drop-down with the same groups
        gSwitchWidth = 170 * gUi;
        ImGui::SetNextItemWidth(gSwitchWidth);
        if (ImGui::BeginCombo("##mode", cur->name)) {
            const char* q = modeSearchBox();
            for (int g = 0; g < kNumGroups; g++) {
                bool any = false;
                for (const auto& m : kModes) any |= m.group == g && modeMatches(m, q);
                if (!any) continue;
                ImGui::Separator();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g]));
                for (const auto& m : kModes) if (m.group == g && modeMatches(m, q)) {
                    ImGui::PushID(&m);
                    if (ImGui::Selectable(m.name, modeSelected(a, m))) selectMode(a, m.family, m.subId);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tip);
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 14 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", cur->blurb);
        return;
    }
    gSwitchWidth = total;
    ImGui::AlignTextToFramePadding();
    for (int g = 0; g < kNumGroups; g++) {
        const ImVec2 ts = ImGui::CalcTextSize(tr(kGroupNames[g]));
        dl->AddText(ImVec2(capX[g], p.y + (h - ts.y) * 0.5f), (kGroupCols[g] & 0x00FFFFFF) | 0xB0000000, tr(kGroupNames[g]));
        dl->AddRectFilled(ImVec2(grpL[g], p.y), ImVec2(grpR[g], p.y + h), IM_COL32(18, 22, 28, 255), 3.f);
        dl->AddRect(ImVec2(grpL[g], p.y), ImVec2(grpR[g], p.y + h), IM_COL32(52, 60, 72, 255), 3.f);
    }
    int selPos = 0;
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) selPos = i;
    static float knobX = -1, knobW = 0;
    if (knobX < 0) { knobX = segX[selPos]; knobW = segW[selPos]; }
    knobX += (segX[selPos] - knobX) * 0.35f; knobW += (segW[selPos] - knobW) * 0.35f;
    dl->AddRectFilled(ImVec2(knobX + 2, p.y + 2), ImVec2(knobX + knobW - 2, p.y + h - 2), kModes[selPos].col, 3.f);
    for (int i = 0; i < kNumModes; i++) {
        ImGui::SetCursorScreenPos(ImVec2(segX[i], p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW[i], h))) selectMode(a, kModes[i].family, kModes[i].subId);
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s", kModes[i].tip);
        ImGui::PopID();
        const ImVec2 ts = ImGui::CalcTextSize(kModes[i].name);
        dl->AddText(ImVec2(segX[i] + (segW[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), modeSelected(a, kModes[i]) ? IM_COL32(255, 255, 255, 255) : hov ? IM_COL32(220, 228, 236, 255) : IM_COL32(140, 152, 166, 255), kModes[i].name);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + total + 14, p.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cur->blurb);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y));
}
