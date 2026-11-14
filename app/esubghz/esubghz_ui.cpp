// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat screens: the chat (lines heard and sent, a line to type), the packets heard, the key and the transmitter.
// Sending: the line ("<name>: <text>", encrypted when a key is set) goes out through a radio that can transmit: the receiving radio when it can,
// otherwise the first free one. A half-duplex radio (a HackRF) cannot do both, so the receiver is stopped for the send and started again after
// it, as the chat says beside the Send button. The rules of the Transmit window hold here too: nothing is sent by itself, the one-time notice
// is asked first, the radio is claimed (device_claims.h) and given back, the map connection is paused, and the HackRF amplifier stays off.
// The key (password or hex) lives only in memory: it is never saved.
#include "app.h"
#include "dect2/common/time_fmt.h"
#include "dect2/engine/device_claims.h"
#include "dect2/esubghz/esubghz_proto.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_tel.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include "dect2/common/diag.h"

namespace {

using namespace dect2;

constexpr double kTxIfHz = 300e3;   // the line is made this far above the radio's centre, so its carrier leak stays off the chat frequency

struct Pending { std::string line; std::vector<uint8_t> octets; bool encrypted = false; };

struct State {
    bool loaded = false;
    int view = 0;                       // 0 chat, 1 packets
    char name[33] = "OnAir";
    int enc = 0;                        // 0 none, 1 password, 2 hex key
    char password[129] = "";
    char hex[100] = "";
    esubghz::Key key;                   // what the receiver and the sender use now
    bool keyBad = false;
    char input[esubghz::kMaxText + 1] = "";
    bool focusInput = false;
    float txGain = 20;
    std::unique_ptr<esubghz::Sender> sender;   // one run id for the program's run, as one Flipper session has
    // the transmitter
    std::deque<Pending> queue;
    TxSession session;
    bool sending = false, claimed = false, pausedRx = false;
    std::string radioName, error;
    bool askNotice = false;
    size_t shownLines = 0;
    std::vector<std::pair<int64_t, std::string>> sentStopped;   // lines sent while the receiver is stopped (its chat shows them once it runs)
};
State S;

bool live(const App& a) { return a.rx.standard == 205; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    snprintf(S.name, sizeof S.name, "%s", p.getS("esgName", "OnAir").c_str());
    S.enc = (int)std::clamp<long>(p.getI("esgEncMode", 0), 0, 2);   // the choice is kept; the password or key itself is asked for again
    S.txGain = (float)p.getD("esgTxGain", 20);
    S.sender = std::make_unique<esubghz::Sender>();
}

void applyKey(App& a) {
    S.keyBad = false;
    esubghz::Key k;
    if (S.enc == 1 && S.password[0]) k = esubghz::keyFromPassword(S.password);
    else if (S.enc == 2 && S.hex[0]) { if (!esubghz::keyFromHex(S.hex, k)) { k = esubghz::Key(); S.keyBad = true; } }
    S.key = k;
    a.engine.esubghz().setKey(k);
}

// the radio that sends, and whether it is the one the receiver uses
const DeviceInfo* txRadio(const App& a, bool& same) {
    same = false;
    const std::vector<int> radios = txRadioChoices(a.devices);
    const DeviceInfo* rxDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? &a.devices[(size_t)a.devIdx] : nullptr;
    if (rxDev && rxDev->isRadio())
        for (int i : radios) if (deviceClaimKey(a.devices[(size_t)i]) == deviceClaimKey(*rxDev)) { same = true; return &a.devices[(size_t)i]; }
    for (int i : radios) {
        const DeviceInfo& d = a.devices[(size_t)i];
        const int h = deviceHolder(d);
        if (h == 0 || h == kTxClaimOwner || d.txFullDuplex) return &d;
    }
    return nullptr;
}

void endSession(App& a) {
    S.session.stop();
    if (S.claimed) { txReleaseRadio(); S.claimed = false; mapTransmitEnded(); }
    S.sending = false;
    (void)a;
}

void resumeRx(App& a) {
    if (!S.pausedRx) return;
    S.pausedRx = false;
    startReceiver(a);
}

void fail(App& a, const std::string& why) {
    S.error = why;
    S.queue.clear();
    if (S.claimed) { txReleaseRadio(); S.claimed = false; mapTransmitAbort(a); }
    S.sending = false;
    resumeRx(a);
}

// every frame: start the next line, see the current one end, give the radio back and start the receiver again
void sendTick(App& a) {
    if (S.sending) {
        if (S.session.running()) return;
        const TxSession::Stats st = S.session.stats();
        endSession(a);
        if (st.failed) { fail(a, st.error.empty() ? std::string("the radio stopped") : st.error); return; }
        const Pending& p = S.queue.front();
        a.engine.esubghz().noteSent(p.line, p.encrypted, p.octets.size());
        if (!S.pausedRx && !a.engine.running()) S.sentStopped.push_back({(int64_t)time(nullptr), p.line});
        a.engine.log("Flipper chat: sent \"" + p.line + "\" on " + S.radioName);
        S.queue.pop_front();
    }
    if (S.queue.empty()) { resumeRx(a); return; }
    if (S.askNotice) return;
    if (!plat::basePrefs().getB("txNoticeAccepted", false)) { S.askNotice = true; return; }
    if (txActive()) { fail(a, "the Transmit window is sending: stop it first"); return; }
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    if (!radio) { fail(a, "no radio here can transmit (a HackRF, PlutoSDR, LimeSDR, USRP or bladeRF can)"); return; }
    const TxModeSpec* mode = txModeById("esubghz");
    if (!mode) { fail(a, "this build has no Flipper chat transmitter"); return; }
    S.error.clear();
    const DeviceInfo dev = *radio;   // a copy: stopping the receiver may refresh the list
    if (same && !dev.txFullDuplex && a.engine.running()) { a.engine.stop(); S.pausedRx = true; }
    std::string err;
    if (!txClaimRadio(dev, err)) { fail(a, err); return; }
    S.claimed = true;
    std::unique_ptr<TxSink> sink = makeTxSink(dev);
    if (!sink) { const std::string why = txUnavailableReason(dev); fail(a, why.empty() ? std::string("this radio has no transmit driver in OnAir") : why); return; }
    TxRequest r;
    r.modeId = "esubghz";
    r.payload.assign(S.queue.front().octets.begin(), S.queue.front().octets.end());
    r.loop = true;   // the generator ends after the message
    r.tune.centerHz = a.freqMhz * 1e6;
    r.tune.offsetHz = -kTxIfHz;
    r.tune.gainDb = dev.txGainMaxDb > dev.txGainMinDb ? std::clamp((double)S.txGain, dev.txGainMinDb, dev.txGainMaxDb) : S.txGain;
    r.tune.ampOn = false;
    r.opt["esubghz.ifHz"] = kTxIfHz;
    mapPauseForTransmit(a);
    if (!S.session.start(std::move(sink), r, TxRateLimits{dev.txMinRateHz, dev.txMaxRateHz}, err)) { fail(a, err.empty() ? std::string("the radio did not start") : err); return; }
    S.sending = true;
    S.radioName = txRadioLabel(dev);
}

void queueLine(App& a, const std::string& text) {
    if (text.empty()) return;
    const std::string line = S.name[0] ? std::string(S.name) + ": " + text : text;
    if (S.enc != 0 && !S.key.on) { S.error = S.enc == 1 ? "type the password first (or set Encryption to Off)" : "the hex key needs 64 hex digits"; return; }
    Pending p;
    p.line = line;
    p.encrypted = S.key.on;
    p.octets = S.sender->encode(S.key.on ? &S.key : nullptr, line);
    if (p.octets.size() > esubghz::kMaxMessage) { DIAG_DEBUG(868605, "A Sub-GHz line of %d octets is too long for a Flipper, so it was not queued.", (int)p.octets.size()); S.error = "the line is too long for a Flipper"; return; }
    S.queue.push_back(std::move(p));
    S.error.clear();
    sendTick(a);
}

void noticePopup() {
    if (S.askNotice && !ImGui::IsPopupOpen("##esgnotice")) ImGui::OpenPopup("##esgnotice");
    ImGui::SetNextWindowSize(ImVec2(480 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##esgnotice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextColored(pal::heading(), "%s", tr("Before you transmit"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Transmitting needs a licence or permission where you are. You are responsible for what you emit and for not interfering with others "
                                  "(including services that people rely on for safety). Connect an antenna, an attenuator or a dummy load to the radio before you start. "
                                  "OnAir does not limit the frequency or the power: the radio's own limits are the only ones."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("I understand"), ImVec2(220 * gUi, 0))) { plat::basePrefs().setB("txNoticeAccepted", true); plat::basePrefs().flush(); S.askNotice = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(-1, 0))) { S.askNotice = false; S.queue.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- settings row

void settings(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Name")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputText("##esgname", S.name, sizeof S.name)) plat::prefs().setS("esgName", S.name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Put in front of every line you send (\"Name: text\"), as a Flipper puts its own name"));
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Encryption")); ImGui::SameLine(0, 5 * gUi);
    static const char* kEnc[] = {"Off", "Password", "Hex key"};
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::Combo("##esgenc", &S.enc, kEnc, 3)) { plat::prefs().setI("esgEncMode", S.enc); applyKey(a); }
    if (S.enc == 1) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::InputTextWithHint("##esgpw", tr("the Flippers' password"), S.password, sizeof S.password, ImGuiInputTextFlags_Password)) applyKey(a);
    } else if (S.enc == 2) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(std::min(420 * gUi, std::max(160 * gUi, ImGui::GetContentRegionAvail().x)));
        if (ImGui::InputTextWithHint("##esghex", "AB CD EF ... (32 octets)", S.hex, sizeof S.hex)) applyKey(a);
    }
    if (S.enc != 0) {
        ImGui::SameLine(0, 6 * gUi);
        if (S.key.on) ImGui::TextColored(pal::okGreen(), "%s", tr("key set"));
        else ImGui::TextColored(pal::warnAmber(), "%s", S.keyBad ? tr("needs 64 hex digits") : tr("no key yet"));
        if (S.key.on && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", tr("The key as a Flipper shows it under Key > Display:"), esubghz::keyHex(S.key).c_str());
    }
    flowEnd();
    // the transmitter
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Send with")); ImGui::SameLine(0, 5 * gUi);
    if (!radio) ImGui::TextColored(pal::warnAmber(), "%s", tr("no radio that can transmit"));
    else {
        ImGui::TextUnformatted(txRadioLabel(*radio).c_str());
        flowNext(12 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("TX gain")); ImGui::SameLine(0, 5 * gUi);
        const float lo = radio->txGainMaxDb > radio->txGainMinDb ? (float)radio->txGainMinDb : 0.f, hi = radio->txGainMaxDb > radio->txGainMinDb ? (float)radio->txGainMaxDb : 47.f;
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::SliderFloat("##esggain", &S.txGain, lo, hi, "%.0f dB")) plat::prefs().setD("esgTxGain", S.txGain);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The radio's own TX gain. A Flipper in the same room needs very little; the HackRF's +14 dB amplifier stays off"));
        flowNext(12 * gUi);
        ImGui::TextDisabled("%s", same && !radio->txFullDuplex ? tr("half duplex: the receiver pauses while a line is sent") : tr("the receiver keeps listening while it sends"));
    }
    flowEnd();
}

// ---------------------------------------------------------------- chat

void chatView(App& a) {
    const EsubghzTelemetry& t = a.rx.esubghz;
    const float inputH = ImGui::GetFrameHeightWithSpacing() * 2 + 4 * gUi;
    ImGui::BeginChild("##esgchat", ImVec2(0, std::max(80 * gUi, ImGui::GetContentRegionAvail().y - inputH)), ImGuiChildFlags_Borders);
    const bool on = live(a);
    if ((!on || t.chat.empty()) && S.sentStopped.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !on && !a.engine.running() ? tr("start the receiver to hear the Flippers (you can still send)")
                                                               : tr("nothing heard yet: on the Flipper open Apps > Sub-GHz > ESubGhzChat, the same frequency, and the same encryption"));
        ImGui::PopTextWrapPos();
    }
    for (size_t i = 0; i < t.chat.size(); i++) {
        const EsgChatLine& c = t.chat[i];
        ImGui::PushID((int)i);
        ImGui::TextDisabled("%s", dect2::timefmt::clock(c.wallTime).c_str());
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextColored(c.dir == 1 ? ImVec4(1.f, 0.66f, 0.36f, 1) : pal::okGreen(), "%s", c.dir == 1 ? tr("sent") : tr("heard"));
        if (c.kind == 1) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("%s", tr("(encrypted)")); }
        ImGui::SameLine(0, 8 * gUi);
        ImGui::PushTextWrapPos(0);
        if (c.kind <= 1) ImGui::TextUnformatted(esubghz::displayText(c.text).c_str());
        else if (c.kind == 2) ImGui::TextColored(pal::warnAmber(), "%s", tr("an encrypted line this key does not open (set the Flippers' password or key above)"));
        else if (c.kind == 3) ImGui::TextDisabled("%s", tr("a repeat of an encrypted line, dropped (as a Flipper drops it)"));
        else { ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%s", c.hex.c_str()); ImGui::PopFont(); }
        ImGui::PopTextWrapPos();
        if (c.dir == 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("%d packet%s, level %.1f dBFS, SNR %.1f dB, frequency offset %+.1f kHz\n%s", c.packets, c.packets == 1 ? "" : "s", c.levelDb, c.snrDb, c.cfoHz / 1e3, c.hex.c_str());
        ImGui::PopID();
    }
    if (a.engine.running()) S.sentStopped.clear();
    for (const auto& l : S.sentStopped) {
        ImGui::TextDisabled("%s", dect2::timefmt::clock(l.first).c_str());
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextColored(ImVec4(1.f, 0.66f, 0.36f, 1), "%s", tr("sent"));
        ImGui::SameLine(0, 8 * gUi);
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(esubghz::displayText(l.second).c_str()); ImGui::PopTextWrapPos();
    }
    if (t.chat.size() + S.sentStopped.size() != S.shownLines) { S.shownLines = t.chat.size() + S.sentStopped.size(); ImGui::SetScrollHereY(1.f); }
    ImGui::EndChild();
    // the line to send
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    const float bw = 80 * gUi;
    ImGui::SetNextItemWidth(std::max(100 * gUi, ImGui::GetContentRegionAvail().x - bw - 8 * gUi));
    if (S.focusInput) { ImGui::SetKeyboardFocusHere(); S.focusInput = false; }
    bool go = ImGui::InputTextWithHint("##esgin", tr("type a line for the Flippers, Enter sends"), S.input, sizeof S.input, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 8 * gUi);
    ImGui::BeginDisabled(!radio || !S.input[0]);
    go = ImGui::Button(tr("Send"), ImVec2(bw, 0)) || go;
    ImGui::EndDisabled();
    if (!radio && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Connect a radio that can transmit (HackRF, PlutoSDR, LimeSDR, USRP, bladeRF)"));
    if (go && radio && S.input[0]) { queueLine(a, S.input); S.input[0] = 0; S.focusInput = true; }
    if (S.sending || !S.queue.empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.66f, 0.36f, 1), "%s %s%s", tr("sending on"), S.radioName.c_str(), S.pausedRx ? tr(" (receiver paused)") : "");
        if (S.queue.size() > 1) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%zu %s", S.queue.size() - 1, tr("more waiting")); }
    } else if (!S.error.empty()) {
        ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "%s", S.error.c_str()); ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("%s %.3f MHz%s", tr("lines go out on"), a.freqMhz, S.key.on ? tr(", encrypted (AES-256-GCM)") : tr(", in the clear"));
    }
}

// ---------------------------------------------------------------- packets

void packetsView(App& a) {
    const EsubghzTelemetry& t = a.rx.esubghz;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (ImGui::Button(tr("Clear"))) a.engine.esubghz().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forget the chat and the packets heard so far"));
    if (!ImGui::BeginTable("##esgpk", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Octets")); ImGui::TableSetupColumn("CRC"); ImGui::TableSetupColumn(tr("Level"));
    ImGui::TableSetupColumn(tr("Offset")); ImGui::TableSetupColumn(tr("Payload"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const EsgPacket& p : t.packets) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::timefmt::clock(p.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", p.len);
        ImGui::TableNextColumn(); if (p.crcOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(pal::badRed(), "bad");
        ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", p.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%+.1f kHz", p.cfoHz / 1e3);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(p.hex.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    ImGui::EndTable();
}

void tab(App& a) {
    loadState();
    settings(a);
    ImGui::Spacing();
    subNav("esgv", S.view, {"Chat", "Packets"});
    if (S.view == 0) chatView(a); else packetsView(a);
    noticePopup();
}

void list(App& a) {
    // who was heard: the names in front of the lines
    const EsubghzTelemetry& t = a.rx.esubghz;
    std::vector<std::pair<std::string, int64_t>> who;
    for (const EsgChatLine& c : t.chat) {
        if (c.dir != 0 || c.kind > 1) continue;
        std::string n = c.text;
        auto cut = [&](std::string_view tail) { if (n.size() <= tail.size() || !n.ends_with(tail)) return false; n.resize(n.size() - tail.size()); return true; };
        const size_t q = n.find(": ");
        if (q != std::string::npos && q <= 32) n.resize(q);
        else if (!cut(" joined chat.") && !cut(" left chat.")) continue;
        auto it = std::find_if(who.begin(), who.end(), [&](const auto& w) { return w.first == n; });
        if (it == who.end()) who.push_back({n, c.wallTime}); else it->second = c.wallTime;
    }
    if (who.empty()) { ImGui::TextDisabled("%s", live(a) ? tr("no Flipper heard yet") : a.engine.running() ? tr("starting") : tr("stopped")); return; }
    for (const auto& w : who) {
        ImGui::TextColored(pal::okGreen(), "%s", esubghz::displayText(w.first).c_str());
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%s", dect2::timefmt::clock(w.second).c_str());
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const EsubghzTelemetry& t = a.rx.esubghz;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", esubghzSummary(t).c_str());
    kv(tr("signal"), "SNR %.1f dB, offset %+.1f kHz, level %.1f dBFS", t.snrDb, t.cfoHz / 1e3, t.levelDb);
    kv(tr("packets"), "%llu good, %llu failed the CRC, %llu bursts", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.bursts);
    kv(tr("messages"), "%llu heard, %llu decrypted, %llu not readable with this key, %llu repeats dropped, %llu sent", (unsigned long long)t.messages,
       (unsigned long long)t.decrypted, (unsigned long long)t.unreadable, (unsigned long long)t.replays, (unsigned long long)t.sent);
    kv(tr("channel"), "%+.0f kHz from the radio's centre", t.signalOffsetHz / 1e3);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The Flipper's Sub-GHz chat: CC1101 GFSK, 9.99 kbaud, 19 kHz deviation, sync word 0x464C, packets of up to 60 octets with a CRC-16. "
                                 "Packets less than 0.5 s apart make one message, as the Flipper app joins them. Encrypted chats use AES-256-GCM with a key "
                                 "from a password (SHA-256) or typed in hex, and drop repeated messages."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Packets"), !on ? 0 : a.rx.esubghz.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    lamp("TX", S.sending ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(S.sending ? tr("sending") : !on ? (run ? tr("starting") : S.pausedRx ? tr("paused for sending") : tr("stopped")) : esubghzSummary(a.rx.esubghz).c_str());
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Flipper Zero Sub-GHz chat";
    if (S.sending) l2 = "sending";
    else if (live(a)) l2 = esubghzSummary(a.rx.esubghz);
}

void tuner(App& a, bool&) {
    loadState();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Chat frequency")); ImGui::SameLine(0, 5 * gUi);
    static const double kMhz[] = {433.92, 315.0, 390.0, 868.35, 915.0};
    for (double m : kMhz) {
        char b[24];
        snprintf(b, sizeof b, "%.2f##esgf", m);
        const bool cur = std::fabs(a.freqMhz - m) < 0.001;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(b)) tuneFreq(a, m);
        if (cur) ImGui::PopStyleColor();
        ImGui::SameLine(0, 4 * gUi);
    }
    ImGui::NewLine();
    ImGui::TextDisabled("%s", tr("433.92 MHz is the app's default; the Flippers must all use the same frequency"));
}

void tick(App& a) {
    loadState();
    static bool keyed = false;
    if (!keyed) { keyed = true; applyKey(a); }
    sendTick(a);
}

} // namespace

extern const ModeUi kEsubghzUi;
const ModeUi kEsubghzUi = {
    .sideTitle = "FLIPPERS",
    .tabName = "Flipper chat",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
