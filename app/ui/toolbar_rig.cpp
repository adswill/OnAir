// Radios added by address: the Network radios window (PlutoSDR by IP, rtl_tcp, SoapyRemote, SpyServer, Hermes Lite 2,
// Red Pitaya) and the Hamlib rigs window (model search, serial port, baud rate), plus following a rig's dial.
// Split from toolbar.cpp.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/engine/updater.h"
#include "dect2/source/antenna_ports.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>
#include "toolbar_internal.h"
#include "../platform/platform.h"
#include <cstdio>
#include <fstream>
#include <sstream>

// The three texts of soapyRemoteModuleHelpFor() (core/src/net/netradio.cpp): listed here so the catalogs carry them; the error line below passes the text through tr().
[[maybe_unused]] static const char* const kSoapyRemoteModuleTexts[] = {
    N_("The SoapyRemote module is missing from this installation of OnAir, so it cannot reach a remote radio. Reinstall OnAir, and please report it if it happens again. "
       "The radio's computer must run SoapySDRServer --bind."),
    N_("The SoapyRemote module is missing from this installation of OnAir, so it cannot reach a remote radio. Reinstall OnAir (or run brew install soapyremote), and please report it if it happens again. "
       "The radio's computer must run SoapySDRServer --bind."),
    N_("SoapySDR here has no SoapyRemote client module, so it cannot reach a remote radio. Install it (Debian/Ubuntu: sudo apt install soapysdr-module-remote; "
       "other systems: github.com/pothosware/SoapyRemote), then restart OnAir. The radio's computer must run SoapySDRServer --bind."),
};

static bool netTypeSearches(int t) { return t == NetRadio::HermesLite2 || t == NetRadio::RedPitaya; }   // found by a discovery broadcast

// looks for Hermes Lite 2 (or Red Pitaya) radios, by the type chosen, on a worker thread (the broadcast waits 1.5 s for answers); netRadioWindow() takes the result
static void startNetDiscovery(App& a) {
    if (a.netDiscovery) return;
    auto d = std::make_shared<App::NetDiscovery>();
    a.netDiscovery = d;
    a.netFound.clear();
    a.netSearched = false;
    const bool rp = a.netAddType == NetRadio::RedPitaya;
    dect2::namedThread("onair-rigdisc", [d, rp] { d->found = rp ? discoverRedPitaya(1500) : discoverHermesLite2(1500); d->done = true; }).detach();
}

// a finished connection test or discovery: add the radio and pick it, or say why not
static void netRadioPoll(App& a) {
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
            memset(a.netAddPass, 0, sizeof a.netAddPass);
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
}

// the Search button and the radios a discovery broadcast found (Hermes Lite 2, Red Pitaya)
static void netRadioSearchUi(App& a) {
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

// the radios added so far, each with a Remove button
static void netRadioList(App& a) {
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Added radios (listed with the others)"));
    int drop = -1;
    ImGui::BeginDisabled(a.engine.running());
    for (int i = 0; i < (int)a.netRadios.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::SmallButton(tr("Remove"))) drop = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(netRadioDeviceName(a.netRadios[i]).c_str());
        if (a.netRadios[i].type == NetRadio::KiwiSdr) {   // what the receiver reports while it streams (the password is never shown)
            const KiwiStatus ks = kiwiStatus(netRadioHostPort(a.netRadios[i]));
            if (ks.connected) { ImGui::SameLine(); ImGui::TextDisabled(tr("S-meter %.0f dBm%s"), ks.rssiDbm, ks.adcOverload ? tr(", ADC overload") : ""); }
            else if (!ks.message.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", ks.message.c_str()); }
        }
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

void netRadioWindow(App& a) {
    netRadioPoll(a);
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
    static const char* kTypes[] = {"PlutoSDR (by IP)", "rtl_tcp", "SoapyRemote", "SpyServer", "Hermes Lite 2", "Red Pitaya", "SDR++ server", "SDRplay nRSP-ST (SDRconnect)", "KiwiSDR", "SoapySDR (device string)", "TCI (network)"};
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
    const bool argsType = a.netAddType == NetRadio::SoapyArgs;
    if (argsType) {
        snprintf(a.netAddHost, sizeof a.netAddHost, "%s", "custom");
        ImGui::SetNextItemWidth(360 * gUi);
        ImGui::InputTextWithHint(tr("Device string"), "type=uhd,addr=10.0.0.55", a.netAddRemote, sizeof a.netAddRemote);
        ImGui::TextDisabled("%s", tr("The SoapySDR device arguments exactly as the driver wants them (key=value, comma separated), for radios that are not found by themselves: "
                                     "driver=uhd,addr=10.0.0.55 or driver=remote,remote=tcp://host:55132. Connect adds it to the Radio list."));
    } else {
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint(tr("Address"), a.netAddType == NetRadio::Pluto ? "192.168.2.1" : netTypeSearches(a.netAddType) ? "192.168.1.20" : "192.168.1.20 or raspberrypi.local", a.netAddHost, sizeof a.netAddHost);
    if (netTypeSearches(a.netAddType)) netRadioSearchUi(a);
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputInt(tr("Port"), &a.netAddPort, 0, 0);
    a.netAddPort = std::max(0, std::min(65535, a.netAddPort));
    }
    if (a.netAddType == NetRadio::Pluto) {
        ImGui::SameLine(); ImGui::TextDisabled("%s", tr("0 = libiio's default"));
        ImGui::TextDisabled("%s", tr("Type the PlutoSDR's IP (USB gadget 192.168.2.1, or its Ethernet address) and Connect. It is then in the Radio list."));
    }
    if (a.netAddType == NetRadio::SdrppServer)
        ImGui::TextDisabled("%s", tr("SDR++ running as a server (sdrpp --server). It takes one client at a time: close SDR++'s own client first. "
                                     "Its source's settings (gain mode, bias tee, AGC, ...) show up under Radio settings once connected."));
    if (a.netAddType == NetRadio::NrspSt)
        ImGui::TextDisabled("%s", tr("The nRSP-ST is reached through SDRconnect: run SDRconnect (or its headless build) on a computer with the WebSocket server on (port 5454) and its nRSP-ST selected, "
                                     "and type that computer's address. No password is needed or stored."));
    if (a.netAddType == NetRadio::Tci)
        ImGui::TextDisabled("%s", tr("TCI: the Transceiver Control Interface of ExpertSDR3 (and SunSDR or other programs with a TCI server), port 50001. Turn its TCI server on and type "
                                     "that computer's address. OnAir only receives (the IQ stream, 48 to 384 kHz); it never transmits. No password is needed."));
    if (a.netAddType == NetRadio::KiwiSdr) {
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputTextWithHint(tr("Password"), tr("blank if the owner set none"), a.netAddPass, sizeof a.netAddPass, ImGuiInputTextFlags_Password);
        if (ImGui::Button(tr("Find KiwiSDR…"))) a.kiwiFindOpen = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("pick a public receiver from the list"));
        ImGui::PushTextWrapPos(400 * gUi);
        ImGui::TextDisabled("%s", tr("A KiwiSDR is a receiver on the internet that covers 0 to 30 MHz; OnAir takes its 12 kHz IQ stream. "
                                     "Public ones have few user slots and many end a session after about 30 minutes: use one connection per receiver and leave when you are done. "
                                     "The password is kept in OnAir's settings."));
        ImGui::PopTextWrapPos();
    }
    if (a.netAddType == NetRadio::SoapyRemote && !soapySupported()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("This build has no SoapySDR, which SoapyRemote needs."));
    if (a.netAddType == NetRadio::SoapyRemote) {
        ImGui::InputTextWithHint(tr("Remote driver"), "mirisdr  (empty: list what the server offers)", a.netAddRemote, sizeof a.netAddRemote);
        ImGui::TextDisabled("%s", tr("The radio's SoapySDR driver on the server, or a key=value list: driver=sdrplay,serial=1."));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460 * gUi);
        ImGui::TextDisabled("%s", tr("On the radio's computer run SoapySDRServer --bind=\"0.0.0.0:55132\" (port 55132, open it in the firewall). "
                                     "The samples go over UDP by default, which works on a LAN; through the internet, a VPN or NAT add prot=tcp to the field "
                                     "(driver=sdrplay,prot=tcp). Client and server should run the same SoapyRemote version."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button(tr("Connect"))) {
        NetRadio r;
        if (a.netAddType == NetRadio::SoapyRemote || a.netAddType == NetRadio::SoapyArgs) r.remote = a.netAddRemote;
        if (a.netAddType == NetRadio::KiwiSdr) r.remote = a.netAddPass;
        r.type = (NetRadio::Type)a.netAddType;
        r.host = a.netAddHost;
        r.port = a.netAddPort;
        auto pr = std::make_shared<App::NetProbe>();
        pr->radio = r;
        a.netAddProbe = pr;
        a.netAddErr.clear();
        dect2::namedThread("onair-rigprobe", [pr] { std::string e; pr->ok = probeNetRadio(pr->radio, 3000, e); pr->err = e; pr->done = true; }).detach();   // never blocks the UI
    }
    ImGui::EndDisabled();
    if (busy) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("connecting…")); }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Setting up the server…"))) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/NETWORK_RADIOS.md");   // #248
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How to run rtl_tcp, SpyServer, SoapySDRServer, SDR++ server, KiwiSDR or TCI on the radio's computer, and what to do when it does not connect (opens the guide in the browser)"));
    if (!a.netAddErr.empty()) { ImGui::PushTextWrapPos(400 * gUi); ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), tr("Cannot connect: %s"), tr(a.netAddErr.c_str())); ImGui::PopTextWrapPos(); }
    if (!a.netRadios.empty()) netRadioList(a);
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

// the rig model: a searchable list and a typed Hamlib number; returns the model (null when the number is unknown)
static const RigModelInfo* hamlibRigPicker(App& a) {
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
    return cur;
}

// the connection: serial ports found, or host:port of a rigctld; returns whether the path is a network address
static bool hamlibRigConnection(App& a, const RigModelInfo* cur) {
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
    return netPath;
}

// the baud rate, from the model
static void hamlibRigBaud(App& a, const RigModelInfo* cur, bool netPath) {
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
}

// label, sound card id and the Add button
static void hamlibRigAdd(App& a, const RigModelInfo* cur, bool netPath) {
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
}

// the rigs added so far, each with a Remove button
static void hamlibRigList(App& a) {
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
}

// the live link state, while a Hamlib receiver runs
static void hamlibRigLink() {
        const HamlibRigState st = hamlibRigState();
        if (st.active) {
            ImGui::Separator();
            if (st.connected) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s %.6f MHz %s", tr("Rig connected:"), st.hz / 1e6, st.mode.c_str());
            else ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s %s", tr("Rig link lost, reconnecting:"), st.status.c_str());
        }
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

    const RigModelInfo* cur = hamlibRigPicker(a);

    const bool netPath = hamlibRigConnection(a, cur);
    hamlibRigBaud(a, cur, netPath);
    hamlibRigAdd(a, cur, netPath);
    hamlibRigList(a);
    hamlibRigLink();
    ImGui::End();
}


// While a Hamlib receiver runs: what was turned on the rig (its dial, its mode) is taken over into the frequency box and the mode setting.
void hamlibFollowRig(App& a) {
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

// ---- Find KiwiSDR -----------------------------------------------------------------------------------------------------------------------------
// The public list of KiwiSDR receivers, downloaded when the user clicks Download (never at startup, one request), shown with the nearest first when the
// receiver location is known (View > Map), one click adds a receiver to Network radios (no connection is made until it is picked as the radio).
// The address is the public listing's machine-readable file; it can be changed here if the listing moves.
static const char* kKiwiListUrl = "http://rx.linkfanel.net/kiwisdr_com.js";   // ASSUMED: the feed the public map uses; kiwisdr.com itself could not be read when this was written

static void kiwiStartFetch(App& a) {
    if (a.kiwiFetch) return;
    auto f = std::make_shared<App::KiwiFetch>();
    a.kiwiFetch = f;
    const std::string url = a.kiwiListUrl[0] ? a.kiwiListUrl : kKiwiListUrl;
    const std::string tmp = plat::cacheDir() + "/kiwisdr_list.part";
    dect2::namedThread("onair-rigdl", [f, url, tmp] {
        if (!plat::fetchUrl(url, tmp, std::string("OnAir/") + ONAIR_VERSION + " (KiwiSDR list)")) { f->err = "download"; f->done = true; return; }
        std::ifstream in(tmp, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::remove(tmp.c_str());
        f->list = parseKiwiList(ss.str().substr(0, 32u << 20));
        f->ok = !f->list.empty();
        if (!f->ok) f->err = "parse";
        f->done = true;
    }).detach();
}

void kiwiFindWindow(App& a) {
    if (a.kiwiFetch && a.kiwiFetch->done) {
        auto f = a.kiwiFetch;
        a.kiwiFetch.reset();
        if (f->ok) {
            a.kiwiList = f->list;
            double lat = 0, lon = 0;
            receiverLocation(lat, lon);
            kiwiSortByDistance(a.kiwiList, lat, lon);
            a.netAddErr.clear();
        } else a.netAddErr = f->err == "parse" ? N_("The downloaded list has no receivers OnAir can read.") : N_("Could not download the list. Check the internet connection.");
    }
    if (!a.kiwiFindOpen) return;
    if (a.kiwiListUrl[0] == 0) snprintf(a.kiwiListUrl, sizeof a.kiwiListUrl, "%s", kKiwiListUrl);
    ImGui::SetNextWindowSize(ImVec2(760 * gUi, 420 * gUi), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Find KiwiSDR"), &a.kiwiFindOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::BeginDisabled(a.kiwiFetch != nullptr);
    if (ImGui::Button(tr("Download the public list"))) kiwiStartFetch(a);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Downloads the list of public KiwiSDR receivers from the internet when you click (one request, never at startup)."));
    if (a.kiwiFetch) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("downloading the list…")); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##kiwifilter", tr("filter by name, place or antenna"), a.kiwiFilter, sizeof a.kiwiFilter);
    if (!a.netAddErr.empty() && a.kiwiList.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", tr(a.netAddErr.c_str()));
    ImGui::TextDisabled("%s", tr("Public receivers have few user slots, and many end a session after about 30 minutes. Pick one that is not full, and leave it when you are done."));
    std::string flt = a.kiwiFilter;
    for (auto& c : flt) c = (char)tolower((unsigned char)c);
    auto matches = [&](const KiwiServer& k) {
        if (flt.empty()) return true;
        std::string t = k.name + " " + k.location + " " + k.antenna + " " + k.host;
        for (auto& c : t) c = (char)tolower((unsigned char)c);
        return t.find(flt) != std::string::npos;
    };
    if (!a.kiwiList.empty() && ImGui::BeginTable("##kiwis", 7, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_None, 3.f);
        ImGui::TableSetupColumn(tr("Location"), ImGuiTableColumnFlags_None, 2.f);
        ImGui::TableSetupColumn(tr("Users"), ImGuiTableColumnFlags_None, 0.8f);
        ImGui::TableSetupColumn(tr("Antenna"), ImGuiTableColumnFlags_None, 3.f);
        ImGui::TableSetupColumn(tr("SNR"), ImGuiTableColumnFlags_None, 0.7f);
        ImGui::TableSetupColumn(tr("Distance"), ImGuiTableColumnFlags_None, 1.f);
        ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (size_t i = 0; i < a.kiwiList.size() && shown < 400; i++) {
            const KiwiServer& k = a.kiwiList[i];
            if (!matches(k)) continue;
            shown++;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.location.c_str());
            ImGui::TableNextColumn();
            if (k.offline) ImGui::TextDisabled("%s", tr("offline"));
            else if (k.full()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%d/%d %s", k.users, k.usersMax, tr("full"));
            else ImGui::Text("%d/%d", k.users, k.usersMax);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.antenna.c_str());
            ImGui::TableNextColumn(); if (k.snr > 0) ImGui::Text("%.0f", k.snr); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.distanceKm >= 0) ImGui::Text("%.0f km", k.distanceKm); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(k.offline);
            if (ImGui::SmallButton(tr("Add"))) {
                NetRadio r;
                r.type = NetRadio::KiwiSdr;
                r.host = k.host;
                r.port = k.port == 8073 ? 0 : k.port;
                if (std::find(a.netRadios.begin(), a.netRadios.end(), r) == a.netRadios.end()) a.netRadios.push_back(r);
                savePrefs(a);
                if (!a.engine.running()) refreshDevices(a);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Adds this receiver to the Network radios (it does not connect until you pick it as the radio)."));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
