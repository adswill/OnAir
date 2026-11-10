// Tools > EchoLink: log in with the user's callsign and password, list and search the stations, connect to a station, link, repeater or
// conference, talk with push-to-talk or VOX and listen on the speakers. An optional RF bridge (off by default, with a licensing note) links
// a radio on a sound card to the connection. The protocol and audio are in core/src/echolink; this file is the window and the sound cards.
// The password lives in the settings under "echolinkPassword" (a credential key: kept out of plain backups) and is never shown or logged.
#include "../app.h"
#include "../ui/typography.h"
#include "dect2/audio/audioin.h"
#include "dect2/audio/audioout.h"
#include "dect2/echolink/el_audio.h"
#include "dect2/echolink/el_client.h"
#include "dect2/source/hamlib_link.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace dect2::echolink;

struct ElUi {
    bool loaded = false;
    char call[16] = {}, name[32] = {}, location[32] = {}, servers[256] = {}, search[64] = {}, chatIn[256] = {}, manualIp[64] = {};
    std::string password;            // in memory and in the settings only
    char passEdit[64] = {};          // the field while the user types; cleared after it is taken
    int port = kDefaultAudioPort;
    bool accept = true;
    int kindFilter = 0;              // 0 all, 1 stations, 2 links, 3 repeaters, 4 conferences
    bool vox = false, pttLatched = false, pttHeld = false;
    float voxDb = -30, volume = 1.f;
    int voxHoldMs = 800;
    std::string micDev, spkDev;
    // RF bridge
    bool bridgeOn = false, bridgeAck = false, askBridge = false;
    std::string rigIn, rigOut;
    float squelchDb = -40;
    int pttKind = 0;                 // 0: the radio's own VOX keys it from the audio, 1: Hamlib CAT / RTS / DTR
    int hamModel = 1, hamPttType = 0, tailMs = 500, maxTxSec = 180;
    char hamPath[96] = {};
    // runtime
    Client client;
    std::vector<Station> shown;
    std::string selected, error;
    std::vector<std::string> chat, log;
    size_t listSeen = 0;
    int listAge = -2;
    std::unique_ptr<dect2::AudioIn> mic, rigAudioIn;
    std::unique_ptr<dect2::AudioOut> spk, rigAudioOut;
    Down6 micDown, rigDown;
    Up6 up;
    Vox voxDet, squelch;
    RfBridge bridge;
    dect2::HamlibLink ham;
    bool radioKeyed = false;
    std::string bridgeNote;
    double lastTick = 0;
};
ElUi* gEl = nullptr;
ElUi& S() { if (!gEl) gEl = new ElUi(); return *gEl; }   // kept for the life of the program (its threads stop in close())

void copyTo(char* d, size_t n, const std::string& s) { std::snprintf(d, n, "%s", s.c_str()); }

std::vector<std::string> splitServers(const char* s) {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = s;; p++) {
        if (*p == ',' || *p == ' ' || *p == ';' || *p == 0) { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return v;
}

void load() {
    ElUi& e = S();
    if (e.loaded) return;
    e.loaded = true;
    plat::Prefs& d = plat::basePrefs();
    copyTo(e.call, sizeof e.call, d.getS("echolinkCall", ""));
    copyTo(e.name, sizeof e.name, d.getS("echolinkName", ""));
    copyTo(e.location, sizeof e.location, d.getS("echolinkLocation", ""));
    copyTo(e.servers, sizeof e.servers, d.getS("echolinkServers", "servers.echolink.org"));
    e.password = d.getS("echolinkPassword", "");
    e.port = (int)d.getI("echolinkPort", kDefaultAudioPort);
    e.accept = d.getB("echolinkAccept", true);
    e.vox = d.getB("echolinkVox", false);
    e.voxDb = (float)d.getD("echolinkVoxDb", -30);
    e.voxHoldMs = (int)d.getI("echolinkVoxHold", 800);
    e.volume = (float)d.getD("echolinkVolume", 1.0);
    e.micDev = d.getS("echolinkMic", "");
    e.spkDev = d.getS("echolinkSpeaker", "");
    e.bridgeAck = d.getB("echolinkBridgeAck", false);   // the bridge itself is off at every start
    e.rigIn = d.getS("echolinkBridgeIn", "");
    e.rigOut = d.getS("echolinkBridgeOut", "");
    e.squelchDb = (float)d.getD("echolinkBridgeSquelch", -40);
    e.pttKind = (int)d.getI("echolinkBridgePtt", 0);
    e.hamModel = (int)d.getI("echolinkBridgeModel", 1);
    e.hamPttType = (int)d.getI("echolinkBridgePttType", 0);
    copyTo(e.hamPath, sizeof e.hamPath, d.getS("echolinkBridgePath", ""));
    e.tailMs = (int)d.getI("echolinkBridgeTail", 500);
    e.maxTxSec = (int)d.getI("echolinkBridgeMaxTx", 180);
}

void save() {
    ElUi& e = S();
    plat::Prefs& d = plat::basePrefs();
    d.setS("echolinkCall", e.call); d.setS("echolinkName", e.name); d.setS("echolinkLocation", e.location); d.setS("echolinkServers", e.servers);
    d.setS("echolinkPassword", e.password);
    d.setI("echolinkPort", e.port); d.setB("echolinkAccept", e.accept);
    d.setB("echolinkVox", e.vox); d.setD("echolinkVoxDb", e.voxDb); d.setI("echolinkVoxHold", e.voxHoldMs); d.setD("echolinkVolume", e.volume);
    d.setS("echolinkMic", e.micDev); d.setS("echolinkSpeaker", e.spkDev);
    d.setB("echolinkBridgeAck", e.bridgeAck); d.setS("echolinkBridgeIn", e.rigIn); d.setS("echolinkBridgeOut", e.rigOut);
    d.setD("echolinkBridgeSquelch", e.squelchDb); d.setI("echolinkBridgePtt", e.pttKind); d.setI("echolinkBridgeModel", e.hamModel);
    d.setI("echolinkBridgePttType", e.hamPttType); d.setS("echolinkBridgePath", e.hamPath); d.setI("echolinkBridgeTail", e.tailMs);
    d.setI("echolinkBridgeMaxTx", e.maxTxSec);
}

ClientConfig config() {
    ElUi& e = S();
    ClientConfig c;
    c.call = e.call; c.password = e.password; c.name = e.name; c.location = e.location;
    c.servers = splitServers(e.servers);
    c.audioPort = e.port;
    c.acceptIncoming = e.accept;
    c.info = std::string("OnAir ") + (e.name[0] ? std::string("- ") + e.name : std::string()) + (e.location[0] ? std::string("\n") + e.location : std::string());
    return c;
}

bool ensureOpen() {
    ElUi& e = S();
    e.client.configure(config());
    if (e.client.isOpen()) return true;
    std::string err;
    if (!e.client.open(err)) { e.error = err; return false; }
    e.error.clear();
    return true;
}

// ---- sound -----------------------------------------------------------------------------------------------------------------------------------
void startAudio() {
    ElUi& e = S();
    std::string err;
    if (!e.spk) {
        e.spk = std::make_unique<dect2::AudioOut>();
        e.spk->setDevice(e.spkDev);
        if (!e.spk->start(48000)) e.error = tr("The speakers could not be opened.");
        e.up.reset();
    }
    if (!e.mic && !e.bridge.active()) {   // the microphone opens only while connected
        e.mic = std::make_unique<dect2::AudioIn>();
        if (!e.mic->start(dect2::AudioIn::Mic, 48000, e.micDev, 0, err)) { e.error = err; e.mic.reset(); }
        e.micDown.reset();
    }
}

void stopAudio() {
    ElUi& e = S();
    if (e.mic) { e.mic->stop(); e.mic.reset(); }
    if (e.spk) { e.spk->stop(); e.spk.reset(); }
    e.pttHeld = e.pttLatched = false;
    e.client.setTransmit(false);
}

void keyRadio(bool on) {
    ElUi& e = S();
    if (on == e.radioKeyed) return;
    e.radioKeyed = on;
    if (e.pttKind == 1) {
        std::string err;
        if (on && !e.ham.isOpen()) {
            dect2::HamlibConn c;
            c.model = e.hamModel; c.path = e.hamPath; c.pttType = e.hamPttType;
            if (!e.ham.open(c, err)) { e.bridgeNote = err; return; }
        }
        if (e.ham.isOpen() && !e.ham.setPtt(on, err)) e.bridgeNote = err;
    }
}

void startBridgeAudio() {
    ElUi& e = S();
    std::string err;
    if (!e.rigAudioIn) {
        e.rigAudioIn = std::make_unique<dect2::AudioIn>();
        if (!e.rigAudioIn->start(dect2::AudioIn::Mic, 48000, e.rigIn, 0, err)) { e.bridgeNote = err; e.rigAudioIn.reset(); }
        e.rigDown.reset();
    }
    if (!e.rigAudioOut) {
        e.rigAudioOut = std::make_unique<dect2::AudioOut>();
        e.rigAudioOut->setDevice(e.rigOut);
        if (!e.rigAudioOut->start(48000)) e.bridgeNote = tr("The radio's sound output could not be opened.");
    }
}

void stopBridgeAudio() {
    ElUi& e = S();
    keyRadio(false);
    if (e.ham.isOpen()) e.ham.close();
    if (e.rigAudioIn) { e.rigAudioIn->stop(); e.rigAudioIn.reset(); }
    if (e.rigAudioOut) { e.rigAudioOut->stop(); e.rigAudioOut.reset(); }
}

// 48 kHz stereo from a capture -> 8 kHz mono
void readCapture(dect2::AudioIn& in, Down6& down, std::vector<int16_t>& out) {
    float st[2 * 2048];
    std::vector<float> mono;
    for (int guard = 0; guard < 8; guard++) {
        const int n = in.readAvailable(st, 2048);
        if (n <= 0) break;
        mono.resize((size_t)n);
        for (int i = 0; i < n; i++) mono[(size_t)i] = 0.5f * (st[2 * i] + st[2 * i + 1]);
        down.process(mono.data(), n, out);
    }
}

void tick() {
    ElUi& e = S();
    const double now = ImGui::GetTime();
    const int dtMs = e.lastTick > 0 ? (int)std::min(500.0, (now - e.lastTick) * 1000) : 0;
    e.lastTick = now;
    if (!e.client.isOpen()) return;
    const ClientStatus st = e.client.status();
    const bool connected = st.qso == QsoState::Connected;
    BridgeConfig bc;
    bc.enabled = e.bridgeOn; bc.licenceAcknowledged = e.bridgeAck; bc.tailMs = e.tailMs; bc.maxTxSec = e.maxTxSec;
    e.bridge.configure(bc);
    if (!connected) {
        if (e.mic || e.spk) stopAudio();
        if (e.rigAudioIn || e.rigAudioOut) stopBridgeAudio();
        e.bridge.reset();
    } else {
        startAudio();
        if (e.bridge.active()) startBridgeAudio();
        else if (e.rigAudioIn || e.rigAudioOut) stopBridgeAudio();
    }
    // transmit: the microphone (PTT or VOX), or the radio's receive when the bridge forwards it
    std::vector<int16_t> mic8, rig8;
    if (e.mic) readCapture(*e.mic, e.micDown, mic8);
    if (e.rigAudioIn) readCapture(*e.rigAudioIn, e.rigDown, rig8);
    e.voxDet.configure({e.voxDb, e.voxHoldMs});
    const bool voxOpen = e.vox && !mic8.empty() && e.voxDet.process(mic8.data(), (int)mic8.size());
    bool tx = connected && (e.pttHeld || e.pttLatched || (e.vox && (voxOpen || e.voxDet.open())));
    const std::vector<int16_t>* txAudio = &mic8;
    if (e.bridge.active() && connected) {
        e.squelch.configure({e.squelchDb, 300});
        const bool sq = !rig8.empty() ? e.squelch.process(rig8.data(), (int)rig8.size()) : e.squelch.open();
        const BridgeOut bo = e.bridge.step(dtMs, sq, st.rx);
        keyRadio(bo.keyRadio);
        if (bo.timedOut) e.bridgeNote = tr("Transmit time-out: the radio waits until the EchoLink station stops.");
        if (bo.forwardRfToEchoLink) { tx = true; txAudio = &rig8; }
    }
    e.client.setTransmit(tx);
    if (tx && !txAudio->empty()) e.client.pushTx(txAudio->data(), (int)txAudio->size());
    // receive: the speakers (and the radio while the bridge keys it)
    int16_t rx8[1600];
    const int got = e.client.pullRx(rx8, 1600);
    if (got > 0) {
        std::vector<float> m48;
        e.up.process(rx8, got, m48);
        std::vector<float> st48(m48.size() * 2);
        for (size_t i = 0; i < m48.size(); i++) st48[2 * i] = st48[2 * i + 1] = m48[i] * e.volume;
        if (e.spk) e.spk->write(st48.data(), (int)m48.size());
        if (e.radioKeyed && e.rigAudioOut) {
            for (size_t i = 0; i < m48.size(); i++) st48[2 * i] = st48[2 * i + 1] = m48[i];
            e.rigAudioOut->write(st48.data(), (int)m48.size());
        }
    }
    for (std::string& s : e.client.takeChat()) { e.chat.push_back(std::move(s)); if (e.chat.size() > 300) e.chat.erase(e.chat.begin()); }
    for (std::string& s : e.client.takeLog()) { e.log.push_back(std::move(s)); if (e.log.size() > 300) e.log.erase(e.log.begin()); }
}

// ---- window parts ------------------------------------------------------------------------------------------------------------------------
void accountSection() {
    ElUi& e = S();
    ImGui::PushID("acct");
    bool ch = false;
    ImGui::SetNextItemWidth(140 * gUi); ch |= ImGui::InputText(tr("Callsign"), e.call, sizeof e.call, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    const char* hint = e.password.empty() ? tr("password") : tr("saved (type to change)");
    if (ImGui::InputTextWithHint(tr("Password"), hint, e.passEdit, sizeof e.passEdit, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && e.passEdit[0])) {
        e.password = e.passEdit;
        std::memset(e.passEdit, 0, sizeof e.passEdit);
        ch = true;
    }
    ImGui::SetNextItemWidth(140 * gUi); ch |= ImGui::InputText(tr("Name"), e.name, sizeof e.name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi); ch |= ImGui::InputText(tr("Location"), e.location, (size_t)kMaxLocation + 1);
    if (ch) save();
    ImGui::TextDisabled("%s", tr("A callsign validated by EchoLink (echolink.org) is needed. The password is kept in the settings and never shown or logged."));
    ImGui::PopID();
}

void stationList() {
    ElUi& e = S();
    ImGui::PushID("list");
    const std::vector<Station> all = e.client.stations();
    static const char* kKinds[] = {N_("All"), N_("Stations"), N_("Links"), N_("Repeaters"), N_("Conferences")};
    ImGui::SetNextItemWidth(200 * gUi);
    bool refilter = ImGui::InputTextWithHint("##search", tr("Search callsign, place or node"), e.search, sizeof e.search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::BeginCombo("##kind", tr(kKinds[e.kindFilter]))) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(kKinds[i]), e.kindFilter == i)) { e.kindFilter = i; refilter = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh list"))) { if (ensureOpen()) e.client.refreshList(); }
    if (refilter || all.size() != e.listSeen || e.shown.empty() != all.empty()) {
        static const StationKind kk[] = {StationKind::User, StationKind::User, StationKind::Link, StationKind::Repeater, StationKind::Conference};
        e.shown = searchStations(all, e.search, e.kindFilter ? 1u << (unsigned)kk[e.kindFilter] : 0);
        e.listSeen = all.size();
    }
    ImGui::TextDisabled(tr("%zu of %zu"), e.shown.size(), all.size());
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##stations", 4, tf, ImVec2(0, 260 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Callsign"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn(tr("Description"));
        ImGui::TableSetupColumn(tr("Node"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)e.shown.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const Station& s = e.shown[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(s.call.c_str(), e.selected == s.call, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    e.selected = s.call;
                    if (ImGui::IsMouseDoubleClicked(0) && ensureOpen()) e.client.connectTo(s.ip);
                }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.description.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", s.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.busy ? tr("busy") : s.online ? tr("on") : "?");
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void qsoSection(const ClientStatus& st) {
    ElUi& e = S();
    ImGui::PushID("qso");
    if (st.qso == QsoState::Idle) {
        ImGui::BeginDisabled(e.selected.empty());
        if (ImGui::Button(e.selected.empty() ? tr("Connect") : (std::string(tr("Connect to")) + " " + e.selected).c_str())) {
            if (ensureOpen() && !e.client.connectCall(e.selected)) e.error = tr("That station is not in the list any more: refresh it.");
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button(tr("Disconnect"))) e.client.disconnect();
        ImGui::SameLine();
        ImGui::Text("%s %s %s", tr(qsoStateName(st.qso)), st.peerCall.c_str(), st.peerName.c_str());
    }
    if (st.qso == QsoState::Connected) {
        ImGui::TextColored(st.rx ? ImVec4(0.3f, 0.9f, 0.3f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", st.rx ? tr("RECEIVING") : tr("quiet"));
        ImGui::SameLine();
        ImGui::TextColored(st.tx ? ImVec4(1, 0.35f, 0.3f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", st.tx ? tr("TRANSMITTING") : tr("not transmitting"));
        ImGui::Button(tr("Hold to talk"), ImVec2(160 * gUi, 40 * gUi));
        e.pttHeld = ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::Checkbox(tr("Talk (latched)"), &e.pttLatched);
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("VOX"), &e.vox)) save();
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderFloat(tr("Volume"), &e.volume, 0.f, 2.f, "%.2f")) save();
        if (!st.peerInfo.empty()) {
            typo::separatorText(tr("Station info"));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(st.peerInfo.c_str());
            ImGui::PopTextWrapPos();
        }
        typo::separatorText(tr("Chat"));
        ImGui::BeginChild("##chat", ImVec2(0, 90 * gUi), ImGuiChildFlags_Borders);
        for (const std::string& c : e.chat) ImGui::TextWrapped("%s", c.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##chatin", tr("Type a message and press Enter"), e.chatIn, sizeof e.chatIn, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (e.client.sendChat(e.chatIn)) e.chat.push_back(std::string(e.call) + ">" + e.chatIn);
            e.chatIn[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::Text(tr("Packets in %llu, out %llu, lost %llu"), (unsigned long long)st.packetsIn, (unsigned long long)st.packetsOut, (unsigned long long)st.lost);
    }
    ImGui::PopID();
}

void advancedSection() {
    ElUi& e = S();
    ImGui::PushID("adv");
    bool ch = false;
    ImGui::SetNextItemWidth(300 * gUi); ch |= ImGui::InputText(tr("Directory servers"), e.servers, sizeof e.servers);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputInt(tr("UDP port (audio; control is the next one)"), &e.port)) { e.port = std::clamp(e.port, 1024, 65534); ch = true; }
    ch |= ImGui::Checkbox(tr("Accept calls from other stations"), &e.accept);
    ImGui::SetNextItemWidth(160 * gUi); ch |= ImGui::SliderFloat(tr("VOX threshold (dBFS)"), &e.voxDb, -60.f, -5.f, "%.0f");
    ImGui::SetNextItemWidth(160 * gUi); ch |= ImGui::SliderInt(tr("VOX hold (ms)"), &e.voxHoldMs, 100, 3000);
    static std::vector<dect2::AudioInputDevice> ins;
    static std::vector<std::string> outs;
    if (ImGui::Button(tr("Find sound devices")) || (ins.empty() && outs.empty())) { ins = dect2::listAudioInputs(); outs = dect2::AudioOut::devices(); }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Microphone"), e.micDev.empty() ? tr("System default") : e.micDev.c_str())) {
        for (const auto& d : ins) if (ImGui::Selectable(d.id.empty() ? tr("System default") : d.name.c_str(), e.micDev == d.id)) { e.micDev = d.id; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Speakers"), e.spkDev.empty() ? tr("System default") : e.spkDev.c_str())) {
        for (size_t i = 0; i < outs.size(); i++) {
            const std::string id = i == 0 ? std::string() : outs[i];
            if (ImGui::Selectable(i == 0 ? tr("System default") : outs[i].c_str(), e.spkDev == id)) { e.spkDev = id; ch = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(180 * gUi);
    ImGui::InputTextWithHint("##ip", tr("IP address"), e.manualIp, sizeof e.manualIp);
    ImGui::SameLine();
    if (ImGui::Button(tr("Connect to this address")) && ensureOpen() && !e.client.connectTo(e.manualIp)) e.error = tr("Not an IPv4 address, or already connected.");
    if (ch) { save(); e.client.configure(config()); }
    if (!e.log.empty()) {
        typo::separatorText(tr("Log"));
        ImGui::BeginChild("##ellog", ImVec2(0, 90 * gUi), ImGuiChildFlags_Borders);
        for (const std::string& l : e.log) ImGui::TextUnformatted(l.c_str());
        ImGui::EndChild();
    }
    ImGui::PopID();
}

void bridgeSection() {
    ElUi& e = S();
    ImGui::PushID("bridge");
    bool want = e.bridgeOn;
    if (ImGui::Checkbox(tr("Link to RF channel"), &want)) {
        if (want && !e.bridgeAck) e.askBridge = true;
        else e.bridgeOn = want;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("(off by default)"));
    if (e.askBridge) { ImGui::OpenPopup(trw("Linking EchoLink to a radio")); e.askBridge = false; }
    if (ImGui::BeginPopupModal(trw("Linking EchoLink to a radio"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextUnformatted(tr("A link sends what the radio receives to the EchoLink connection and transmits what EchoLink sends. Running a link or "
                                  "repeater node needs an amateur licence that allows it and, in many countries, a separate permit for unattended or "
                                  "remote operation. You are responsible for identifying the station, for staying in your licence's bands and for "
                                  "what is relayed. EchoLink also asks link nodes to be registered with -L or -R."));
        ImGui::PopTextWrapPos();
        bool ack = false;
        if (ImGui::Button(tr("I understand, turn the link on"))) ack = true;
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        if (ack) { e.bridgeAck = true; e.bridgeOn = true; save(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    bool ch = false;
    static std::vector<dect2::AudioInputDevice> ins;
    static std::vector<std::string> outs;
    if (ins.empty() && outs.empty()) { ins = dect2::listAudioInputs(); outs = dect2::AudioOut::devices(); }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Radio audio in (its speaker)"), e.rigIn.empty() ? tr("System default") : e.rigIn.c_str())) {
        for (const auto& d : ins) if (ImGui::Selectable(d.id.empty() ? tr("System default") : d.name.c_str(), e.rigIn == d.id)) { e.rigIn = d.id; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Radio audio out (its microphone)"), e.rigOut.empty() ? tr("System default") : e.rigOut.c_str())) {
        for (size_t i = 0; i < outs.size(); i++) {
            const std::string id = i == 0 ? std::string() : outs[i];
            if (ImGui::Selectable(i == 0 ? tr("System default") : outs[i].c_str(), e.rigOut == id)) { e.rigOut = id; ch = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(160 * gUi); ch |= ImGui::SliderFloat(tr("Squelch (dBFS)"), &e.squelchDb, -70.f, -10.f, "%.0f");
    static const char* kPtt[] = {N_("The radio's own VOX"), N_("Hamlib (CAT, RTS or DTR)")};
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo(tr("Keying"), tr(kPtt[e.pttKind]))) {
        for (int i = 0; i < 2; i++) if (ImGui::Selectable(tr(kPtt[i]), e.pttKind == i)) { e.pttKind = i; ch = true; }
        ImGui::EndCombo();
    }
    if (e.pttKind == 1) {
        ImGui::SetNextItemWidth(100 * gUi); ch |= ImGui::InputInt(tr("Hamlib model"), &e.hamModel);
        ImGui::SetNextItemWidth(200 * gUi); ch |= ImGui::InputText(tr("Port or host:port"), e.hamPath, sizeof e.hamPath);
        static const char* kType[] = {N_("CAT command"), N_("RTS line"), N_("DTR line")};
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::BeginCombo(tr("PTT by"), tr(kType[std::clamp(e.hamPttType, 0, 2)]))) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(kType[i]), e.hamPttType == i)) { e.hamPttType = i; ch = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderInt(tr("Tail (ms)"), &e.tailMs, 0, 3000);
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderInt(tr("Transmit time-out (s, 0 = none)"), &e.maxTxSec, 0, 900);
    if (ch) save();
    if (!e.bridgeNote.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", e.bridgeNote.c_str());
    ImGui::PopID();
}

} // namespace

void echolinkWindow(App& a) {
    load();
    tick();   // the sound and the connection run while the window is closed too
    if (!a.echolinkOpen) return;
    firstWindowSize(640 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("EchoLink"), &a.echolinkOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ElUi& e = S();
    const ClientStatus st = e.client.status();
    // one click: open the ports, log in and fetch the list
    if (!st.loggedIn) {
        ImGui::BeginDisabled(st.busy);
        if (ImGui::Button(tr("Log in and list stations"))) {
            if (ensureOpen()) { e.client.login(); e.client.refreshList(); }
        }
        ImGui::EndDisabled();
    } else if (ImGui::Button(tr("Log out"))) {
        e.client.close();
        stopAudio();
        stopBridgeAudio();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(st.busy ? tr("working...") : st.directory.c_str());
    if (!e.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", e.error.c_str());
    if (!e.call[0] || e.password.empty() || ImGui::CollapsingHeader(trw("Account"))) accountSection();
    typo::separatorText(tr("Stations"));
    stationList();
    typo::separatorText(tr("Connection"));
    qsoSection(st);
    if (ImGui::CollapsingHeader(trw("RF link"))) bridgeSection();
    if (ImGui::CollapsingHeader(trw("Advanced"))) advancedSection();
    ImGui::TextDisabled("%s", tr("UDP ports 5198 and 5199 must reach this computer (forward them on the router) for stations to hear you."));
    ImGui::End();
}

void echolinkShutdown() {
    if (!gEl) return;
    stopAudio();
    stopBridgeAudio();
    gEl->client.close();
}
