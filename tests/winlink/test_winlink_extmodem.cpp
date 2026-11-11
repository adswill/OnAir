// External modems for Winlink (3.9, docs/HFMODEMS-3.9.md): a mock ARDOP TNC, a mock VARA modem (HF and FM) and a mock Mercury, each a local TCP
// listener on 127.0.0.1 that speaks the command set as published (ardopcf Host_Interface_Commands.md, VARA "Native TNC Commands", Mercury
// docs/TNC.md). They are written from those descriptions: NO real ARDOP, VARA or Mercury program was used, and nothing here touches a real
// Winlink gateway, the CMS or a radio. The flows: connect and a whole B2F session through the data port against a simulated gateway, listen
// and accept a peer-to-peer call, buffer / S/N / busy status, a modem that is not running, a modem that goes away mid-session, a call nobody
// answers, a rejected call, user stop, the Test connection helper, profile storage, and command-injection safety.
#include "winlink_harness.h"
#include "../../core/src/net/tcp_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
using namespace wltest;
#include "../common/test_util.h"

namespace {

struct MockModem {
    ModemType type = ModemType::Ardop;
    // behaviour
    std::string callOutcome = "ok";        // ok | never | wrong | disc
    bool versionReply = true;
    size_t dropAfterBytes = 0;             // close both sockets after this many data bytes arrived (0: never)
    // what happened
    std::mutex mu;
    std::vector<std::string> cmds;
    std::vector<uint8_t> gotData;
    std::vector<B2Message> gwGot;
    std::string inboundFrom;               // listen test: the caller to inject once LISTEN was seen
    std::atomic<bool> injectCall{false};
    std::atomic<bool> stop{false};
    // plumbing
    sock_t lc = kBadSock, ld = kBadSock, cs = kBadSock, ds = kBadSock;
    int cmdPort = 0, dataPort = 0;
    std::thread th;
    MockGateway gw;
    std::unique_ptr<B2fSession> peerClient;   // listen test: this mock calls in as a P2P station
    std::vector<B2Message> peerGot;
    std::vector<B2Message> peerOutbox;
    std::string lineBuf, calledMy, calledPeer;
    bool connected = false, listening = false;
    std::vector<uint8_t> frameBuf;
    bool asCaller = false, firstData = false, dataGone = false;

    static sock_t mkListen(int& port) {
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (s == kBadSock || bind(s, (const sockaddr*)&a, sizeof a) != 0 || listen(s, 2) != 0) return kBadSock;
        socklen_t al = sizeof a;
        getsockname(s, (sockaddr*)&a, &al);
        port = ntohs(a.sin_port);
        return s;
    }
    static int freePort() { int p = 0; sock_t s = mkListen(p); if (s != kBadSock) sockClose(s); return p; }

    bool arq() const { return type == ModemType::Ardop; }
    void sendLine(const std::string& l) { const std::string x = l + "\r"; sockSendAll(cs, x.data(), x.size()); }
    void sendData(const uint8_t* d, size_t n) {
        if (ds == kBadSock) return;
        if (arq()) {
            size_t off = 0;
            while (off < n) {
                const size_t k = std::min<size_t>(n - off, 100);   // frames of a few bytes, as ARQ frames arrive
                std::vector<uint8_t> f;
                f.push_back((uint8_t)((k + 3) >> 8)); f.push_back((uint8_t)((k + 3) & 0xFF));
                f.push_back('A'); f.push_back('R'); f.push_back('Q');
                f.insert(f.end(), d + off, d + off + k);
                sockSendAll(ds, f.data(), f.size());
                off += k;
            }
        } else sockSendAll(ds, d, n);
    }
    void record(const std::string& c) { std::lock_guard<std::mutex> lk(mu); cmds.push_back(c); }
    bool saw(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& c : cmds) if (c.rfind(prefix, 0) == 0) return true;
        return false;
    }
    std::vector<std::string> all() { std::lock_guard<std::mutex> lk(mu); return cmds; }
    int index(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < cmds.size(); i++) if (cmds[i].rfind(prefix, 0) == 0) return (int)i;
        return -1;
    }

    void becomeConnected(const std::string& src, const std::string& dst, const std::string& callerBw) {
        connected = true;
        if (arq()) sendLine("NEWSTATE ISS"), sendLine("CONNECTED " + dst + " 500");
        else sendLine("CONNECTED " + src + " " + dst + (type == ModemType::VaraFM ? "" : " " + callerBw));
        sendLine("PTT ON"); sendLine("BUSY ON");   // switched off again at the first data
        if (!arq()) sendLine("IAMALIVE");
        if (type == ModemType::Mercury) { sendLine("SN 8.2"); sendLine("BITRATE (3) 1200 BPS"); }
        if (arq()) {   // an ID frame before the data: not session data
            const char* id = "LY0GW:[KO24]";
            std::vector<uint8_t> f = {0, (uint8_t)(3 + strlen(id)), 'I', 'D', 'F'};
            f.insert(f.end(), id, id + strlen(id));
            sockSendAll(ds, f.data(), f.size());
        }
        if (!asCaller) {
            gw.begin([this](const uint8_t* d, size_t n) { sendData(d, n); });
            gw.s->start();
        }
    }

    void onCmd(const std::string& line) {
        record(line);
        std::vector<std::string> w;
        { size_t i = 0; while (i < line.size()) { size_t e = line.find(' ', i); if (e == std::string::npos) e = line.size(); if (e > i) w.push_back(line.substr(i, e - i)); i = e + 1; } }
        if (w.empty()) return;
        const std::string k = w[0];
        if (arq()) {
            if (k == "VERSION") { if (versionReply) sendLine("VERSION ardopcf_mock-1.0"); }
            else if (k == "MYCALL") { calledMy = w.size() > 1 ? w[1] : ""; sendLine("MYCALL now " + calledMy); }
            else if (k == "LISTEN") { listening = w.size() > 1 && w[1] == "TRUE"; sendLine("LISTEN now " + (w.size() > 1 ? w[1] : std::string())); }
            else if (k == "ARQCALL") {
                calledPeer = w.size() > 1 ? w[1] : "";
                sendLine(line);
                if (callOutcome == "ok") becomeConnected(calledMy, calledPeer, "");
                else if (callOutcome == "disc") sendLine("STATUS CONNECT TO " + calledPeer + " FAILED!"), sendLine("DISCONNECTED");
            } else if (k == "DISCONNECT") { if (connected) { connected = false; sendLine("DISCONNECTED"); } }
            else if (k == "ABORT") { connected = false; sendLine("NEWSTATE DISC"); }
            else if (k == "INITIALIZE") {}
            else sendLine(k + " now " + (w.size() > 1 ? w[1] : std::string()));
            return;
        }
        // VARA / Mercury: OK or WRONG for each command, in order
        static const char* known[] = {"MYCALL", "LISTEN", "COMPRESSION", "WINLINK", "P2P", "CHAT", "CONNECT", "DISCONNECT", "ABORT", "PUBLIC"};
        bool ok = false;
        for (const char* n : known) if (k == n) ok = true;
        if (k.rfind("BW", 0) == 0 && type != ModemType::VaraFM) ok = true;   // VARA FM has no BW command
        if (type == ModemType::VaraFM && (k == "WINLINK" || k == "P2P")) ok = false;   // HF only
        if (k == "VERSION") {
            if (versionReply) sendLine(type == ModemType::Mercury ? "VERSION Mercury mock 1.9" : "VERSION VARA mock 4.8");
            else sendLine("WRONG");
            return;
        }
        if (k == "CONNECT") {
            calledPeer = w.size() > 2 ? w[2] : "";
            if (callOutcome == "wrong") { sendLine("WRONG"); return; }
            sendLine("OK");
            if (callOutcome == "ok") becomeConnected(w.size() > 1 ? w[1] : "", calledPeer, "2300");
            else if (callOutcome == "disc") sendLine("DISCONNECTED");
            return;
        }
        if (k == "LISTEN" && w.size() > 1) listening = w[1] == "ON";
        if (k == "MYCALL") calledMy = w.size() > 1 ? w[1] : "";
        sendLine(ok ? "OK" : "WRONG");
        if ((k == "DISCONNECT" || k == "ABORT") && connected) { connected = false; sendLine("DISCONNECTED"); }
    }

    void onData(const uint8_t* d, size_t n) {
        std::vector<uint8_t> payload;
        if (arq()) {
            frameBuf.insert(frameBuf.end(), d, d + n);
            while (frameBuf.size() >= 2) {
                const size_t len = ((size_t)frameBuf[0] << 8) | frameBuf[1];
                if (frameBuf.size() < 2 + len) break;
                payload.insert(payload.end(), frameBuf.begin() + 2, frameBuf.begin() + 2 + (long)len);
                frameBuf.erase(frameBuf.begin(), frameBuf.begin() + 2 + (long)len);
            }
        } else payload.assign(d, d + n);
        if (payload.empty()) return;
        { std::lock_guard<std::mutex> lk(mu); gotData.insert(gotData.end(), payload.begin(), payload.end()); }
        if (!firstData) { firstData = true; sendLine("PTT OFF"); sendLine("BUSY OFF"); }
        sendLine("BUFFER " + std::to_string(payload.size()));   // stays until the next data or DISCONNECTED
        if (gw.s) gw.s->onBytes(payload.data(), payload.size());
        if (peerClient) peerClient->onBytes(payload.data(), payload.size());
    }

    void startPeerCall() {   // this mock is the caller of a P2P session: our station was listening
        asCaller = true;
        B2fSession::Config c;
        c.role = B2fSession::Role::Client;
        c.mycall = inboundFrom; c.targetCall = kMyCall;
        B2fSession::Hooks h;
        h.send = [this](const uint8_t* d, size_t n) { sendData(d, n); };
        h.outbound = [this]() {
            std::vector<B2Proposal> v;
            for (auto& m : peerOutbox) v.push_back(makeProposal(m));
            return v;
        };
        h.received = [this](const B2Message& m, const B2Proposal&) { std::lock_guard<std::mutex> lk(mu); peerGot.push_back(m); };
        h.sent = [this](const std::string& mid, bool) { std::lock_guard<std::mutex> lk(mu); peerOutbox.erase(std::remove_if(peerOutbox.begin(), peerOutbox.end(), [&](const B2Message& m) { return m.mid() == mid; }), peerOutbox.end()); };
        peerClient = std::make_unique<B2fSession>(c, h);
        connected = true;
        if (arq()) sendLine("CONNECTED " + inboundFrom + " 500");
        else sendLine("CONNECTED " + inboundFrom + " " + calledMy + (type == ModemType::VaraFM ? "" : " 2300"));
        peerClient->start();
    }

    void run() {
        auto acceptOne = [&](sock_t l, sock_t& out) {
            while (!stop) {
                fd_set rd; FD_ZERO(&rd); FD_SET(l, &rd);
                timeval tv{0, 50000};
                if (select((int)l + 1, &rd, nullptr, nullptr, &tv) > 0) { out = accept(l, nullptr, nullptr); return; }
            }
        };
        acceptOne(lc, cs);
        if (stop || cs == kBadSock) return;
        char buf[4096];
        while (!stop) {
            // the data port is accepted whenever the client opens it (the Test connection opens it and closes it again)
            fd_set rd; FD_ZERO(&rd); FD_SET(cs, &rd);
            int mx = (int)cs;
            if (ds == kBadSock) { FD_SET(ld, &rd); mx = std::max(mx, (int)ld); }
            else if (!dataGone) { FD_SET(ds, &rd); mx = std::max(mx, (int)ds); }
            timeval tv{0, 30000};
            const int n = select(mx + 1, &rd, nullptr, nullptr, &tv);
            if (injectCall && !connected && ds != kBadSock) { injectCall = false; startPeerCall(); }
            if (n <= 0) continue;
            if (FD_ISSET(cs, &rd)) {
                const long long k = sockRecv(cs, buf, sizeof buf);
                if (k <= 0) break;
                lineBuf.append(buf, (size_t)k);
                size_t p;
                while ((p = lineBuf.find('\r')) != std::string::npos) { const std::string l = lineBuf.substr(0, p); lineBuf.erase(0, p + 1); if (!l.empty()) onCmd(l); }
            }
            if (ds == kBadSock) { if (FD_ISSET(ld, &rd)) ds = accept(ld, nullptr, nullptr); continue; }
            if (!dataGone && FD_ISSET(ds, &rd)) {
                const long long k = sockRecv(ds, buf, sizeof buf);
                if (k <= 0) { dataGone = true; continue; }   // a client may close the data port alone
                onData((const uint8_t*)buf, (size_t)k);
                if (dropAfterBytes && gotData.size() >= dropAfterBytes) { sockShutdown(cs); sockShutdown(ds); break; }
            }
        }
    }

    bool begin(ModemType t) {
        type = t;
        lc = mkListen(cmdPort); ld = mkListen(dataPort);
        if (lc == kBadSock || ld == kBadSock) return false;
        th = std::thread([this] { run(); });
        return true;
    }
    ModemProfile profile() const {
        ModemProfile p = defaultModemProfile(type);
        p.host = "127.0.0.1"; p.cmdPort = cmdPort; p.dataPort = dataPort; p.tcpAttempts = 1;
        return p;
    }
    ~MockModem() {
        stop = true;
        if (th.joinable()) th.join();
        for (sock_t s : {cs, ds, lc, ld}) if (s != kBadSock) sockClose(s);
    }
};

const std::time_t t0 = 1700000000;

// the mock reads commands on its own thread: give it a moment after the station has finished
bool sawSoon(MockModem& m, const std::string& prefix) {
    for (int i = 0; i < 200; i++) { if (m.saw(prefix)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return false;
}

struct Outcome {
    bool ok = false, ended = false;
    int rx = 0, tx = 0;
    std::string err;
    bool sawBuffer = false, sawSnr = false, sawBusy = false, sawPtt = false;
    std::string lastDetail, lastStatus;
    std::vector<std::string> log;
};

// run a station until it ends (or the real-time limit); simClock: advance the station clock faster than real time
Outcome drive(WinlinkStation& st, double secLimit, double simStep = 0.005, const std::function<void(double)>& each = nullptr) {
    Outcome o;
    const auto t = std::chrono::steady_clock::now();
    double now = 0;
    while (st.running() && std::chrono::steady_clock::now() - t < std::chrono::seconds((long)secLimit)) {
        st.tick(now);
        if (const auto* l = dynamic_cast<const ExtModemLink*>(st.link())) {
            o.sawBuffer |= l->bufferBytes() > 0;
            o.sawSnr |= l->snrKnown();
            o.sawBusy |= l->busy();
            o.sawPtt |= l->ptt();
            o.lastDetail = st.status();
        }
        for (auto& s : st.takeLog()) o.log.push_back(s);
        if (each) each(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        now += simStep;
    }
    o.ended = !st.running();
    if (st.running()) { st.stop(now); st.stop(now); }
    for (auto& s : st.takeLog()) o.log.push_back(s);
    o.ok = st.lastOk(); o.err = st.lastError(); o.rx = st.received(); o.tx = st.sent();
    return o;
}



StationConfig baseCfg(const MockModem& m, const std::string& gwCall = kGwCall) {
    StationConfig sc;
    sc.mycall = kMyCall; sc.password = kPassword; sc.locator = "KO24";
    sc.gateway.call = gwCall;
    sc.gateway.transport = Transport::Modem;
    sc.gateway.modem = m.profile().name;
    sc.modem = m.profile();
    return sc;
}

// a whole session through each modem type: B2F proposals both ways over the data port
void testSessions() {
    struct Case { ModemType t; const char* bw; const char* via; };
    const Case cases[] = {{ModemType::Ardop, "500MAX", ""}, {ModemType::VaraHF, "500", ""}, {ModemType::VaraFM, "", "DIGI1,DIGI2"}, {ModemType::Mercury, "2300", ""}};
    for (const Case& c : cases) {
        MockModem m;
        CHECK(m.begin(c.t), "mock %s listens", modemTypeName(c.t));
        m.gw.waiting.push_back(msg("LY1GW", kMyCall, "over the modem", "Hello through the data port\r\n" + std::string(1500, 'm'), t0));
        m.gw.waiting.push_back(msg("LY1GW", kMyCall, "second", "again", t0 + 1));
        const std::string dir = tempDir("extmodem");
        Mailbox mb(dir);
        mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "outgoing", std::string(900, 'o'), t0 + 2));
        WinlinkStation st(mb);
        StationConfig sc = baseCfg(m);
        sc.modem.bw = c.bw;
        sc.gateway.via = c.via;
        std::string err;
        CHECK(st.start(sc, 0, err), "%s start: %s", modemTypeName(c.t), err.c_str());
        const Outcome o = drive(st, 30);
        printf("%-8s ok=%d rx=%d tx=%d buffer=%d snr=%d busy=%d ptt=%d | %s\n", modemTypeName(c.t), o.ok, o.rx, o.tx, o.sawBuffer, o.sawSnr, o.sawBusy, o.sawPtt, o.lastDetail.c_str());
        CHECK(o.ok && o.rx == 2 && o.tx == 1, "%s session: ok %d rx %d tx %d err %s", modemTypeName(c.t), o.ok, o.rx, o.tx, o.err.c_str());
        CHECK(m.gw.got.size() == 1 && m.gw.got[0].subject() == "outgoing", "%s: the simulated gateway got the message", modemTypeName(c.t));
        CHECK(mb.list(Folder::Inbox).size() == 2 && mb.list(Folder::Sent).size() == 1 && mb.list(Folder::Outbox).empty(), "%s: mailbox folders", modemTypeName(c.t));
        CHECK(o.sawBuffer && o.sawBusy && o.sawPtt, "%s: buffer / busy / PTT status reached the link", modemTypeName(c.t));
        if (c.t == ModemType::Mercury) CHECK(o.sawSnr, "Mercury S/N reported");
        // the commands, in the documented form and order
        const auto all = m.all();
        std::string joined;
        for (auto& a : all) joined += a + " | ";
        printf("   commands: %s\n", joined.c_str());
        if (c.t == ModemType::Ardop) {
            CHECK(m.index("INITIALIZE") == 0, "ARDOP: INITIALIZE first");
            CHECK(m.saw(std::string("MYCALL ") + kMyCall) && m.saw("PROTOCOLMODE ARQ") && m.saw("ARQBW 500MAX") && m.saw("LISTEN FALSE"), "ARDOP: setup commands");
            CHECK(m.index("ARQCALL " + std::string(kGwCall) + " 5") > m.index("LISTEN FALSE"), "ARDOP: ARQCALL after the setup");
            CHECK(m.saw("DISCONNECT"), "ARDOP: DISCONNECT at the end");
        } else {
            CHECK(m.index(std::string("MYCALL ") + kMyCall) == 0, "%s: MYCALL first", modemTypeName(c.t));
            CHECK(m.saw("LISTEN OFF") && m.saw("DISCONNECT"), "%s: LISTEN OFF and DISCONNECT", modemTypeName(c.t));
            if (c.t == ModemType::VaraHF) CHECK(m.saw("WINLINK SESSION") && m.saw("BW500") && m.saw("COMPRESSION TEXT") && !m.saw("P2P"), "VARA HF: session type, BW500, compression");
            if (c.t == ModemType::VaraFM) CHECK(m.saw(std::string("CONNECT ") + kMyCall + " " + kGwCall + " via DIGI1 DIGI2") && !m.saw("BW") && !m.saw("WINLINK"), "VARA FM: digipeaters in CONNECT, no BW, no WINLINK SESSION");
            if (c.t == ModemType::Mercury) CHECK(m.saw("BW2300") && !m.saw("COMPRESSION"), "Mercury: BW2300, no COMPRESSION");
            if (c.t != ModemType::VaraFM) CHECK(m.saw(std::string("CONNECT ") + kMyCall + " " + kGwCall), "%s: CONNECT <mycall> <gateway>", modemTypeName(c.t));
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

// the answering side: listen, a station calls in, messages go both ways
void testListen() {
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::Mercury}) {
        MockModem m;
        CHECK(m.begin(t), "mock listens");
        m.inboundFrom = "LY0PEER";
        m.peerOutbox.push_back(msg("LY0PEER", kMyCall, "hello from the peer", "p2p body", t0));
        const std::string dir = tempDir("extmodem-p2p");
        Mailbox mb(dir);
        mb.save(Folder::Outbox, msg(kMyCall, "LY0PEER", "for the peer", "reply", t0 + 1));
        mb.save(Folder::Outbox, msg(kMyCall, "other@example.com", "not for the peer", "stay", t0 + 2));
        WinlinkStation st(mb);
        StationConfig sc = baseCfg(m, "");
        std::string err;
        CHECK(st.listen(sc, 0, err), "listen: %s", err.c_str());
        bool injected = false, sawListening = false;
        Outcome o = drive(st, 20, 0.005, [&](double) {
            if (st.link() && st.link()->state() == LinkState::Listening) sawListening = true;
            if (!injected && sawListening && (m.saw("LISTEN ON") || m.saw("LISTEN TRUE"))) { injected = true; m.injectCall = true; }
            if (injected && mb.list(Folder::Inbox).size() == 1 && mb.list(Folder::Sent).size() == 1 && m.peerOutbox.empty()) st.stop(0);
        });
        st.stop(0); st.stop(0);
        std::lock_guard<std::mutex> lk(m.mu);
        printf("%-8s listen: injected=%d inbox=%zu sent=%zu peerGot=%zu\n", modemTypeName(t), injected, mb.list(Folder::Inbox).size(), mb.list(Folder::Sent).size(), m.peerGot.size());
        CHECK(sawListening && injected, "%s: link listened and the call was injected", modemTypeName(t));
        CHECK(mb.list(Folder::Inbox).size() == 1 && mb.list(Folder::Sent).size() == 1, "%s: inbox / sent after the peer-to-peer session", modemTypeName(t));
        CHECK(m.peerGot.size() == 1 && m.peerGot[0].subject() == "for the peer", "%s: the peer received only what was addressed to it", modemTypeName(t));
        CHECK(mb.list(Folder::Outbox).size() == 1, "%s: the other message stays in the outbox", modemTypeName(t));
        (void)o;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

void testNotRunning() {
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::Mercury}) {
        ModemProfile p = defaultModemProfile(t);
        p.cmdPort = MockModem::freePort(); p.dataPort = MockModem::freePort(); p.tcpAttempts = 1;
        const std::string dir = tempDir("extmodem-down");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        StationConfig sc;
        sc.mycall = kMyCall;
        sc.gateway.call = kGwCall; sc.gateway.transport = Transport::Modem;
        sc.modem = p;
        std::string err;
        CHECK(st.start(sc, 0, err), "start with no modem");
        Outcome o = drive(st, 10);
        printf("%-8s not running: %s\n", modemTypeName(t), o.err.c_str());
        CHECK(o.ended && !o.ok && o.err.find("is the modem program running") != std::string::npos && o.err.find("command port") != std::string::npos, "%s: clear message when the modem is not running", modemTypeName(t));
        // the test connection says so too
        const ModemTestResult r = testModemProfile(p, 800);
        CHECK(!r.cmdOpen && r.message.find("cannot open the command port") != std::string::npos, "%s: Test connection, closed port: %s", modemTypeName(t), r.message.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    // station refuses a profile with no host
    const std::string dir = tempDir("extmodem-nohost");
    Mailbox mb(dir);
    WinlinkStation st(mb);
    StationConfig sc;
    sc.mycall = kMyCall; sc.gateway.call = kGwCall; sc.gateway.transport = Transport::Modem; sc.modem.host.clear();
    std::string err;
    CHECK(!st.start(sc, 0, err) && !err.empty(), "no profile: refused (%s)", err.c_str());
    // the data port closed while the command port is open
    MockModem m;
    m.begin(ModemType::VaraHF);
    ModemProfile p = m.profile();
    p.dataPort = MockModem::freePort();
    WinlinkStation st2(mb);
    sc.modem = p;
    CHECK(st2.start(sc, 0, err), "start with the data port closed");
    Outcome o = drive(st2, 10);
    CHECK(o.ended && o.err.find("data port") != std::string::npos, "data port closed: %s", o.err.c_str());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// the modem goes away in the middle of a session: the link fails, the outbox keeps the message, the user sees why
void testDropMid() {
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::Mercury}) {
        MockModem m;
        m.begin(t);
        m.dropAfterBytes = 300;
        std::mt19937 rg(7);
        auto rnd = [&](size_t n) { std::string r; for (size_t i = 0; i < n; i++) r += (char)('a' + rg() % 26); return r; };   // does not compress
        m.gw.waiting.push_back(msg("LY1GW", kMyCall, "big", rnd(3000), t0));
        const std::string dir = tempDir("extmodem-drop");
        Mailbox mb(dir);
        mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "outgoing", rnd(4000), t0 + 2));
        WinlinkStation st(mb);
        std::string err;
        CHECK(st.start(baseCfg(m), 0, err), "start");
        const Outcome o = drive(st, 15);
        printf("%-8s drop: ended=%d ok=%d err=%s\n", modemTypeName(t), o.ended, o.ok, o.err.c_str());
        CHECK(o.ended && !o.ok && !o.err.empty(), "%s: a lost modem ends the session with a reason", modemTypeName(t));
        CHECK(mb.list(Folder::Outbox).size() == 1, "%s: the unsent message stays in the outbox", modemTypeName(t));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    // the modem says DISCONNECTED while a session runs (the other station hung up)
    MockModem m;
    m.begin(ModemType::VaraHF);
    m.gw.waiting.push_back(msg("LY1GW", kMyCall, "x", "y", t0));
    const std::string dir = tempDir("extmodem-hup");
    Mailbox mb(dir);
    mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "outgoing", "z", t0 + 2));
    WinlinkStation st(mb);
    std::string err;
    CHECK(st.start(baseCfg(m), 0, err), "start");
    bool hung = false;
    const Outcome o = drive(st, 15, 0.005, [&](double) { if (!hung && m.saw("CONNECT")) { hung = true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); m.sendLine("DISCONNECTED"); } });
    CHECK(hung && o.ended, "DISCONNECTED from the modem ends the link");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// a call nobody answers, a rejected call, a call that fails at once, the user pressing Stop
void testCallOutcomes() {
    // nobody answers: the clock runs faster than real time so the 3 s timeout passes quickly
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::Mercury}) {
        MockModem m;
        m.begin(t);
        m.callOutcome = "never";
        const std::string dir = tempDir("extmodem-to");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        StationConfig sc = baseCfg(m);
        sc.modem.connectTimeoutSec = 3;
        std::string err;
        CHECK(st.start(sc, 0, err), "start");
        const Outcome o = drive(st, 15, 0.25);
        printf("%-8s timeout: %s\n", modemTypeName(t), o.err.c_str());
        CHECK(o.ended && !o.ok && o.err.find("no answer") != std::string::npos, "%s: call timeout reported: %s", modemTypeName(t), o.err.c_str());
        CHECK(sawSoon(m, t == ModemType::Ardop ? "ABORT" : "DISCONNECT"), "%s: the modem is told to give up", modemTypeName(t));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    {   // VARA / Mercury answer WRONG to CONNECT
        MockModem m;
        m.begin(ModemType::VaraHF);
        m.callOutcome = "wrong";
        const std::string dir = tempDir("extmodem-wr");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        std::string err;
        CHECK(st.start(baseCfg(m), 0, err), "start");
        const Outcome o = drive(st, 10);
        CHECK(o.ended && !o.ok && o.err.find("rejected") != std::string::npos && o.err.find("CONNECT") != std::string::npos, "WRONG to CONNECT: %s", o.err.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    for (ModemType t : {ModemType::Ardop, ModemType::Mercury}) {   // DISCONNECTED while calling (ARDOP: STATUS ... FAILED!)
        MockModem m;
        m.begin(t);
        m.callOutcome = "disc";
        const std::string dir = tempDir("extmodem-fail");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        std::string err;
        CHECK(st.start(baseCfg(m), 0, err), "start");
        const Outcome o = drive(st, 10);
        CHECK(o.ended && !o.ok && !o.err.empty(), "%s: a failed call is reported: %s", modemTypeName(t), o.err.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    {   // Stop while the call is out
        MockModem m;
        m.begin(ModemType::VaraHF);
        m.callOutcome = "never";
        const std::string dir = tempDir("extmodem-stop");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        std::string err;
        CHECK(st.start(baseCfg(m), 0, err), "start");
        double now = 0;
        const auto t = std::chrono::steady_clock::now();
        while (!m.saw("CONNECT") && std::chrono::steady_clock::now() - t < std::chrono::seconds(5)) { st.tick(now); std::this_thread::sleep_for(std::chrono::milliseconds(5)); now += 0.005; }
        CHECK(sawSoon(m, "CONNECT"), "the call went out");
        st.stop(now);
        const auto t2 = std::chrono::steady_clock::now();
        while (st.running() && std::chrono::steady_clock::now() - t2 < std::chrono::seconds(5)) { st.tick(now); std::this_thread::sleep_for(std::chrono::milliseconds(5)); now += 0.005; }
        if (st.running()) st.stop(now);
        CHECK(!st.running() && sawSoon(m, "DISCONNECT"), "Stop while calling sends DISCONNECT and ends");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

void testConnectionTest() {
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::VaraFM, ModemType::Mercury}) {
        MockModem m;
        m.begin(t);
        const ModemTestResult r = testModemProfile(m.profile(), 2000);
        printf("%-8s test: %s\n", modemTypeName(t), r.message.c_str());
        CHECK(r.cmdOpen && r.dataOpen && r.answered, "%s: ports open and the modem answered (%d %d %d, %zu commands): %s", modemTypeName(t), r.cmdOpen, r.dataOpen, r.answered, m.all().size(), r.message.c_str());
        if (t != ModemType::VaraHF) CHECK(r.version.find("mock") != std::string::npos, "%s: version read: %s", modemTypeName(t), r.version.c_str());
        // nothing but VERSION was sent: no INITIALIZE, no MYCALL, no CONNECT, no PTT-relevant command
        const auto all = m.all();
        CHECK(all.size() == 1 && all[0] == "VERSION", "%s: Test connection sent only VERSION (%zu commands)", modemTypeName(t), all.size());
    }
    MockModem m;
    m.begin(ModemType::Ardop);
    m.versionReply = false;
    const ModemTestResult r = testModemProfile(m.profile(), 600);
    CHECK(r.cmdOpen && r.dataOpen && !r.answered && r.message.find("did not answer") != std::string::npos, "silent modem: %s", r.message.c_str());
}

void testProfiles() {
    std::vector<ModemProfile> v;
    ModemProfile a = defaultModemProfile(ModemType::Ardop); a.name = "ardopcf shack"; a.bw = "500MAX"; a.audioNote = "USB codec";
    ModemProfile b = defaultModemProfile(ModemType::VaraHF); b.name = "VARA (Wine)"; b.host = "192.168.1.20"; b.bw = "2750"; b.connectTimeoutSec = 200;
    ModemProfile c = defaultModemProfile(ModemType::Mercury); c.name = "Mercury|odd\nname";
    v = {a, b, c};
    const std::vector<ModemProfile> w = loadModemProfiles(saveModemProfiles(v));
    CHECK(w.size() == 3 && w[0].name == "ardopcf shack" && w[0].cmdPort == 8515 && w[0].dataPort == 8516 && w[0].bw == "500MAX" && w[0].audioNote == "USB codec", "profile 1 round trip");
    CHECK(w[1].type == ModemType::VaraHF && w[1].host == "192.168.1.20" && w[1].cmdPort == 8300 && w[1].dataPort == 8301 && w[1].bw == "2750" && w[1].connectTimeoutSec == 200, "profile 2 round trip");
    CHECK(w[2].type == ModemType::Mercury && w[2].name.find('|') == std::string::npos && w[2].name.find('\n') == std::string::npos, "separators in a name are replaced");
    CHECK(loadModemProfiles("junk\n|1\nx|9|h|0|99999\n").size() == 1, "bad lines skipped, type and ports clamped");
    // gateways keep the modem name and the old format still loads
    Gateway g; g.call = "LY0GW-10"; g.transport = Transport::Modem; g.modem = "VARA (Wine)"; g.peer = true; g.via = "DIGI1";
    const std::vector<Gateway> gw = loadGateways(saveGateways({g}));
    CHECK(gw.size() == 1 && gw[0].transport == Transport::Modem && gw[0].modem == "VARA (Wine)" && gw[0].peer && gw[0].via == "DIGI1", "gateway keeps the modem and peer flag");
    const std::vector<Gateway> old = loadGateways("LY2XYZ-10|0|144.8|DIGI|||note|1\n");
    CHECK(old.size() == 1 && old[0].peer && old[0].modem.empty() && old[0].transport == Transport::Packet1200, "the old gateway line still loads");
    // BW rules
    CHECK(modemBwValid(ModemType::Ardop, "500MAX") && modemBwValid(ModemType::Ardop, "1000force") && !modemBwValid(ModemType::Ardop, "500X") && modemBwValid(ModemType::Ardop, ""), "ARDOP BW forms");
    CHECK(modemBwValid(ModemType::VaraHF, "2300") && !modemBwValid(ModemType::VaraHF, "500\rPTT") && !modemBwValid(ModemType::VaraFM, "500"), "VARA BW forms");
    CHECK(modemBwCommand(ModemType::Ardop, "500max") == "ARQBW 500MAX" && modemBwCommand(ModemType::VaraHF, "2750") == "BW2750" && modemBwCommand(ModemType::VaraFM, "").empty(), "BW commands");
    CHECK(modemBwChoices(ModemType::VaraFM).empty() && modemBwChoices(ModemType::Mercury).size() == 3 && modemBwChoices(ModemType::Ardop).size() == 8, "BW choices");
    for (ModemType t : {ModemType::Ardop, ModemType::VaraHF, ModemType::VaraFM, ModemType::Mercury}) {
        const ModemHelp h = modemHelp(t);
        CHECK(!h.steps.empty() && h.installUrl.rfind("https://", 0) == 0 && !h.licence.empty() && !h.source.empty(), "help text for %s", modemTypeName(t));
        CHECK(h.steps.find("does not include") != std::string::npos, "%s help says OnAir does not bundle the modem", modemTypeName(t));
    }
}

// nothing a user types may add a second command
void testInjection() {
    MockModem m;
    m.begin(ModemType::VaraHF);
    const std::string dir = tempDir("extmodem-inj");
    Mailbox mb(dir);
    struct Bad { const char* what; std::string my, peer, bw, via; };
    const Bad bads[] = {{"call with CR", "LY2TST\rABORT", kGwCall, "", ""}, {"peer with space", kMyCall, "LY0 GW", "", ""}, {"empty peer", kMyCall, "", "", ""},
                        {"bw with CR", kMyCall, kGwCall, "500\rPTT ON", ""}, {"via with CR", kMyCall, kGwCall, "", "A\rB"}};
    for (const Bad& b : bads) {
        WinlinkStation st(mb);
        StationConfig sc = baseCfg(m);
        sc.mycall = b.my; sc.gateway.call = b.peer; sc.modem.bw = b.bw; sc.gateway.via = b.via;
        std::string err;
        const bool started = st.start(sc, 0, err);
        bool failed = !started;
        if (started) { const Outcome o = drive(st, 5); failed = o.ended && !o.ok; }
        CHECK(failed, "%s is refused", b.what);
    }
    CHECK(!m.saw("ABORT") && !m.saw("PTT") && m.all().empty(), "no command from a refused setting reached the modem (%zu)", m.all().size());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    netInit();
    testProfiles();
    testConnectionTest();
    testNotRunning();
    testInjection();
    testCallOutcomes();
    testSessions();
    testListen();
    testDropMid();
    return testSummary("winlink extmodem");
}
