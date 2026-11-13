// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EchoLink conference hosting and the relay / proxy connection.
//  - a conference on a fake clock and a simulated network: stations joining, the station list in the info text, the station limit, two
//    people talking at once (each hears the other, nobody hears himself), the host talking and listening, chat passed on, leaving and
//    timing out, two stations behind one NAT address, and packet loss, duplicates and reordering;
//  - a conference of real clients over UDP on 127.0.0.1;
//  - the proxy protocol bytes, and a client behind a mock proxy on 127.0.0.1 that logs in to a mock directory, lists, and holds a QSO with a
//    direct client, and a wrong proxy password.
// Nothing here contacts the real EchoLink servers or a real proxy: every socket is on 127.0.0.1.
#include "dect2/echolink/el_client.h"
#include "dect2/echolink/el_conference.h"
#include "dect2/echolink/el_proto.h"
#include "dect2/echolink/el_proxy.h"
#include "dect2/echolink/gsm610.h"
#include "dect2/winlink/b2msg.h"
#include "../../core/src/net/tcp_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace dect2::echolink;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F> static bool waitFor(F&& f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; sleepMs(10); }
    return f();
}
static double toneAmp(const std::vector<int16_t>& x, size_t from, size_t n, double f) {
    if (x.size() < from + n) return 0;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) { re += x[from + i] * std::cos(2 * M_PI * f * (double)i / 8000); im += x[from + i] * std::sin(2 * M_PI * f * (double)i / 8000); }
    return 2 * std::sqrt(re * re + im * im) / (double)n;
}

// ---- a conference on a simulated network ---------------------------------------------------------------------------------------------
struct Pkt { std::string fromIp; int fromPort; std::string toIp; int toPort; bool ctrl; std::vector<uint8_t> b; int64_t at; };

struct Net {
    std::vector<Pkt> q;
    std::mt19937 rng{7};
    double loss = 0, dup = 0, reorder = 0;
    int64_t now = 0;
    void send(const std::string& fromIp, int fromPort, const std::string& toIp, int toPort, bool ctrl, const std::vector<uint8_t>& b) {
        std::uniform_real_distribution<double> u(0, 1);
        if (!ctrl && u(rng) < loss) return;   // audio is lost; the keep-alives are not, to keep the test about the mixer
        int64_t delay = 5;
        if (u(rng) < reorder) delay += 90;     // arrives after the next packet
        q.push_back({fromIp, fromPort, toIp, toPort, ctrl, b, now + delay});
        if (u(rng) < dup) q.push_back({fromIp, fromPort, toIp, toPort, ctrl, b, now + delay + 3});
    }
};

struct Peer {
    std::string ip;
    int audio;
    Session s;
    GsmEncoder enc;
    GsmDecoder dec;
    std::vector<int16_t> heard;
    std::vector<std::string> chat;
    double toneHz = 0;
    double phase = 0;
    bool talk = false, alive = true;
};

struct Rig {
    Net net;
    Conference conf;
    std::vector<std::unique_ptr<Peer>> peers;
    const std::string hostIp = "10.0.0.1";
    std::vector<int16_t> hostHeard;
    std::vector<std::string> events;
    Rig(int maxStations) {
        conf.setLocal("*OATEST*", "OnAir test", "OnAir test conference");
        conf.setMaxStations(maxStations);
        conf.setSend([this](bool ctrl, const std::string& ip, int port, const std::vector<uint8_t>& b) {
            net.send(hostIp, ctrl ? kDefaultAudioPort + 1 : kDefaultAudioPort, ip, port, ctrl, b);
        });
    }
    Peer& add(const std::string& call, const std::string& ip, int audioPort) {
        std::unique_ptr<Peer> p(new Peer);
        p->ip = ip; p->audio = audioPort;
        p->s.setLocal(call, "Op " + call, "station " + call);
        Peer* pp = p.get();
        p->s.setSend([this, pp](bool ctrl, const std::string& ip2, int port, const std::vector<uint8_t>& b) {
            net.send(pp->ip, ctrl ? pp->audio + 1 : pp->audio, ip2, port, ctrl, b);
        });
        peers.push_back(std::move(p));
        return *peers.back();
    }
    void step() {   // 10 ms
        net.now += 10;
        // talkers: one RTP packet per 80 ms
        if (net.now % kMixBlockMs == 0) {
            for (auto& p : peers) {
                if (!p->talk || !p->alive || p->s.state() != QsoState::Connected) continue;
                uint8_t fr[kFramesPerPacket][kGsmFrameBytes];
                for (int f = 0; f < kFramesPerPacket; f++) {
                    int16_t pcm[kGsmFrameSamples];
                    for (int i = 0; i < kGsmFrameSamples; i++) { pcm[i] = (int16_t)(6000 * std::sin(p->phase)); p->phase += 2 * M_PI * p->toneHz / 8000; }
                    p->enc.encode(pcm, fr[f]);
                }
                p->s.sendAudio(fr);
            }
        }
        // deliver
        std::vector<Pkt> due, later;
        for (Pkt& k : net.q) (k.at <= net.now ? due : later).push_back(std::move(k));
        net.q.swap(later);
        for (Pkt& k : due) {
            if (k.toIp == hostIp) {
                if (k.ctrl) conf.onCtrl(k.fromIp, k.fromPort, k.b.data(), k.b.size(), net.now);
                else conf.onAudio(k.fromIp, k.fromPort, k.b.data(), k.b.size(), net.now);
                continue;
            }
            for (auto& p : peers) {
                if (!p->alive || p->ip != k.toIp || (k.ctrl ? p->audio + 1 : p->audio) != k.toPort) continue;
                if (k.ctrl) p->s.onCtrl(k.fromIp, k.fromPort, k.b.data(), k.b.size(), net.now);
                else p->s.onAudio(k.fromIp, k.fromPort, k.b.data(), k.b.size(), net.now);
            }
        }
        conf.tick(net.now);
        for (auto& p : peers) {
            if (!p->alive) continue;
            p->s.tick(net.now);
            for (const RxPacket& r : p->s.takeAudio())
                for (int f = 0; f < r.nframes; f++) { int16_t pcm[kGsmFrameSamples]; if (p->dec.decode(r.frames[f], pcm)) p->heard.insert(p->heard.end(), pcm, pcm + kGsmFrameSamples); }
            for (std::string& c : p->s.takeChat()) p->chat.push_back(c);
            p->s.takeEvents();
        }
        int16_t b[kMixBlockSamples];
        for (int n; (n = conf.pullLocal(b, kMixBlockSamples)) > 0;) hostHeard.insert(hostHeard.end(), b, b + n);
        for (std::string& e : conf.takeEvents()) events.push_back(e);
    }
    void run(int ms) { for (int t = 0; t < ms; t += 10) step(); }
};

static void testConferenceSim() {
    Rig r(5);
    const char* calls[] = {"LY1AA", "LY2BB", "LY3CC", "LY4DD", "LY5EE"};
    for (int i = 0; i < 5; i++) {
        Peer& p = r.add(calls[i], "10.0.0." + std::to_string(10 + i), kDefaultAudioPort);
        CHECK(p.s.connect(r.hostIp, kDefaultAudioPort, r.net.now), "peer %d connect", i);
        r.run(200);
    }
    r.run(500);
    CHECK(r.conf.connectedCount() == 5, "5 stations joined (%d)", r.conf.connectedCount());
    for (auto& p : r.peers) {
        CHECK(p->s.state() == QsoState::Connected && p->s.peerCall() == "*OATEST*", "peer connected to the conference ('%s')", p->s.peerCall().c_str());
        CHECK(p->s.peerInfo().find("Connected stations (5):") != std::string::npos, "station list in the info text: '%s'", p->s.peerInfo().c_str());
        for (const char* c : calls) CHECK(p->s.peerInfo().find(c) != std::string::npos, "%s listed", c);
    }
    // the limit: a sixth is refused with a BYE
    Peer& six = r.add("LY6FF", "10.0.0.20", kDefaultAudioPort);
    six.s.connect(r.hostIp, kDefaultAudioPort, r.net.now);
    r.run(500);
    CHECK(six.s.state() == QsoState::Idle && r.conf.connectedCount() == 5, "the sixth station is refused (full)");
    bool sawFull = false;
    for (const std::string& e : r.events) sawFull = sawFull || e.find("full") != std::string::npos;
    CHECK(sawFull, "the log says the conference is full");
    six.alive = false;

    // two people talk at once: 600 Hz and 1400 Hz
    for (auto& p : r.peers) p->heard.clear();
    r.hostHeard.clear();
    r.peers[0]->toneHz = 600; r.peers[0]->talk = true;
    r.peers[1]->toneHz = 1400; r.peers[1]->talk = true;
    r.run(2000);
    int talking = 0;
    for (const ConferenceMember& m : r.conf.members()) talking += m.talking;
    CHECK(talking == 2, "two stations shown talking (%d)", talking);
    r.peers[0]->talk = r.peers[1]->talk = false;
    r.run(400);
    const size_t from = 4000, n = 8000;
    for (int i = 0; i < 5; i++) {
        const Peer& p = *r.peers[(size_t)i];
        const double a6 = toneAmp(p.heard, from, n, 600), a14 = toneAmp(p.heard, from, n, 1400);
        printf("conference: %s heard 600 Hz %.0f, 1400 Hz %.0f (%zu samples)\n", calls[i], a6, a14, p.heard.size());
        if (i == 0) CHECK(a14 > 3500 && a6 < 300, "the first talker hears the second only");
        else if (i == 1) CHECK(a6 > 3500 && a14 < 300, "the second talker hears the first only");
        else CHECK(a6 > 3500 && a14 > 3500, "listener %d hears both talkers", i);
    }
    CHECK(toneAmp(r.hostHeard, from, n, 600) > 3500 && toneAmp(r.hostHeard, from, n, 1400) > 3500, "the host hears both");

    // the host talks: 1000 Hz to everyone
    for (auto& p : r.peers) p->heard.clear();
    r.conf.setLocalTalk(true);
    std::vector<int16_t> tone(8000 * 2);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (int16_t)(6000 * std::sin(2 * M_PI * 1000.0 * (double)i / 8000));
    for (size_t i = 0; i < tone.size(); i += 80) { r.conf.pushLocal(&tone[i], 80); r.step(); }
    r.conf.setLocalTalk(false);
    r.run(300);
    for (int i = 0; i < 5; i++) CHECK(toneAmp(r.peers[(size_t)i]->heard, 2000, 8000, 1000) > 3500, "station %d hears the host", i);

    // chat from LY4DD reaches the others and the host
    r.peers[3]->s.sendChat("hello all");
    r.run(100);
    CHECK(r.peers[4]->chat.size() == 1 && r.peers[4]->chat[0] == "LY4DD>hello all", "chat passed on: '%s'", r.peers[4]->chat.empty() ? "" : r.peers[4]->chat[0].c_str());
    CHECK(r.peers[3]->chat.empty(), "no echo of one's own chat");
    const std::vector<std::string> hc = r.conf.takeChat();
    CHECK(hc.size() == 1 && hc[0] == "LY4DD>hello all", "the host sees the chat");
    CHECK(r.conf.sendChat("from the host"), "host chat");
    r.run(100);
    CHECK(r.peers[0]->chat.size() == 2 && r.peers[0]->chat[1] == "*OATEST*>from the host", "host chat arrives");

    // LY5EE leaves (BYE), LY4DD goes silent and times out
    r.peers[4]->s.disconnect();
    r.peers[4]->alive = false;
    r.run(200);
    CHECK(r.conf.connectedCount() == 4, "a BYE removes the station (%d)", r.conf.connectedCount());
    CHECK(r.peers[0]->s.peerInfo().find("Connected stations (4):") != std::string::npos && r.peers[0]->s.peerInfo().find("LY5EE") == std::string::npos,
          "the list is sent again: '%s'", r.peers[0]->s.peerInfo().c_str());
    r.peers[3]->alive = false;
    r.run(kConnectTimeoutMs + 1000);
    CHECK(r.conf.connectedCount() == 3, "a silent station times out (%d)", r.conf.connectedCount());
    for (int i = 0; i < 3; i++) CHECK(r.peers[(size_t)i]->s.state() == QsoState::Connected, "the others stay (keep-alives both ways)");

    // the host kicks one
    CHECK(r.conf.disconnect("LY3CC"), "disconnect one station");
    r.run(100);
    CHECK(r.peers[2]->s.state() == QsoState::Idle && r.conf.connectedCount() == 2, "kicked");
    r.conf.disconnectAll();
    r.run(100);
    CHECK(r.peers[0]->s.state() == QsoState::Idle && r.peers[1]->s.state() == QsoState::Idle && r.conf.size() == 0, "all disconnected");
}

// two stations behind one NAT address, and an unreliable network
static void testConferenceNatAndLoss() {
    for (int trial = 0; trial < 3; trial++) {
        Rig r(10);
        r.net.rng.seed(100 + (unsigned)trial);
        r.net.loss = trial == 0 ? 0 : 0.1;
        r.net.dup = trial == 0 ? 0 : 0.05;
        r.net.reorder = trial == 2 ? 0.1 : 0;
        Peer& a = r.add("LY1NAT", "192.0.2.7", 5198);
        Peer& b = r.add("LY2NAT", "192.0.2.7", 6198);   // the same public address, another port
        Peer& c = r.add("LY3LIS", "198.51.100.3", 5198);
        for (auto& p : r.peers) { p->s.connect(r.hostIp, kDefaultAudioPort, r.net.now); r.run(200); }
        r.run(300);
        CHECK(r.conf.connectedCount() == 3, "trial %d: three joined, two behind one address (%d)", trial, r.conf.connectedCount());
        a.toneHz = 700; a.talk = true;
        r.run(3000);
        a.talk = false;
        r.run(400);
        // two hops (talker to host, host to listener) each lose and reorder: about 0.8 x 0.8 of the audio arrives in trial 2
        const double ab = toneAmp(b.heard, 2000, 8000, 700), ac = toneAmp(c.heard, 2000, 8000, 700), aa = toneAmp(a.heard, 0, 800, 700);
        printf("conference trial %d (loss %.0f %%, dup %.0f %%, reorder %.0f %%): B %.0f, C %.0f, A itself %.0f; B got %zu samples\n", trial,
               r.net.loss * 100, r.net.dup * 100, r.net.reorder * 100, ab, ac, aa, b.heard.size());
        const double want = trial == 0 ? 4500 : 2500;   // lost blocks are gaps in the window
        CHECK(ab > want && ac > want, "trial %d: the other station at the same address and the listener hear the talker", trial);
        CHECK(a.heard.size() < 1000, "trial %d: the talker gets nothing back (%zu samples)", trial, a.heard.size());
        uint64_t lost = 0;
        for (const ConferenceMember& m : r.conf.members()) if (m.call == "LY1NAT") lost = m.lost;
        if (trial > 0) CHECK(lost > 0, "trial %d: losses are counted", trial);
    }
}

// ---- real clients over loopback ------------------------------------------------------------------------------------------------------
static int pickBase(int salt) {
#ifdef _WIN32
    const int pid = 4321;
#else
    const int pid = (int)getpid();
#endif
    return 23000 + ((pid * 11 + salt * 137) % 1400) * 20;
}
static bool openAt(Client& c, ClientConfig& cfg, int salt) {
    std::string err;
    for (int i = 0; i < 12; i++) { cfg.audioPort = pickBase(salt + i); c.configure(cfg); if (c.open(err)) return true; }
    printf("open failed: %s\n", err.c_str());
    return false;
}

static void testConferenceLoopback() {
    Client host, a, b, c;
    ClientConfig ch, ca, cb, cc;
    ch.call = "*OALOOP*"; ch.name = "Loop"; ch.info = "OnAir loopback conference"; ch.servers.clear(); ch.conference = true; ch.conferenceMax = 3;
    ca.call = "LY1ABC"; ca.name = "A"; ca.servers.clear();
    cb.call = "LY2XYZ"; cb.name = "B"; cb.servers.clear();
    cc.call = "LY3QQ"; cc.name = "C"; cc.servers.clear();
    CHECK(openAt(host, ch, 10) && openAt(a, ca, 30) && openAt(b, cb, 50) && openAt(c, cc, 70), "ports");
    CHECK(a.connectTo("127.0.0.1", host.localAudioPort()) && b.connectTo("127.0.0.1", host.localAudioPort()), "two connect");
    CHECK(waitFor([&] { return host.status().members.size() == 2 && a.status().qso == QsoState::Connected && b.status().qso == QsoState::Connected; }, 3000), "two joined");
    // the host calls the third
    CHECK(host.connectTo("127.0.0.1", c.localAudioPort()), "host adds a station");
    CHECK(waitFor([&] { int k = 0; for (auto& m : host.status().members) k += !m.connecting; return k == 3 && c.status().qso == QsoState::Connected; }, 3000), "three in");
    CHECK(waitFor([&] { return a.status().peerInfo.find("Connected stations (3):") != std::string::npos; }, 2000), "list '%s'", a.status().peerInfo.c_str());
    CHECK(host.status().conference && host.status().peerCall == "3 stations", "status '%s'", host.status().peerCall.c_str());

    a.setTransmit(true);
    std::vector<int16_t> tone(8000);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (int16_t)(7000 * std::sin(2 * M_PI * 900.0 * (double)i / 8000));
    for (size_t i = 0; i < tone.size(); i += 640) { a.pushTx(&tone[i], 640); sleepMs(80); }
    a.setTransmit(false);
    CHECK(waitFor([&] { return b.rxBuffered() >= 6400 && c.rxBuffered() >= 6400 && host.rxBuffered() >= 6400; }, 3000), "B %d, C %d, host %d samples",
          b.rxBuffered(), c.rxBuffered(), host.rxBuffered());
    for (Client* x : {&b, &c, &host}) {
        std::vector<int16_t> rx(8000);
        rx.resize((size_t)x->pullRx(rx.data(), 8000));
        const double amp = toneAmp(rx, 1600, 3200, 900);
        printf("loopback conference: %.0f of 7000 at 900 Hz\n", amp);
        CHECK(amp > 4500, "the tone reaches every other station and the host");
    }
    CHECK(a.rxBuffered() < 800, "A hears nothing of itself (%d)", a.rxBuffered());
    CHECK(b.sendChat("hi"), "chat");
    std::vector<std::string> got;
    CHECK(waitFor([&] { for (auto& s : c.takeChat()) got.push_back(s); return !got.empty(); }, 2000) && got[0] == "LY2XYZ>hi", "chat passed on");
    // a fourth is refused: the limit is 3
    Client d; ClientConfig cd; cd.call = "LY4ZZ"; cd.servers.clear();
    CHECK(openAt(d, cd, 90), "port");
    d.connectTo("127.0.0.1", host.localAudioPort());
    CHECK(waitFor([&] { return d.status().qso == QsoState::Idle; }, 3000), "full conference refuses");
    CHECK(host.disconnectStation("LY3QQ"), "kick");
    CHECK(waitFor([&] { return c.status().qso == QsoState::Idle && host.status().members.size() == 2; }, 2000), "kicked");
    host.close();
    CHECK(waitFor([&] { return a.status().qso == QsoState::Idle && b.status().qso == QsoState::Idle; }, 2000), "closing the conference sends BYE to all");
    a.close(); b.close(); c.close(); d.close();
}

// ---- the proxy -------------------------------------------------------------------------------------------------------------------------
static void testProxyBytes() {
    const uint8_t d[3] = {1, 2, 3};
    const std::vector<uint8_t> m = makeProxyBlock(ProxyType::UdpData, "192.168.1.2", d, 3);
    const uint8_t want[] = {5, 192, 168, 1, 2, 3, 0, 0, 0, 1, 2, 3};
    CHECK(m.size() == sizeof want && std::memcmp(m.data(), want, sizeof want) == 0, "block layout");
    const std::vector<uint8_t> big = makeProxyBlock(ProxyType::TcpData, "", nullptr, 0);
    CHECK(big.size() == 9 && big[0] == 2 && big[1] == 0 && big[5] == 0, "empty block");
    const uint8_t nonce[8] = {'0', '1', 'a', 'b', 'c', 'd', '8', '9'};
    const std::vector<uint8_t> au = proxyAuthReply("ly1abc", "secret", nonce);
    std::string pre = "SECRET";
    pre.append((const char*)nonce, 8);
    uint8_t md[16];
    dect2::winlink::md5((const uint8_t*)pre.data(), pre.size(), md);
    CHECK(au.size() == 6 + 1 + 16 && std::memcmp(au.data(), "LY1ABC\n", 7) == 0 && std::memcmp(au.data() + 7, md, 16) == 0, "auth reply");
    // MD5 itself (RFC 1321): MD5("PUBLIC01abcd89") is what a public proxy expects
    uint8_t m1[16];
    dect2::winlink::md5((const uint8_t*)"abc", 3, m1);
    CHECK(m1[0] == 0x90 && m1[1] == 0x01 && m1[15] == 0x72, "md5('abc')");
    const std::vector<uint8_t> pub = proxyAuthReply("LY1ABC", "", nonce);
    std::string pp = "PUBLIC"; pp.append((const char*)nonce, 8);
    dect2::winlink::md5((const uint8_t*)pp.data(), pp.size(), md);
    CHECK(std::memcmp(pub.data() + 7, md, 16) == 0, "an empty password is PUBLIC");
    // the parser at every chunk size
    std::vector<uint8_t> stream(nonce, nonce + 8);
    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0; i < 20; i++) {
        std::vector<uint8_t> payload((size_t)(i * 37 % 200));
        for (size_t k = 0; k < payload.size(); k++) payload[k] = (uint8_t)(i + k);
        blocks.push_back(makeProxyBlock((ProxyType)(1 + i % 7), "10.1.2." + std::to_string(i), payload.data(), payload.size()));
        stream.insert(stream.end(), blocks.back().begin(), blocks.back().end());
    }
    for (size_t chunk = 1; chunk <= 40; chunk++) {
        ProxyParser p;
        int got = 0;
        bool ok = true;
        for (size_t at = 0; at < stream.size(); at += chunk) {
            p.feed(stream.data() + at, std::min(chunk, stream.size() - at));
            ProxyBlock b;
            while (p.next(b)) {
                const std::vector<uint8_t> again = makeProxyBlock(b.type, b.ip, b.data.data(), b.data.size());
                ok = ok && again == blocks[(size_t)got];
                got++;
            }
        }
        CHECK(ok && got == 20 && std::memcmp(p.nonce(), nonce, 8) == 0, "chunk %zu: %d blocks", chunk, got);
    }
    ProxyParser bad;
    const uint8_t junk[] = {0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0};
    bad.feed(junk, sizeof junk);
    ProxyBlock b;
    CHECK(!bad.next(b) && bad.failed(), "unknown type refused");
    ProxyParser huge;
    const uint8_t hb[] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0};
    huge.feed(hb, sizeof hb);
    CHECK(!huge.next(b) && huge.failed(), "oversized block refused");
}

static const char* kList =
    "@@@\n3\n"
    "LY1ABC\nVilnius [ON 12:34]\n123456\n127.0.0.1\n"
    "LY2XYZ-L\nKaunas 145.500 [BUSY 08:15]\n234567\n10.0.0.2\n"
    "*LITHUANIA*\nConference server  [ON 23:59]\n456789\n10.0.0.4\n"
    "+++";

static dect2::sock_t tcpListen(int& port) {
    dect2::netInit();
    dect2::sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return dect2::kBadSock;
    socklen_t al = sizeof a; getsockname(ls, (sockaddr*)&a, &al);
    port = ntohs(a.sin_port);
    return ls;
}

// The directory: one command per connection
struct MockDir {
    dect2::sock_t ls = dect2::kBadSock;
    int port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> logins{0}, lists{0}, offs{0};
    std::thread th;
    bool start() { ls = tcpListen(port); if (ls == dect2::kBadSock) return false; th = std::thread([this] { loop(); }); return true; }
    void loop() {
        while (!stop) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 50000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            dect2::sock_t c = accept(ls, nullptr, nullptr);
            if (c == dect2::kBadSock) continue;
            dect2::sockRecvTimeout(c, 2000);
            std::string cmd; char b[512];
            for (;;) {
                const long long n = dect2::sockRecv(c, b, sizeof b);
                if (n <= 0) break;
                cmd.append(b, (size_t)n);
                if (cmd == "s") break;
                if (cmd[0] == 'l' && std::count(cmd.begin(), cmd.end(), '\r') >= 3) break;
            }
            std::string reply;
            if (cmd == "s") { reply = kList; lists++; }
            else if (cmd.size() > 1 && cmd[0] == 'l') {
                reply = cmd.find("\xAC\xACs3cret\r") != std::string::npos ? "OK2.6\r" : "BAD\r";
                if (cmd.find("OFF-V") != std::string::npos) offs++; else logins++;
            }
            dect2::sockSendAll(c, reply.data(), reply.size());
            dect2::sockClose(c);
        }
    }
    ~MockDir() { stop = true; if (th.joinable()) th.join(); if (ls != dect2::kBadSock) dect2::sockClose(ls); }
};

// A proxy: checks the log-in, then carries UDP and one TCP connection. The far end of UDP is farAudioPort (5198 for a real proxy) and the
// directory is at dirPort (5200 for a real one), so the test can run on 127.0.0.1 next to the other stations.
struct MockProxy {
    dect2::sock_t ls = dect2::kBadSock, ua = dect2::kBadSock, uc = dect2::kBadSock;
    int port = 0, udpBase = 0, farAudioPort = 0, dirPort = 0;
    std::string password = "PROXYPW";
    std::atomic<bool> stop{false};
    std::atomic<int> authOk{0}, authBad{0}, udpOut{0}, udpIn{0}, tcpOpens{0};
    std::string seenCall;
    std::thread th;
    bool start(int base) {
        ls = tcpListen(port);
        if (ls == dect2::kBadSock) return false;
        for (int i = 0; i < 12 && ua == dect2::kBadSock; i++) {
            udpBase = pickBase(base + i);
            ua = udp(udpBase);
            uc = ua == dect2::kBadSock ? dect2::kBadSock : udp(udpBase + 1);
            if (uc == dect2::kBadSock && ua != dect2::kBadSock) { dect2::sockClose(ua); ua = dect2::kBadSock; }
        }
        if (ua == dect2::kBadSock) return false;
        th = std::thread([this] { loop(); });
        return true;
    }
    static dect2::sock_t udp(int p) {
        dect2::sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)p); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) { dect2::sockClose(s); return dect2::kBadSock; }
        return s;
    }
    void loop() {
        while (!stop) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 50000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            dect2::sock_t c = accept(ls, nullptr, nullptr);
            if (c == dect2::kBadSock) continue;
            serve(c);
            dect2::sockClose(c);
        }
    }
    void serve(dect2::sock_t c) {
        const uint8_t nonce[8] = {'4', 'f', '2', 'a', '9', '0', 'c', '1'};
        dect2::sockSendAll(c, nonce, 8);
        dect2::sockRecvTimeout(c, 2000);
        std::vector<uint8_t> in;
        uint8_t b[4096];
        while (true) {
            auto nl = std::find(in.begin(), in.end(), '\n');
            if (nl != in.end() && in.end() - nl >= 17) break;
            const long long n = dect2::sockRecv(c, b, sizeof b);
            if (n <= 0) return;
            in.insert(in.end(), b, b + n);
        }
        auto nl = std::find(in.begin(), in.end(), '\n');
        seenCall.assign(in.begin(), nl);
        const std::vector<uint8_t> want = proxyAuthReply(seenCall, password, nonce);
        if (!std::equal(want.begin(), want.end(), in.begin()) || in.size() < want.size()) {
            authBad++;
            const uint8_t code = kProxyBadPassword;
            const std::vector<uint8_t> m = makeProxyBlock(ProxyType::System, "", &code, 1);
            dect2::sockSendAll(c, m.data(), m.size());
            sleepMs(100);
            return;
        }
        authOk++;
        ProxyParser p;
        const uint8_t zero[8] = {};
        p.feed(zero, 8);   // the parser expects a nonce first; the client sends none
        p.feed(in.data() + want.size(), in.size() - want.size());
        dect2::sock_t dir = dect2::kBadSock;
        while (!stop) {
            ProxyBlock blk;
            while (p.next(blk)) {
                if (blk.type == ProxyType::UdpData || blk.type == ProxyType::UdpControl) {
                    const bool ctrl = blk.type == ProxyType::UdpControl;
                    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons((uint16_t)(farAudioPort + (ctrl ? 1 : 0)));
                    inet_pton(AF_INET, blk.ip.c_str(), &to.sin_addr);
                    dect2::sockSendTo(ctrl ? uc : ua, blk.data.data(), blk.data.size(), to);
                    udpOut++;
                } else if (blk.type == ProxyType::TcpOpen) {
                    tcpOpens++;
                    std::string e;
                    dir = dect2::tcpConnect(blk.ip, dirPort, 2000, e);
                    uint8_t st[4] = {dir == dect2::kBadSock ? (uint8_t)1 : (uint8_t)0, 0, 0, 0};
                    const std::vector<uint8_t> m = makeProxyBlock(ProxyType::TcpStatus, "", st, 4);
                    dect2::sockSendAll(c, m.data(), m.size());
                } else if (blk.type == ProxyType::TcpData && dir != dect2::kBadSock) {
                    dect2::sockSendAll(dir, blk.data.data(), blk.data.size());
                } else if (blk.type == ProxyType::TcpClose && dir != dect2::kBadSock) {
                    dect2::sockClose(dir); dir = dect2::kBadSock;
                }
            }
            if (p.failed()) return;
            fd_set rd; FD_ZERO(&rd); FD_SET(c, &rd); FD_SET(ua, &rd); FD_SET(uc, &rd);
            dect2::sock_t top = std::max(c, std::max(ua, uc));
            if (dir != dect2::kBadSock) { FD_SET(dir, &rd); top = std::max(top, dir); }
            timeval tv{0, 20000};
            if (select((int)top + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(c, &rd)) {
                const long long n = recv(c, (char*)b, sizeof b, 0);
                if (n <= 0) break;
                p.feed(b, (size_t)n);
            }
            for (int w = 0; w < 2; w++) {
                const dect2::sock_t u = w ? uc : ua;
                if (!FD_ISSET(u, &rd)) continue;
                sockaddr_in from{}; socklen_t fl = sizeof from;
                const long long n = recvfrom(u, (char*)b, sizeof b, 0, (sockaddr*)&from, &fl);
                if (n <= 0) continue;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                const std::vector<uint8_t> m = makeProxyBlock(w ? ProxyType::UdpControl : ProxyType::UdpData, ip, b, (size_t)n);
                dect2::sockSendAll(c, m.data(), m.size());
                udpIn++;
            }
            if (dir != dect2::kBadSock && FD_ISSET(dir, &rd)) {
                const long long n = recv(dir, (char*)b, sizeof b, 0);
                if (n <= 0) {
                    dect2::sockClose(dir); dir = dect2::kBadSock;
                    const std::vector<uint8_t> m = makeProxyBlock(ProxyType::TcpClose, "");
                    dect2::sockSendAll(c, m.data(), m.size());
                } else {
                    const std::vector<uint8_t> m = makeProxyBlock(ProxyType::TcpData, "", b, (size_t)n);
                    dect2::sockSendAll(c, m.data(), m.size());
                }
            }
        }
        if (dir != dect2::kBadSock) dect2::sockClose(dir);
    }
    ~MockProxy() {
        stop = true;
        if (th.joinable()) th.join();
        if (ls != dect2::kBadSock) dect2::sockClose(ls);
        if (ua != dect2::kBadSock) dect2::sockClose(ua);
        if (uc != dect2::kBadSock) dect2::sockClose(uc);
    }
};

static void testProxyClient() {
    MockDir dir;
    CHECK(dir.start(), "mock directory");
    Client b;   // the station on the other side, reachable directly
    ClientConfig cb; cb.call = "LY2XYZ"; cb.name = "B"; cb.info = "direct station"; cb.servers.clear();
    CHECK(openAt(b, cb, 110), "B ports");
    MockProxy px;
    px.farAudioPort = b.localAudioPort();
    px.dirPort = dir.port;
    CHECK(px.start(130), "mock proxy");

    Client a;   // behind NAT: only the proxy
    ClientConfig ca; ca.call = "ly1abc"; ca.name = "A"; ca.info = "behind a proxy"; ca.password = "s3cret";
    ca.servers = {"127.0.0.1"}; ca.directoryTimeoutMs = 3000;
    ca.proxyHost = "127.0.0.1"; ca.proxyPort = px.port; ca.proxyPassword = "proxypw";
    a.configure(ca);
    std::string err;
    CHECK(a.open(err), "open through the proxy: %s", err.c_str());
    CHECK(waitFor([&] { return px.authOk == 1; }, 2000) && px.seenCall == "LY1ABC", "the proxy accepted the log-in ('%s')", px.seenCall.c_str());
    CHECK(a.status().viaProxy && a.localAudioPort() == 0, "no local UDP ports");
    a.login();
    CHECK(waitFor([&] { return a.status().loggedIn; }, 4000), "logged in through the proxy: '%s'", a.status().directory.c_str());
    a.refreshList();
    CHECK(waitFor([&] { return !a.status().busy && a.stations().size() == 3; }, 4000), "list through the proxy (%zu): '%s'", a.stations().size(), a.status().directory.c_str());
    CHECK(px.tcpOpens == 2 && dir.logins == 1 && dir.lists == 1, "one TCP_OPEN per command (%d)", px.tcpOpens.load());

    CHECK(a.connectTo("127.0.0.1"), "connect through the proxy");
    CHECK(waitFor([&] { return a.status().qso == QsoState::Connected && b.status().qso == QsoState::Connected; }, 3000), "QSO through the proxy");
    CHECK(b.status().peerCall == "LY1ABC" && a.status().peerCall == "LY2XYZ", "names");
    CHECK(waitFor([&] { return a.status().peerInfo == "direct station" && b.status().peerInfo == "behind a proxy"; }, 2000), "info both ways");
    std::vector<int16_t> tone(8000);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (int16_t)(7000 * std::sin(2 * M_PI * 1100.0 * (double)i / 8000));
    a.setTransmit(true);
    for (size_t i = 0; i < tone.size(); i += 640) { a.pushTx(&tone[i], 640); sleepMs(40); }
    a.setTransmit(false);
    b.setTransmit(true);
    for (size_t i = 0; i < tone.size(); i += 640) { b.pushTx(&tone[i], 640); sleepMs(40); }
    b.setTransmit(false);
    CHECK(waitFor([&] { return b.rxBuffered() >= 7680 && a.rxBuffered() >= 7680; }, 3000), "audio both ways (B %d, A %d)", b.rxBuffered(), a.rxBuffered());
    for (Client* x : {&a, &b}) {
        std::vector<int16_t> rx(8000);
        rx.resize((size_t)x->pullRx(rx.data(), 8000));
        const double amp = toneAmp(rx, 1600, 3200, 1100);
        printf("proxy: %.0f of 7000 at 1100 Hz\n", amp);
        CHECK(amp > 4500, "tone through the proxy");
    }
    CHECK(a.sendChat("via proxy"), "chat");
    std::vector<std::string> got;
    CHECK(waitFor([&] { for (auto& s : b.takeChat()) got.push_back(s); return !got.empty(); }, 2000) && got[0] == "LY1ABC>via proxy", "chat through the proxy");
    for (const std::string& l : a.takeLog()) CHECK(l.find("proxypw") == std::string::npos && l.find("PROXYPW") == std::string::npos && l.find("s3cret") == std::string::npos, "no password in '%s'", l.c_str());
    CHECK(a.status().proxy.find("proxypw") == std::string::npos, "no password in the status");
    a.close();
    CHECK(waitFor([&] { return b.status().qso == QsoState::Idle; }, 2000), "the BYE goes through the proxy");
    CHECK(waitFor([&] { return dir.offs == 1; }, 2000), "the log-out goes through the proxy (%d)", dir.offs.load());
    CHECK(px.udpOut > 10 && px.udpIn > 10, "the proxy carried the UDP (%d out, %d in)", px.udpOut.load(), px.udpIn.load());

    // a wrong proxy password
    Client w;
    ClientConfig cw = ca; cw.proxyPassword = "nope";
    w.configure(cw);
    CHECK(w.open(err), "the TCP connection opens");
    CHECK(waitFor([&] { return w.status().proxy == "bad proxy password"; }, 3000), "bad password reported: '%s'", w.status().proxy.c_str());
    w.login();
    CHECK(waitFor([&] { return !w.status().busy; }, 3000) && !w.status().loggedIn && w.status().directory.find("proxy") != std::string::npos,
          "log-in without a proxy fails plainly: '%s'", w.status().directory.c_str());
    w.close();
    CHECK(px.authBad == 1, "the mock saw one bad log-in");
    // a proxy that is not there
    Client z;
    ClientConfig cz = ca; cz.proxyPort = 1;
    z.configure(cz);
    CHECK(!z.open(err) && err.find("cannot reach the proxy") != std::string::npos, "'%s'", err.c_str());
    b.close();
}

int main() {
    testConferenceSim();
    testConferenceNatAndLoss();
    testConferenceLoopback();
    testProxyBytes();
    testProxyClient();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("echolink_conf: all passed\n");
    return 0;
}
