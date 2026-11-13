// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Winlink over OnAir's own modems, end to end in one process: every frame of a whole B2F session (connect, handshake, proposals,
// compressed messages, disconnect) is rendered by the packet generator (1200 baud AFSK and 9600 baud G3RUH, FM) or the ARDOP generator
// (4FSK, upper sideband), with noise and a tuning offset, and read back by the packet / ARDOP receiver through its frame tap, for
// both directions, against the local mock gateway. Also the "winlink" transmit payload round trip and the transmit generator ending.
// Simulated signals only: no recording of a real Winlink gateway exists in the repository, and nothing is transmitted.
#include "winlink_harness.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/tx/tx.h"
#include "dect2/winlink/wl_modem.h"
#include <cmath>
#include <cstdio>
#include <mutex>
using namespace wltest;
#include "../common/test_util.h"

namespace {

const std::time_t t0 = 1700000000;

struct Rx {
    // packet or ARDOP receiver hearing one side, with the tapped frames
    PacketReceiver pk;
    ArdopReceiver ar;
    std::vector<ax25::Frame> gotAx;
    std::vector<ArdopRxFrameIn> gotAr;
    bool ardop;
    double rate;
    explicit Rx(bool isArdop) : ardop(isArdop), rate(isArdop ? 250000 : 240000) {
        if (ardop) {
            ar.setSilent(true);
            ar.configure(rate);
            ar.setSignalOffset(-ardopTuning().tuneOffsetHz);
            ar.setFrameTap([this](const ArdopFrameEvent& e) {
                ArdopRxFrameIn f;
                f.code = e.code; f.sid = e.sid; f.bodyOk = e.bodyOk; f.from = e.from; f.to = e.to; f.payload = e.payload;
                gotAr.push_back(f);
            });
        } else {
            pk.setSilent(true);
            pk.configure(rate);
            pk.setSignalOffset(-packetTuning().tuneOffsetHz);
            pk.setFrameTap([this](const ax25::Frame& f, int) { gotAx.push_back(f); });
        }
    }
    void feed(std::vector<cf32> s) {
        s.resize(s.size() + (size_t)(0.4 * rate), cf32(0.f, 0.f));
        for (size_t i = 0; i < s.size(); i += 20000) {
            const size_t n = std::min((size_t)20000, s.size() - i);
            if (ardop) ar.feed(s.data() + i, n); else pk.feed(s.data() + i, n);
        }
        if (ardop) ar.flush();
    }
};

struct ModemResult { bool ok = false; std::string err; int rx = 0, tx = 0; double airSec = 0; size_t frames = 0, heard = 0; };

ModemResult runModem(Transport tr, double snrDb, double cfoClient, double cfoGw, int nGw, int nMine) {
    const bool isArdop = tr == Transport::Ardop;
    const int baud = tr == Transport::Packet9600 ? 9600 : 1200;
    const std::string dir = tempDir("modem");
    Mailbox mb(dir);
    for (int i = 0; i < nMine; i++) mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "over the air " + std::to_string(i), "73 de LY2TST, test " + std::to_string(i) + "\r\n" + std::string(120 * (i + 1), 'z'), t0 + i));
    MockGateway gw;
    for (int i = 0; i < nGw; i++) gw.waiting.push_back(msg("LY1GW", kMyCall, "bulletin " + std::to_string(i), "Weather and news " + std::string(150 * (i + 1), (char)('a' + i)), t0 + 10 + i));
    Rx atGw(isArdop), atClient(isArdop);
    double now = 0;
    ModemResult r;
    // the gateway's link
    std::unique_ptr<Ax25Link> gax;
    std::unique_ptr<ArdopLink> gar;
    ByteLink* gl;
    std::vector<ax25::Frame> gOutAx;
    std::vector<ArdopLinkFrame> gOutAr;
    if (isArdop) {
        ArdopLinkConfig c; c.mycall = kGwCall;
        gar = std::make_unique<ArdopLink>(c);
        gar->sendFrame = [&](const ArdopLinkFrame& f) { gOutAr.push_back(f); };
        gl = gar.get();
    } else {
        Ax25LinkConfig c; c.mycall = kGwCall; c.t1Sec = baud == 9600 ? 4 : 8;
        gax = std::make_unique<Ax25Link>(c);
        gax->sendFrame = [&](const ax25::Frame& f) { gOutAx.push_back(f); };
        gl = gax.get();
    }
    gw.begin([&](const uint8_t* d, size_t n) { gl->write(d, n); });
    gl->onData = [&](const uint8_t* d, size_t n) { gw.s->onBytes(d, n); };
    gl->onState = [&](LinkState s) { if (s == LinkState::Connected && gw.s->state() == B2fSession::State::Idle) gw.s->start(); };
    gl->listen();
    WinlinkStation st(mb);
    StationConfig sc;
    sc.mycall = kMyCall; sc.locator = "KO24"; sc.password = kPassword;
    sc.gateway.call = kGwCall; sc.gateway.transport = tr;
    if (!isArdop) sc.t1Sec = baud == 9600 ? 4 : 8;
    std::string err;
    if (!st.start(sc, now, err)) { r.err = err; return r; }
    // a burst on the air takes its time: frames reach the other side when it has been sent
    struct Pending { double at; std::vector<ax25::Frame> ax; std::vector<ArdopRxFrameIn> ar; bool toGw; };
    std::vector<Pending> pend;
    uint32_t seed = 1;
    auto airOut = [&](bool fromClient, std::vector<ax25::Frame> ax, std::vector<ArdopLinkFrame> ar) {
        if (ax.empty() && ar.empty()) return;
        Rx& rx = fromClient ? atGw : atClient;
        const double cfo = fromClient ? cfoClient : cfoGw;
        std::vector<cf32> s = isArdop ? renderArdopBurst(ar, rx.rate, snrDb, cfo, 0.15, seed++) : renderAx25Burst(ax, baud, rx.rate, snrDb, cfo);
        const double sec = (double)s.size() / rx.rate;
        r.airSec += sec;
        r.frames += isArdop ? ar.size() : ax.size();
        rx.gotAx.clear(); rx.gotAr.clear();
        rx.feed(std::move(s));
        r.heard += isArdop ? rx.gotAr.size() : rx.gotAx.size();
        pend.push_back({now + sec, rx.gotAx, rx.gotAr, fromClient});
    };
    const double dt = 0.05;
    for (; now < 3000 && st.running(); now += dt) {
        airOut(true, st.takeAx25(), st.takeArdop());
        airOut(false, std::move(gOutAx), std::move(gOutAr));
        gOutAx.clear(); gOutAr.clear();
        std::vector<Pending> keep;
        for (auto& p : pend) {
            if (p.at > now) { keep.push_back(std::move(p)); continue; }
            for (auto& f : p.ax) { if (p.toGw) gax->onFrame(f, now); else st.rxAx25(f, now); }
            for (auto& f : p.ar) { if (p.toGw) gar->onFrame(f, now); else st.rxArdop(f, now); }
        }
        pend.swap(keep);
        gl->tick(now);
        st.tick(now);
    }
    r.ok = st.lastOk();
    r.err = st.lastError();
    r.rx = st.received(); r.tx = st.sent();
    for (const auto& m : gw.waiting) {
        B2Message in;
        if (!mb.load(Folder::Inbox, m.mid(), in) || in.body != m.body) { r.ok = false; r.err = "inbox copy differs"; }
    }
    if ((int)gw.got.size() != nMine) { r.ok = false; r.err = "the gateway got " + std::to_string(gw.got.size()); }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return r;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // the transmit payload and generator
    {
        ax25::Frame f;
        f.from.call = "LY2TST"; f.to.call = "LY0GW"; f.to.ssid = 10; f.control = 0x00; f.pid = 0xF0; f.info = std::string("\x01\x02 binary \r\n\0x", 13);
        ax25::Frame g = f; g.control = 0x21; g.pid = -1; g.info.clear();
        const std::string p = winlinkTxPayloadAx25(kModemPacket9600, {f, g});
        int modem = 0;
        std::vector<ax25::Frame> ax;
        std::vector<ArdopLinkFrame> ar;
        CHECK(parseWinlinkTxPayload(p, modem, ax, ar) && modem == kModemPacket9600 && ax.size() == 2 && ax[0].info == f.info && ax[1].control == 0x21, "AX.25 payload round trip");
        ArdopLinkFrame a; a.code = 0x4A; a.sid = 0x33; a.body = {1, 2, 3};
        CHECK(parseWinlinkTxPayload(winlinkTxPayloadArdop({a}), modem, ax, ar) && modem == kModemArdop && ar.size() == 1 && ar[0].body == a.body && ar[0].sid == 0x33, "ARDOP payload round trip");
        CHECK(!parseWinlinkTxPayload("", modem, ax, ar) && !parseWinlinkTxPayload(std::string("\x01\x00\x09", 3), modem, ax, ar), "bad payloads refused");
        TxRequest q;
        q.modeId = "winlinkpkt";
        q.payload = p;
        std::string err;
        auto gen = makeWinlinkTxGenerator(q, 2e6, err);
        CHECK(gen != nullptr && !gen->finished(), "transmit generator: %s", err.c_str());
        if (gen) {
            std::vector<cf32> buf(100000);
            size_t total = 0;
            while (!gen->finished() && total < 20000000) { gen->generate(buf.data(), buf.size()); total += buf.size(); }
            CHECK(gen->finished() && total < 3000000, "the generator ends after the burst (%zu samples)", total);
        }
        q.payload = "\x05xx";
        CHECK(makeWinlinkTxGenerator(q, 2e6, err) == nullptr, "unreadable payload refused");
        q.payload.clear();
        CHECK(makeWinlinkTxGenerator(q, 2e6, err) != nullptr, "empty payload: one test frame");
#ifndef WL_LOCAL_NO_TX_TABLE   // a quick local build without the transmit table (it pulls in every radio driver)
        const TxModeSpec* m = txModeById("winlinkpkt");
        const TxModeSpec* m2 = txModeById("winlinkardop");
        CHECK(m && m2 && txModeFinite(*m) && txModeFinite(*m2) && txModeTuneOffsetHz(*m) == packetTuning().tuneOffsetHz && txModeTuneOffsetHz(*m2) == ardopTuning().tuneOffsetHz,
              "the Winlink transmit modes exist, end by themselves, and sit where the receivers listen");
#endif
    }
    struct Case { Transport tr; double snr, cfoC, cfoG; const char* name; };
    const Case cases[] = {
        {Transport::Packet1200, 25, 0, 0, "1200 baud, clean"},
        {Transport::Packet1200, 14, 1500, -2000, "1200 baud, 14 dB, offsets +1.5 / -2 kHz"},
        {Transport::Packet9600, 25, -800, 600, "9600 baud, offsets"},
        {Transport::Packet9600, 18, 1200, -1200, "9600 baud, 18 dB"},
        {Transport::Ardop, 99, 0, 0, "ARDOP, clean"},
        {Transport::Ardop, 12, 25, -30, "ARDOP, 12 dB, offsets +25 / -30 Hz"},
        {Transport::Packet1200, 6, 900, -700, "1200 baud, 6 dB"},
        {Transport::Ardop, -1, -15, 10, "ARDOP, -1 dB"},
    };
    for (const Case& c : cases) {
        const ModemResult r = runModem(c.tr, c.snr, c.cfoC, c.cfoG, 2, 1);
        printf("%s: %s, %d in, %d out, %zu frames sent, %zu heard, %.0f s on the air\n", c.name, r.ok ? "ok" : r.err.c_str(), r.rx, r.tx, r.frames, r.heard, r.airSec);
        CHECK(r.ok && r.rx == 2 && r.tx == 1, "%s: %s", c.name, r.err.c_str());
    }
    return testSummary("winlink modem");
}
