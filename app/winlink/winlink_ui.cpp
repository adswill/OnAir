// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Winlink email: Inbox, Outbox, Sent, Compose, Gateways, Modems and Settings, and the Connect button that runs one B2F session with the chosen
// gateway over packet (1200 / 9600 baud AX.25), ARDOP, an external modem program (ARDOP / VARA / Mercury over TCP), or (only when turned on in Settings) Telnet to the CMS. The gateway list is
// entered by hand: OnAir never looks gateways up on the internet. The Winlink password stays in memory for this run only.
// The frames go out as one-shot bursts of the "winlinkpkt" / "winlinkardop" transmit modes and come back through the receivers'
// frame taps; the receiver is paused while its radio transmits (half duplex), as on the MeshCore screen.
#include "winlink_ui.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/engine/device_claims.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/tx/tx.h"
#include "dect2/winlink/wl_extmodem.h"
#include "dect2/winlink/wl_modem.h"
#include "dect2/winlink/wl_station.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace dect2;
using namespace dect2::winlink;

double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Taps {
    std::mutex mu;
    std::vector<ax25::Frame> ax;
    std::vector<ArdopRxFrameIn> ar;
};

struct ModemTest {
    std::atomic<bool> done{false};
    ModemTestResult r;
};

struct W {
    bool loaded = false;
    // external modems (ARDOP / VARA / Mercury programs reached over TCP)
    std::vector<ModemProfile> modems;
    int modemSel = -1;
    int mType = 0, mCmd = 8515, mData = 8516, mTimeout = 0;
    char mName[48] = "", mHost[96] = "127.0.0.1", mBw[16] = "", mPtt[96] = "", mAudio[96] = "";
    std::shared_ptr<ModemTest> mtest;
    std::string mtestMsg;
    int gModem = -1;
    Mailbox mb;
    std::unique_ptr<WinlinkStation> st;
    std::vector<Gateway> gws;
    int gwSel = -1;
    char call[16] = "", grid[12] = "", password[64] = "";
    bool telnetOn = false;
    bool gzipOn = false, resumeOn = true, gPeer = false;
    float txGain = 10;
    int view = 0;                        // 0 inbox 1 outbox 2 sent 3 compose 4 gateways 5 modems 6 settings
    std::string selMid;
    std::vector<MailEntry> list;
    int listFolder = -1;
    double listAt = -1;
    B2Message preview;
    // compose
    char to[256] = "", cc[256] = "", subject[128] = "", body[16384] = "", attachPath[512] = "";
    std::vector<B2File> attach;
    std::string composeNote;
    // new gateway
    char gCall[24] = "", gVia[64] = "", gHost[96] = "", gNote[96] = "";
    int gTransport = 0, gPort = 8772;
    double gMhz = 0;
    // session
    std::deque<std::string> log;
    std::shared_ptr<Taps> taps;
    bool tapsOn = false;
    Transport tr = Transport::Packet1200;
    // transmit
    TxSession session;
    bool sending = false, claimed = false, pausedRx = false, askNotice = false;
    std::vector<ax25::Frame> outAx;
    std::vector<ArdopLinkFrame> outAr;
    std::string error;
};
W& w() { static W x; return x; }

void addLog(const std::string& s) {
    W& d = w();
    char t[16];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    snprintf(t, sizeof t, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    d.log.push_back(t + s);
    while (d.log.size() > 400) d.log.pop_front();
}

void saveGws() { plat::prefs().setS("winlinkGateways", saveGateways(w().gws)); plat::prefs().flush(); }
void saveModems() { plat::prefs().setS("winlinkModems", saveModemProfiles(w().modems)); plat::prefs().flush(); }
const ModemProfile* findModem(const std::string& name) {
    for (const ModemProfile& m : w().modems) if (m.name == name) return &m;
    return nullptr;
}

void load() {
    W& d = w();
    if (d.loaded) return;
    d.loaded = true;
    d.mb.setDir(plat::dataDir() + "/winlink");
    d.st = std::make_unique<WinlinkStation>(d.mb);
    d.gws = loadGateways(plat::prefs().getS("winlinkGateways", ""));
    d.modems = loadModemProfiles(plat::prefs().getS("winlinkModems", ""));
    d.gwSel = (int)plat::prefs().getI("winlinkGateway", d.gws.empty() ? -1 : 0);
    if (d.gwSel >= (int)d.gws.size()) d.gwSel = d.gws.empty() ? -1 : 0;
    snprintf(d.call, sizeof d.call, "%s", plat::prefs().getS("winlinkCall", "").c_str());
    snprintf(d.grid, sizeof d.grid, "%s", plat::prefs().getS("winlinkGrid", "").c_str());
    d.telnetOn = plat::prefs().getB("winlinkTelnet", false);
    d.gzipOn = plat::prefs().getB("winlinkGzip", false);
    d.resumeOn = plat::prefs().getB("winlinkResume", true);
    d.txGain = (float)plat::prefs().getD("winlinkTxGain", 10);
}

bool fits(int ctx, Transport t) {
    if (t == Transport::Telnet || t == Transport::Modem) return true;
    return ctx == 1 ? t == Transport::Ardop : t != Transport::Ardop;
}

// ---------------------------------------------------------------- transmit (as the MeshCore screen)

const DeviceInfo* txRadio(const App& a, bool& same) {
    same = false;
    const std::vector<int> radios = txRadioChoices(a.devices);
    const DeviceInfo* rxDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? &a.devices[(size_t)a.devIdx] : nullptr;
    if (rxDev && rxDev->isRadio())
        for (int i : radios) if (deviceClaimKey(a.devices[(size_t)i]) == deviceClaimKey(*rxDev)) { same = true; return &a.devices[(size_t)i]; }
    for (int i : radios) {
        const DeviceInfo& dv = a.devices[(size_t)i];
        const int h = deviceHolder(dv);
        if (h == 0 || h == kTxClaimOwner || dv.txFullDuplex) return &dv;
    }
    return nullptr;
}

void resumeRx(App& a) {
    W& d = w();
    if (!d.pausedRx) return;
    d.pausedRx = false;
    startReceiver(a);
}

void txFail(App& a, const std::string& why) {
    W& d = w();
    d.error = why;
    addLog("transmit: " + why);
    if (d.claimed) { txReleaseRadio(); d.claimed = false; mapTransmitAbort(a); }
    d.sending = false;
    d.outAx.clear(); d.outAr.clear();
    resumeRx(a);
    if (d.st && d.st->running()) d.st->stop(mono()), d.st->stop(mono());
}

void sendTick(App& a) {
    W& d = w();
    if (d.sending) {
        if (d.session.running()) return;
        const TxSession::Stats s = d.session.stats();
        d.session.stop();
        if (d.claimed) { txReleaseRadio(); d.claimed = false; mapTransmitEnded(); }
        d.sending = false;
        if (s.failed) { txFail(a, s.error.empty() ? std::string("the radio stopped") : s.error); return; }
    }
    if (d.outAx.empty() && d.outAr.empty()) { resumeRx(a); return; }
    if (d.askNotice) return;
    if (!plat::basePrefs().getB("txNoticeAccepted", false)) { d.askNotice = true; return; }
    if (txActive()) { txFail(a, "the Transmit window is sending: stop it first"); return; }
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    if (!radio) { txFail(a, "no radio here can transmit (a HackRF, PlutoSDR, LimeSDR, USRP or bladeRF can)"); return; }
    const bool ardop = !d.outAr.empty();
    const TxModeSpec* mode = txModeById(ardop ? "winlinkardop" : "winlinkpkt");
    if (!mode) { txFail(a, "this build has no Winlink transmitter"); return; }
    const DeviceInfo dev = *radio;
    if (same && !dev.txFullDuplex && a.engine.running()) { a.engine.stop(); d.pausedRx = true; }
    std::string err;
    if (!txClaimRadio(dev, err)) { txFail(a, err); return; }
    d.claimed = true;
    std::unique_ptr<TxSink> sink = makeTxSink(dev);
    if (!sink) { const std::string why = txUnavailableReason(dev); txFail(a, why.empty() ? std::string("this radio has no transmit driver in OnAir") : why); return; }
    TxRequest q;
    q.modeId = mode->id;
    q.payload = ardop ? winlinkTxPayloadArdop(d.outAr) : winlinkTxPayloadAx25(d.tr == Transport::Packet9600 ? kModemPacket9600 : kModemPacket1200, d.outAx);
    q.loop = true;   // the generator ends after the burst
    q.tune.centerHz = a.freqMhz * 1e6;
    q.tune.gainDb = dev.txGainMaxDb > dev.txGainMinDb ? std::clamp((double)d.txGain, dev.txGainMinDb, dev.txGainMaxDb) : d.txGain;
    q.tune.ampOn = false;
    mapPauseForTransmit(a);
    if (!d.session.start(std::move(sink), q, TxRateLimits{dev.txMinRateHz, dev.txMaxRateHz}, err)) { txFail(a, err.empty() ? std::string("the radio did not start") : err); return; }
    d.sending = true;
    d.outAx.clear(); d.outAr.clear();
}

void noticePopup() {
    W& d = w();
    if (d.askNotice && !ImGui::IsPopupOpen("##wlnotice")) ImGui::OpenPopup("##wlnotice");
    ImGui::SetNextWindowSize(ImVec2(480 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##wlnotice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextColored(pal::heading(), "%s", tr("Before you transmit"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Transmitting needs a licence or permission where you are. You are responsible for what you emit and for not interfering with others "
                                  "(including services that people rely on for safety). Connect an antenna, an attenuator or a dummy load to the radio before you start. "
                                  "OnAir does not limit the frequency or the power: the radio's own limits are the only ones."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("I understand"), ImVec2(220 * gUi, 0))) { plat::basePrefs().setB("txNoticeAccepted", true); plat::basePrefs().flush(); d.askNotice = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(-1, 0))) { d.askNotice = false; d.outAx.clear(); d.outAr.clear(); if (d.st) { d.st->stop(mono()); d.st->stop(mono()); } ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- session

void setTaps(App& a, bool on) {
    W& d = w();
    if (on == d.tapsOn) return;
    d.tapsOn = on;
    if (!on) {
        a.engine.packet().setFrameTap(nullptr);
        a.engine.ardop().setFrameTap(nullptr);
        return;
    }
    d.taps = std::make_shared<Taps>();
    std::weak_ptr<Taps> wt = d.taps;
    a.engine.packet().setFrameTap([wt](const ax25::Frame& f, int) { if (auto t = wt.lock()) { std::lock_guard<std::mutex> lk(t->mu); t->ax.push_back(f); } });
    a.engine.ardop().setFrameTap([wt](const ArdopFrameEvent& e) {
        if (auto t = wt.lock()) {
            ArdopRxFrameIn f;
            f.code = e.code; f.sid = e.sid; f.bodyOk = e.bodyOk; f.from = e.from; f.to = e.to; f.payload = e.payload;
            std::lock_guard<std::mutex> lk(t->mu); t->ar.push_back(std::move(f));
        }
    });
}

void connect(App& a) {
    W& d = w();
    d.error.clear();
    if (d.gwSel < 0 || d.gwSel >= (int)d.gws.size()) { d.error = tr("add a gateway first (Gateways)"); return; }
    const Gateway g = d.gws[(size_t)d.gwSel];
    StationConfig c;
    c.mycall = d.call; c.locator = d.grid; c.password = d.password;
    c.gateway = g;
    c.telnetEnabled = d.telnetOn;
    c.p2p = g.peer;
    c.gzip = d.gzipOn;
    c.resume = d.resumeOn;
    if (g.transport == Transport::Modem) {
        const ModemProfile* mp = findModem(g.modem);
        if (!mp) { d.error = tr("this gateway uses a modem profile that does not exist: add it under Modems"); return; }
        c.modem = *mp;
    }
    std::string err;
    if (!d.st->start(c, mono(), err)) { d.error = err; return; }
    d.tr = g.transport;
    if (g.transport != Transport::Telnet && g.transport != Transport::Modem) {   // an external modem owns its radio: OnAir neither tunes nor transmits
        if (g.mhz > 0 && std::fabs(a.freqMhz - g.mhz) > 1e-6) tuneFreq(a, g.mhz);
        if (!a.engine.running()) startReceiver(a);
        setTaps(a, true);
    }
}

// Peer to peer, the answering side: on the selected entry's link and frequency (packet or ARDOP), any station may connect
void listenP2p(App& a, int ctx) {
    W& d = w();
    d.error.clear();
    Gateway g;
    if (d.gwSel >= 0 && d.gwSel < (int)d.gws.size() && d.gws[(size_t)d.gwSel].transport != Transport::Telnet) g = d.gws[(size_t)d.gwSel];
    else g.transport = ctx == 1 ? Transport::Ardop : Transport::Packet1200;
    StationConfig c;
    c.mycall = d.call; c.locator = d.grid;
    c.gateway = g;
    c.gzip = d.gzipOn;
    c.resume = d.resumeOn;
    if (g.transport == Transport::Modem) {
        const ModemProfile* mp = findModem(g.modem);
        if (!mp) { d.error = tr("this gateway uses a modem profile that does not exist: add it under Modems"); return; }
        c.modem = *mp;
    }
    std::string err;
    if (!d.st->listen(c, mono(), err)) { d.error = err; return; }
    d.tr = g.transport;
    if (g.transport == Transport::Modem) return;
    if (g.mhz > 0 && std::fabs(a.freqMhz - g.mhz) > 1e-6) tuneFreq(a, g.mhz);
    if (!a.engine.running()) startReceiver(a);
    setTaps(a, true);
}

} // namespace

void winlinkTick(App& a) {
    load();
    W& d = w();
    const double now = mono();
    if (d.taps) {
        std::vector<ax25::Frame> ax;
        std::vector<ArdopRxFrameIn> ar;
        { std::lock_guard<std::mutex> lk(d.taps->mu); ax.swap(d.taps->ax); ar.swap(d.taps->ar); }
        if (!d.sending)   // our own burst is not heard back while sending: the radio is the transmitter then
            for (auto& f : ax) d.st->rxAx25(f, now);
        if (!d.sending)
            for (auto& f : ar) d.st->rxArdop(f, now);
    }
    d.st->tick(now);
    for (auto& l : d.st->takeLog()) addLog(l);
    for (auto& f : d.st->takeAx25()) d.outAx.push_back(std::move(f));
    for (auto& f : d.st->takeArdop()) d.outAr.push_back(std::move(f));
    if (d.sending || !d.outAx.empty() || !d.outAr.empty() || d.pausedRx) sendTick(a);
    if (!d.st->running() && d.tapsOn && !d.sending) { setTaps(a, false); d.taps.reset(); d.listAt = -1; }
}

namespace {

void mailList(Folder f) {
    W& d = w();
    const double now = mono();
    if (d.listFolder != (int)f || now - d.listAt > 2) { d.list = d.mb.list(f); d.listFolder = (int)f; d.listAt = now; }
    if (d.list.empty()) { ImGui::TextDisabled("%s", tr("no messages")); return; }
    const float h = ImGui::GetContentRegionAvail().y * 0.45f;
    if (ImGui::BeginTable("##wlmail", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Date (UTC)")); ImGui::TableSetupColumn(f == Folder::Inbox ? tr("From") : tr("To"));
        ImGui::TableSetupColumn(tr("Subject"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Size"));
        ImGui::TableHeadersRow();
        for (const MailEntry& e : d.list) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool sel = d.selMid == e.mid;
            char id[64]; snprintf(id, sizeof id, "%s##%s", e.date.c_str(), e.mid.c_str());
            if (ImGui::Selectable(id, sel, ImGuiSelectableFlags_SpanAllColumns)) { d.selMid = e.mid; d.mb.load(f, e.mid, d.preview); }
            ImGui::TableNextColumn();
            std::string who = f == Folder::Inbox ? e.from : (e.to.empty() ? std::string() : e.to[0] + (e.to.size() > 1 ? " +" + std::to_string(e.to.size() - 1) : ""));
            ImGui::TextUnformatted(who.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.subject.c_str()); if (e.files) { ImGui::SameLine(); ImGui::TextDisabled("(%d %s)", e.files, tr("files")); }
            ImGui::TableNextColumn(); ImGui::Text("%zu", e.bytes);
        }
        ImGui::EndTable();
    }
    if (d.selMid.empty() || d.preview.mid() != d.selMid) return;
    const B2Message& m = d.preview;
    ImGui::Separator();
    ImGui::Text("%s %s", tr("From"), m.from().c_str());
    std::string to;
    for (const auto& t : m.to()) to += (to.empty() ? "" : ", ") + t;
    ImGui::Text("%s %s", tr("To"), to.c_str());
    ImGui::Text("%s %s", tr("Subject"), m.subject().c_str());
    if (f == Folder::Inbox && ImGui::Button(tr("Reply"))) {
        snprintf(d.to, sizeof d.to, "%s", m.from().c_str());
        snprintf(d.subject, sizeof d.subject, "Re: %s", m.subject().c_str());
        d.body[0] = 0;
        d.view = 3;
    }
    ImGui::SameLine();
    if (ImGui::Button(f == Folder::Outbox ? tr("Delete") : tr("Archive"))) {
        if (f == Folder::Outbox) d.mb.remove(f, d.selMid); else d.mb.move(d.selMid, f, Folder::Archive);
        d.selMid.clear(); d.listAt = -1;
        return;
    }
    for (const auto& fl : m.files) {
        ImGui::SameLine();
        char b[160]; snprintf(b, sizeof b, "%s %s (%zu)##sv%s", tr("Save"), fl.name.c_str(), fl.data.size(), fl.name.c_str());
        if (ImGui::SmallButton(b)) {
            std::string nm = fl.name;
            for (auto& c : nm) if (c == '/' || c == '\\' || c == ':') c = '_';
            const std::string p = plat::dataDir() + "/winlink/files/" + nm;
            std::error_code ec;
            std::filesystem::create_directories(plat::dataDir() + "/winlink/files", ec);
            std::ofstream o(p, std::ios::binary);
            o.write((const char*)fl.data.data(), (std::streamsize)fl.data.size());
            addLog(o ? "saved " + p : "could not save " + p);
        }
    }
    if (ImGui::BeginChild("##wlbody", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0);
        std::string b = m.bodyText();
        b.erase(std::remove(b.begin(), b.end(), '\r'), b.end());
        ImGui::TextUnformatted(b.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

std::vector<std::string> splitList(const char* s) {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = s; ; p++) {
        if (*p == ',' || *p == ';' || *p == 0) {
            while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1);
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else cur += *p;
    }
    return v;
}

void compose() {
    W& d = w();
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wlto", tr("To: call signs or e-mail addresses, comma separated"), d.to, sizeof d.to);
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wlcc", tr("Cc"), d.cc, sizeof d.cc);
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wlsubj", tr("Subject"), d.subject, sizeof d.subject);
    ImGui::InputTextMultiline("##wlbody", d.body, sizeof d.body, ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.6f));
    ImGui::SetNextItemWidth(320 * gUi); ImGui::InputTextWithHint("##wlatt", tr("file to attach (path)"), d.attachPath, sizeof d.attachPath);
    ImGui::SameLine();
    if (ImGui::Button(tr("Attach")) && d.attachPath[0]) {
        std::ifstream f(d.attachPath, std::ios::binary);
        if (!f) d.composeNote = tr("cannot read that file");
        else {
            B2File bf;
            bf.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            bf.name = std::filesystem::path(d.attachPath).filename().string();
            d.composeNote = bf.data.size() > 120000 ? tr("a large file: it takes a long time on the radio") : std::string();
            d.attach.push_back(std::move(bf));
            d.attachPath[0] = 0;
        }
    }
    for (size_t i = 0; i < d.attach.size(); i++) {
        char b[160]; snprintf(b, sizeof b, "x %s (%zu)##wlatt%zu", d.attach[i].name.c_str(), d.attach[i].data.size(), i);
        if (ImGui::SmallButton(b)) { d.attach.erase(d.attach.begin() + (long)i); break; }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::Button(tr("Put in the outbox"), ImVec2(220 * gUi, 0))) {
        const auto to = splitList(d.to);
        if (!d.call[0]) d.composeNote = tr("enter your call sign in Settings first");
        else if (to.empty()) d.composeNote = tr("enter at least one recipient");
        else {
            B2Message m = makeMessage(d.call, to, splitList(d.cc), d.subject, d.body, d.attach, std::time(nullptr));
            std::string err;
            if (d.mb.save(Folder::Outbox, m, &err)) {
                d.composeNote = std::string(tr("in the outbox: sent at the next connection")) + " (" + m.mid() + ")";
                d.to[0] = d.cc[0] = d.subject[0] = d.body[0] = 0;
                d.attach.clear();
                d.listAt = -1;
            } else d.composeNote = err;
        }
    }
    if (!d.composeNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", d.composeNote.c_str()); }
}

void gateways(int ctx) {
    W& d = w();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Gateways are entered by hand: OnAir does not look them up on the internet. Winlink publishes the list of gateways (RMS) with their frequencies."));
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##wlgw", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Gateway")); ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("MHz / host"));
        ImGui::TableSetupColumn(tr("Via")); ImGui::TableSetupColumn(tr("Note"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < d.gws.size(); i++) {
            const Gateway& g = d.gws[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[64]; snprintf(id, sizeof id, "%s##wlg%zu", g.call.empty() ? "CMS" : g.call.c_str(), i);
            if (ImGui::Selectable(id, d.gwSel == (int)i)) { d.gwSel = (int)i; plat::prefs().setI("winlinkGateway", d.gwSel); }
            ImGui::TableNextColumn(); ImGui::Text("%s%s", tr(transportName(g.transport)), g.peer ? tr(", peer to peer") : "");
            ImGui::TableNextColumn();
            if (g.transport == Transport::Telnet) ImGui::Text("%s:%d", g.host.c_str(), g.port);
            else if (g.transport == Transport::Modem) ImGui::TextUnformatted(g.modem.c_str());
            else ImGui::Text("%.4f", g.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(g.via.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(g.note.c_str());
            ImGui::TableNextColumn();
            char rb[32]; snprintf(rb, sizeof rb, "%s##wlrm%zu", tr("Remove"), i);
            if (ImGui::SmallButton(rb)) { d.gws.erase(d.gws.begin() + (long)i); if (d.gwSel >= (int)d.gws.size()) d.gwSel = (int)d.gws.size() - 1; saveGws(); break; }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Add a gateway"));
    static const char* kTr[] = {N_("Packet 1200"), N_("Packet 9600"), N_("ARDOP"), N_("Telnet"), N_("External modem")};
    if (d.gTransport == 0 && ctx == 1) d.gTransport = 2;
    {
        const char* trs[5];
        for (int i = 0; i < 5; i++) trs[i] = tr(kTr[i]);
        ImGui::SetNextItemWidth(160 * gUi); ImGui::Combo("##wlgt", &d.gTransport, trs, 5);
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(140 * gUi); ImGui::InputTextWithHint("##wlgc", tr("call sign"), d.gCall, sizeof d.gCall);
    if (d.gTransport == 3) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(200 * gUi); ImGui::InputTextWithHint("##wlgh", tr("host"), d.gHost, sizeof d.gHost);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt("##wlgp", &d.gPort, 0);
    } else if (d.gTransport == 4) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(200 * gUi);
        if (d.gModem >= (int)d.modems.size()) d.gModem = -1;
        if (d.gModem < 0 && !d.modems.empty()) d.gModem = 0;
        const char* cur = d.gModem >= 0 ? d.modems[(size_t)d.gModem].name.c_str() : tr("(no modem profile: add one under Modems)");
        if (ImGui::BeginCombo("##wlgmod", cur)) {
            for (size_t i = 0; i < d.modems.size(); i++) {
                const std::string l = d.modems[i].name + " (" + modemTypeName(d.modems[i].type) + ")##gm" + std::to_string(i);
                if (ImGui::Selectable(l.c_str(), d.gModem == (int)i)) d.gModem = (int)i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputTextWithHint("##wlgv", tr("digipeaters (VARA FM, optional)"), d.gVia, sizeof d.gVia);
    } else {
        ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi); ImGui::InputDouble("MHz##wlgm", &d.gMhz, 0, 0, "%.4f");
        if (d.gTransport != 2) { ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputTextWithHint("##wlgvia", tr("digipeaters (optional)"), d.gVia, sizeof d.gVia); }
    }
    ImGui::SetNextItemWidth(300 * gUi); ImGui::InputTextWithHint("##wlgn", tr("note (optional)"), d.gNote, sizeof d.gNote);
    if (d.gTransport != 3) { ImGui::SameLine(); ImGui::Checkbox(tr("another station (peer to peer)"), &d.gPeer); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Add"))) {
        Gateway g;
        g.call = d.gCall; g.transport = (Transport)d.gTransport; g.mhz = d.gMhz; g.via = d.gVia; g.host = d.gHost; g.port = std::max(1, std::min(65535, d.gPort)); g.note = d.gNote;
        g.peer = d.gPeer && g.transport != Transport::Telnet;
        for (auto& c : g.call) c = (char)toupper((unsigned char)c);
        if (g.transport == Transport::Modem) {
            if (d.gModem >= 0 && d.gModem < (int)d.modems.size()) g.modem = d.modems[(size_t)d.gModem].name;
            g.mhz = 0;
        }
        if (g.transport == Transport::Telnet ? !g.host.empty() : (!g.call.empty() && (g.transport != Transport::Modem || !g.modem.empty()))) {
            d.gws.push_back(g);
            d.gwSel = (int)d.gws.size() - 1;
            plat::prefs().setI("winlinkGateway", d.gwSel);
            saveGws();
            d.gCall[0] = d.gVia[0] = d.gHost[0] = d.gNote[0] = 0;
        }
    }
}

// ---------------------------------------------------------------- external modems

// The help lines come from the core (modemHelp in core/src/winlink/wl_extmodem.cpp) and are translated line by line in modemHelpBlock:
// listed here so that the catalog check (tests/app/test_i18n.cpp) knows them.
[[maybe_unused]] static const char* const kModemHelpTexts[] = {
    N_("OnAir does not include, start or bundle this modem: it is a separate program that OnAir connects to over TCP."),
    N_("The modem program owns the sound card and the radio's PTT. Set the audio device and PTT in the modem program, not in OnAir."),
    N_("Use Test connection: it opens the two ports and asks for the version; nothing is sent over the air."),
    N_("1. Install ardopcf (open source) and start it: ardopcf 8515 <capture device> <playback device> (see its USAGE files)."),
    N_("2. In OnAir add an ARDOP profile: host 127.0.0.1, command port 8515, data port 8516."),
    N_("3. Choose the BW (ARQBW), for example 500MAX, or leave it empty for the modem's own setting."),
    N_("4. Add a gateway with link type External modem, pick the profile, enter the gateway's call sign."),
    N_("ardopcf listens on all network interfaces by default: keep the ports behind a firewall."),
    N_("ardopcf is open source (MIT licence). ARDOP's specification is public."),
    N_("1. Install VARA HF from its author's site (EA5HVK) and start it. It is Windows software."),
    N_("2. Linux and macOS: people run it under Wine in its own prefix (see the Pat wiki page on VARA and the DF3FY guide, linked in docs/HFMODEMS-3.9.md). OnAir cannot check that setup."),
    N_("3. In OnAir add a VARA HF profile: host 127.0.0.1, command port 8300, data port 8301 (the defaults; check VARA's Settings > TCP ports)."),
    N_("4. BW: 500, 2300 (the default) or 2750. Add a gateway with link type External modem and pick the profile."),
    N_("5. For a peer-to-peer session mark the gateway entry as another station: OnAir then sends P2P SESSION."),
    N_("VARA's free version is speed limited; a licence key (bought from the author) removes the limit. OnAir does not read or change it."),
    N_("VARA is closed, proprietary, Windows-only software by EA5HVK. Its licence and any registration are between you and its author. OnAir does not contain or decode VARA."),
    N_("1. Install VARA FM from its author's site and start it. It is Windows software."),
    N_("2. In OnAir add a VARA FM profile: host 127.0.0.1, command port 8300, data port 8301 (VARA FM and HF both default to these, so run one at a time or change one program's ports)."),
    N_("3. VARA FM has no BW command. Digipeaters go in the gateway's Via field and are sent as: CONNECT <mycall> <call> via <digi1> <digi2>."),
    N_("4. Add a gateway with link type External modem and pick the profile."),
    N_("VARA FM is closed, proprietary, Windows-only software by EA5HVK. OnAir does not contain or decode it."),
    N_("1. Install Mercury (Rhizomatica, open source) and start it; it exposes a VARA-style TCP interface."),
    N_("2. In OnAir add a Mercury profile: host 127.0.0.1, command port 8300, data port 8301 (Mercury: -p <base_port>, data = base + 1)."),
    N_("3. BW: 500, 2300 or 2750 (2750 behaves as 2300 today). Add a gateway with link type External modem and pick the profile."),
    N_("A Mercury call can last up to 300 s before it gives up: the connect timeout of the profile is 330 s unless you set one."),
    N_("Mercury is open source (GPL-3.0-or-later; some bundled parts under LGPL, MIT, Unlicense)."),
};

void modemHelpBlock(ModemType t) {
    const ModemHelp h = modemHelp(t);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 38);
    size_t i = 0;
    while (i < h.steps.size()) {
        size_t e = h.steps.find('\n', i);
        if (e == std::string::npos) e = h.steps.size();
        const std::string line = h.steps.substr(i, e - i);
        if (!line.empty()) ImGui::TextUnformatted(tr(line.c_str()));
        i = e + 1;
    }
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr(h.licence.c_str()));
    ImGui::Text("%s %s", tr("Get it from:"), h.installUrl.c_str());
    ImGui::TextDisabled("%s %s", tr("Interface taken from:"), h.source.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton(tr("Copy the download link"))) ImGui::SetClipboardText(h.installUrl.c_str());
}

void loadModemEditor(const ModemProfile& m) {
    W& d = w();
    d.mType = (int)m.type; d.mCmd = m.cmdPort; d.mData = m.dataPort; d.mTimeout = m.connectTimeoutSec;
    snprintf(d.mName, sizeof d.mName, "%s", m.name.c_str());
    snprintf(d.mHost, sizeof d.mHost, "%s", m.host.c_str());
    snprintf(d.mBw, sizeof d.mBw, "%s", m.bw.c_str());
    snprintf(d.mPtt, sizeof d.mPtt, "%s", m.pttNote.c_str());
    snprintf(d.mAudio, sizeof d.mAudio, "%s", m.audioNote.c_str());
}

ModemProfile editorProfile() {
    W& d = w();
    ModemProfile m;
    m.type = (ModemType)std::max(0, std::min(3, d.mType));
    m.name = d.mName; m.host = d.mHost;
    m.cmdPort = std::max(1, std::min(65535, d.mCmd));
    m.dataPort = std::max(1, std::min(65535, d.mData));
    m.bw = d.mBw; m.connectTimeoutSec = std::max(0, std::min(3600, d.mTimeout));
    m.pttNote = d.mPtt; m.audioNote = d.mAudio;
    return m;
}

void modems() {
    W& d = w();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("OnAir does not include, start or bundle ARDOP, VARA or Mercury. Each is a separate program that does the radio part; OnAir connects to it over TCP and carries the Winlink session through it."));
    ImGui::TextDisabled("%s", tr("Tested only against mock modems built from the published command descriptions: no real ARDOP, VARA or Mercury modem was used."));
    ImGui::PopTextWrapPos();
    if (d.mtest && d.mtest->done) { d.mtestMsg = d.mtest->r.message; d.mtest.reset(); }
    if (ImGui::BeginTable("##wlmod", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Host : command / data port"));
        ImGui::TableSetupColumn(tr("BW"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < d.modems.size(); i++) {
            const ModemProfile& m = d.modems[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[96]; snprintf(id, sizeof id, "%s##wlm%zu", m.name.c_str(), i);
            if (ImGui::Selectable(id, d.modemSel == (int)i)) { d.modemSel = (int)i; loadModemEditor(m); d.mtestMsg.clear(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(modemTypeName(m.type));
            ImGui::TableNextColumn(); ImGui::Text("%s : %d / %d", m.host.c_str(), m.cmdPort, m.dataPort);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.bw.empty() ? tr("modem's own") : m.bw.c_str());
            ImGui::TableNextColumn();
            char rb[32]; snprintf(rb, sizeof rb, "%s##wlmr%zu", tr("Remove"), i);
            if (ImGui::SmallButton(rb)) { d.modems.erase(d.modems.begin() + (long)i); d.modemSel = -1; saveModems(); break; }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(d.modemSel >= 0 ? tr("Edit the modem profile") : tr("Add a modem profile"));
    ImGui::SameLine();
    if (ImGui::SmallButton("?##wlmodhelp")) ImGui::OpenPopup("##wlmodhelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Setup steps for this modem"));
    if (ImGui::BeginPopup("##wlmodhelppop")) { modemHelpBlock((ModemType)std::max(0, std::min(3, d.mType))); ImGui::EndPopup(); }
    static const char* kT[] = {N_("ARDOP"), N_("VARA HF"), N_("VARA FM"), N_("Mercury")};
    const char* trs[4];
    for (int i = 0; i < 4; i++) trs[i] = tr(kT[i]);
    ImGui::SetNextItemWidth(130 * gUi);
    const int before = d.mType;
    if (ImGui::Combo("##wlmt", &d.mType, trs, 4) && d.mType != before) {
        const ModemProfile def = defaultModemProfile((ModemType)d.mType);
        d.mCmd = def.cmdPort; d.mData = def.dataPort; d.mBw[0] = 0;
        if (!d.mName[0]) snprintf(d.mName, sizeof d.mName, "%s", def.name.c_str());
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(180 * gUi); ImGui::InputTextWithHint("##wlmn", tr("name"), d.mName, sizeof d.mName);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputTextWithHint("##wlmh", tr("host"), d.mHost, sizeof d.mHost);
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt(tr("command port##wlmc"), &d.mCmd, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt(tr("data port##wlmd"), &d.mData, 0);
    const std::vector<std::string> bws = modemBwChoices((ModemType)std::max(0, std::min(3, d.mType)));
    if (bws.empty()) ImGui::TextDisabled("%s", tr("This modem type has no BW command."));
    else {
        ImGui::SetNextItemWidth(110 * gUi); ImGui::InputTextWithHint("##wlmb", tr("BW (empty: modem's own)"), d.mBw, sizeof d.mBw);
        ImGui::SameLine();
        if (ImGui::BeginCombo("##wlmbc", tr("choices"), ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLarge)) {
            for (const std::string& b : bws) if (ImGui::Selectable(b.c_str())) snprintf(d.mBw, sizeof d.mBw, "%s", b.c_str());
            ImGui::EndCombo();
        }
        if (d.mBw[0] && !modemBwValid((ModemType)d.mType, d.mBw)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s", tr("not a BW this modem documents: it will not be sent")); }
    }
    ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt(tr("connect timeout, s (0: default)##wlmt2"), &d.mTimeout, 0);
    ImGui::SetNextItemWidth(260 * gUi); ImGui::InputTextWithHint("##wlmp", tr("PTT note (set it in the modem program)"), d.mPtt, sizeof d.mPtt);
    ImGui::SameLine(); ImGui::SetNextItemWidth(260 * gUi); ImGui::InputTextWithHint("##wlma", tr("audio note (set it in the modem program)"), d.mAudio, sizeof d.mAudio);
    const bool nameOk = d.mName[0] != 0 && d.mHost[0] != 0;
    if (ImGui::Button(d.modemSel >= 0 ? tr("Save changes") : tr("Add")) && nameOk) {
        ModemProfile m = editorProfile();
        if (d.modemSel >= 0 && d.modemSel < (int)d.modems.size()) {
            const std::string old = d.modems[(size_t)d.modemSel].name;
            d.modems[(size_t)d.modemSel] = m;
            if (old != m.name) for (Gateway& g : d.gws) if (g.transport == Transport::Modem && g.modem == old) g.modem = m.name;
            saveGws();
        } else { d.modems.push_back(m); d.modemSel = (int)d.modems.size() - 1; }
        saveModems();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("New profile"))) { d.modemSel = -1; d.mName[0] = 0; const ModemProfile def = defaultModemProfile((ModemType)d.mType); d.mCmd = def.cmdPort; d.mData = def.dataPort; }
    ImGui::SameLine();
    ImGui::BeginDisabled(d.mtest != nullptr);
    if (ImGui::Button(tr("Test connection")) && d.mHost[0]) {
        d.mtest = std::make_shared<ModemTest>();
        d.mtestMsg = tr("testing...");
        std::shared_ptr<ModemTest> t = d.mtest;
        const ModemProfile p = editorProfile();
        std::thread([t, p] { t->r = testModemProfile(p, 3000); t->done = true; }).detach();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Opens the two ports and asks for the version. Nothing is sent over the air."));
    if (!d.mtestMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(d.mtestMsg.c_str()); ImGui::PopTextWrapPos(); }
}

void settings() {
    W& d = w();
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputTextWithHint(tr("Your call sign"), "LY2ABC", d.call, sizeof d.call)) { for (char* p = d.call; *p; p++) *p = (char)toupper((unsigned char)*p); plat::prefs().setS("winlinkCall", d.call); }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputTextWithHint(tr("Locator"), "KO24", d.grid, sizeof d.grid)) plat::prefs().setS("winlinkGrid", d.grid);
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputText(tr("Winlink password"), d.password, sizeof d.password, ImGuiInputTextFlags_Password);
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(kept in memory for this run only)"));
    if (ImGui::Checkbox(tr("Allow Telnet to the Winlink CMS (internet)"), &d.telnetOn)) plat::prefs().setB("winlinkTelnet", d.telnetOn);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Off by default. When on, a Telnet gateway in the list connects over the internet to the host you entered; nothing connects until you press Connect."));
    ImGui::PopTextWrapPos();
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        if (ImGui::Checkbox(tr("Resume a message that was cut off (ask only for the rest)"), &d.resumeOn)) plat::prefs().setB("winlinkResume", d.resumeOn);
        const std::vector<std::string> parts = d.mb.partials();
        if (!parts.empty()) {
            ImGui::TextDisabled(tr("%d message(s) held part way:"), (int)parts.size());
            for (const std::string& m : parts) {
                ImGui::BulletText("%s", m.c_str());
                ImGui::SameLine();
                char b[48]; snprintf(b, sizeof b, "%s##wlpart%s", tr("Drop"), m.c_str());
                if (ImGui::SmallButton(b)) d.mb.removePartial(m);
            }
        }
        if (ImGui::Checkbox(tr("Offer gzip compression (experimental, as Pat's GZIP_EXPERIMENT)"), &d.gzipOn)) plat::prefs().setB("winlinkGzip", d.gzipOn);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Used only when the other side offers it too (\"G\" in its SID); otherwise messages go LZHUF compressed as usual."));
        ImGui::PopTextWrapPos();
    }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat(tr("Transmit gain (dB)"), &d.txGain, 0, 60, "%.0f")) plat::prefs().setD("winlinkTxGain", d.txGain);
    ImGui::TextDisabled("%s %s", tr("Mailbox folder:"), d.mb.dir().c_str());
}

} // namespace

void winlinkPanel(App& a, int ctx) {
    load();
    W& d = w();
    noticePopup();
    // the connection row: gateway, Connect / Stop, status
    const bool run = d.st->running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Gateway"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * gUi);
    std::string cur = d.gwSel >= 0 && d.gwSel < (int)d.gws.size() ? d.gws[(size_t)d.gwSel].call + " (" + transportName(d.gws[(size_t)d.gwSel].transport) + ")" : std::string(tr("none"));
    if (ImGui::BeginCombo("##wlgsel", cur.c_str())) {
        for (size_t i = 0; i < d.gws.size(); i++) {
            if (!fits(ctx, d.gws[i].transport)) continue;
            const std::string l = (d.gws[i].call.empty() ? std::string("CMS") : d.gws[i].call) + " (" + transportName(d.gws[i].transport) + ")##" + std::to_string(i);
            if (ImGui::Selectable(l.c_str(), d.gwSel == (int)i)) { d.gwSel = (int)i; plat::prefs().setI("winlinkGateway", d.gwSel); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (!run) {
        if (ImGui::Button(tr("Connect"), ImVec2(120 * gUi, 0))) connect(a);
        ImGui::SameLine();
        if (ImGui::Button(tr("Listen for a station"))) listenP2p(a, ctx);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Peer to peer: another station connects to you on the selected entry's link and frequency (tested between two OnAir stations only)."));
    } else if (ImGui::Button(d.st->listening() ? tr("Stop listening") : tr("Stop"), ImVec2(120 * gUi, 0))) d.st->stop(mono());
    ImGui::SameLine();
    const std::string s = d.st->status();
    if (!d.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s", d.error.c_str());
    else ImGui::TextDisabled("%s%s", s.c_str(), d.sending ? ", transmitting" : "");
    subNav("wlv", d.view, {"Inbox", "Outbox", "Sent", "Compose", "Gateways", "Modems", "Settings"});
    const float logH = 90 * gUi;
    if (ImGui::BeginChild("##wlmain", ImVec2(0, ImGui::GetContentRegionAvail().y - logH))) {
        switch (d.view) {
        case 0: mailList(Folder::Inbox); break;
        case 1: mailList(Folder::Outbox); break;
        case 2: mailList(Folder::Sent); break;
        case 3: compose(); break;
        case 4: gateways(ctx); break;
        case 5: modems(); break;
        default: settings(); break;
        }
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("##wllog", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, 0);
        for (const auto& l : d.log) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}
