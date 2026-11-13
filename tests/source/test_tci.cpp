// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The TCI source (source_tci.cpp) against a fake TCI server on localhost that follows the public spec 1.9: WebSocket upgrade, initialization
// commands up to ready, then iq_samplerate / dds / vfo / iq_start / iq_stop from the client, and binary IQ blocks (64-byte header, float32 / int16 /
// int24 samples). Checks the handshake and probe, the commands sent (and that no transmit command ever is), the IQ delivered in each sample
// format, retuning, a rate change, malformed blocks, a reconnect, and servers that are not TCI. No real ExpertSDR3 was available.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/ws_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

template <class F> static bool eventually(F f, int ms = 4000) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}

struct Fake {
    enum Mode { Tci, Plain, Silent } mode = Tci;
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true};
    std::atomic<int> sendStop{0}, dropNow{0}, badBurst{0}, connections{0}, fmt{3};   // fmt: sample type of the blocks (0 int16, 1 int24, 3 float32)
    std::mutex mu;
    std::vector<std::string> log;   // each client command as "name:args" lower case
    bool streaming = false;
    std::atomic<bool> radioOn{false};
    std::atomic<uint32_t> rate{48000}, rx{0};

    bool has(const std::string& l) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x == l) return true; return false; }
    int count(const std::string& l) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& x : log) n += x == l; return n; }
    bool anyPrefix(const std::string& p) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x.compare(0, p.size(), p) == 0) return true; return false; }

    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        sockSetNonBlocking(ls, true);
        th = std::thread([this] { loop(); });
        return true;
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }

    static void wsSend(sock_t c, int op, const std::string& p) {
        std::string f;
        f += (char)(0x80 | op);
        if (p.size() < 126) f += (char)p.size();
        else if (p.size() < 65536) { f += (char)126; f += (char)(p.size() >> 8); f += (char)(p.size() & 255); }
        else { f += (char)127; for (int i = 7; i >= 0; i--) f += (char)((uint64_t)p.size() >> (8 * i) & 255); }
        sockSendAll(c, (f + p).data(), f.size() + p.size());
    }
    static void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s += (char)(v >> (8 * i) & 255); }
    // a stream block: receiver, rate, format, codec, crc, length (real samples), type, channels, 8 reserved; then n complex samples
    static std::string block(uint32_t rx, uint32_t sr, uint32_t format, uint32_t type, uint32_t channels, float i, float q, int n) {
        std::string p;
        for (uint32_t v : {rx, sr, format, 0u, 0u, (uint32_t)(n * 2), type, channels}) put32(p, v);
        for (int k = 0; k < 8; k++) put32(p, 0);
        for (int k = 0; k < n; k++)
            for (float v : {i, q}) {
                if (format == 3) { uint32_t u; memcpy(&u, &v, 4); put32(p, u); }
                else if (format == 0) { const int16_t s = (int16_t)std::lround(v * 32767); p += (char)(s & 255); p += (char)(s >> 8 & 255); }
                else { const int32_t s = (int32_t)std::lround(v * 8388607); p += (char)(s & 255); p += (char)(s >> 8 & 255); p += (char)(s >> 16 & 255); }
            }
        return p;
    }

    bool rd(sock_t c, void* d, size_t n, bool idleOk) {
        char* p = (char*)d;
        size_t got = 0;
        int idle = 0;
        while (got < n) {
            const long long k = sockRecv(c, p + got, n - got);
            if (k > 0) { got += (size_t)k; continue; }
            if (k == 0) return false;
            if (got == 0 && idleOk) return false;
            if (++idle > 100) return false;
        }
        return true;
    }

    void serve(sock_t c) {
        sockRecvTimeout(c, 20);
        std::string head;
        char ch;
        while (head.size() < 4096 && run) {
            if (sockRecv(c, &ch, 1) <= 0) { if (!run) return; continue; }
            head += ch;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (mode == Plain) { const char* g = "SSH-2.0-not a websocket\r\n\r\n"; sockSendAll(c, g, strlen(g)); return; }
        const size_t k = head.find("Sec-WebSocket-Key: ");
        if (k == std::string::npos) return;
        const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
        const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
        sockSendAll(c, r.data(), r.size());
        { std::lock_guard<std::mutex> lk(mu); streaming = false; }
        if (mode == Tci) {   // two messages, the first with several commands
            wsSend(c, 1, "protocol:ExpertSDR3,1.9;device:SunSDR2DX;receive_only:false;trx_count:2;vfo_limits:10000,30000000;");
            wsSend(c, 1, "if_limits:-24000,24000;modulations_list:AM,LSB,USB;ready;");
            if (sendStop) wsSend(c, 1, "stop;");   // the radio is switched off: no IQ until "start"
        }
        int tick = 0;
        while (run) {
            if (dropNow.exchange(0)) return;
            if (badBurst.exchange(0)) {
                wsSend(c, 1, "garbage without a semicolon");
                wsSend(c, 2, std::string("\x01\x02", 2));                              // shorter than a header
                wsSend(c, 2, block(rx.load(), rate.load(), 3, 0, 2, 1, 1, 10) + std::string("\x01", 1));   // odd size
                wsSend(c, 2, block(rx.load(), rate.load(), 9, 0, 2, 1, 1, 10));                       // unknown sample format
                wsSend(c, 2, block(rx.load(), rate.load(), 3, 1, 2, 1, 1, 10));                       // audio stream: ignored
                wsSend(c, 2, block(rx.load() + 1, rate.load(), 3, 0, 2, 1, 1, 10));                       // another receiver: ignored
                wsSend(c, 2, block(rx.load(), rate.load(), 3, 0, 1, 1, 1, 10));                       // one channel: not IQ
            }
            bool s;
            { std::lock_guard<std::mutex> lk(mu); s = streaming; }
            if (s && (!sendStop || radioOn) && (++tick % 5) == 0) wsSend(c, 2, block(rx.load(), rate.load(), (uint32_t)fmt.load(), 0, 2, 0.5f, -0.25f, 1024));
            unsigned char h[2];
            if (!rd(c, h, 2, true)) { if (!run) return; if (sockPeekClosed(c) == 0) return; continue; }
            const int op = h[0] & 15;
            size_t len = h[1] & 127;
            if (len == 126) { unsigned char x[2]; if (!rd(c, x, 2, false)) return; len = x[0] << 8 | x[1]; }
            unsigned char m[4];
            if (!rd(c, m, 4, false)) return;
            std::string p(len, 0);
            if (len && !rd(c, &p[0], len, false)) return;
            for (size_t i = 0; i < len; i++) p[i] ^= m[i & 3];
            if (op == 8) return;
            if (op != 1) continue;
            std::vector<std::pair<std::string, std::vector<std::string>>> cs;
            native::tciParseCommands(p, cs);
            for (auto& cm : cs) {
                std::string line = cm.first + ":";
                for (size_t i = 0; i < cm.second.size(); i++) line += (i ? "," : "") + cm.second[i];
                std::lock_guard<std::mutex> lk(mu);
                log.push_back(line);
                if (cm.first == "start") radioOn = true;
                else if (cm.first == "iq_start") { streaming = true; rx = cm.second.empty() ? 0 : (uint32_t)atoi(cm.second[0].c_str()); }
                else if (cm.first == "iq_stop") streaming = false;
                else if (cm.first == "iq_samplerate" && !cm.second.empty()) rate = (uint32_t)atoi(cm.second[0].c_str());
            }
        }
    }
    void loop() {
        while (run) {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            sockSetNonBlocking(c, false);
            connections++;
            serve(c);
            sockShutdown(c);
            sockClose(c);
        }
    }
};

static bool readIq(IqRing& ring, std::vector<cf32>& buf) {
    size_t got = 0;
    for (int i = 0; i < 400 && got < buf.size(); i++) { got += ring.read(buf.data() + got, buf.size() - got); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return got == buf.size();
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    // the command parser
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> cs;
        CHECK(native::tciParseCommands("DDS:0,7100000;vfo:0,1,7100000;ready;", cs) && cs.size() == 3);
        CHECK(cs.size() == 3 && cs[0].first == "dds" && cs[0].second.size() == 2 && cs[0].second[1] == "7100000" && cs[2].first == "ready" && cs[2].second.empty());
        CHECK(!native::tciParseCommands("no semicolon", cs) && !native::tciParseCommands("", cs) && !native::tciParseCommands(";;;", cs));
        CHECK(native::tciParseCommands(std::string(5000, 'x') + ";a:1;", cs) && cs.size() == 1 && cs[0].first == "a");   // an over-long name is dropped
    }
    // not TCI servers
    {
        Fake f; f.mode = Fake::Plain; CHECK(f.start());
        std::string err;
        CHECK(!native::tciProbe("127.0.0.1", f.port, 1000, err) && err.find("WebSocket") != std::string::npos);
        f.stop();
    }
    {
        Fake f; f.mode = Fake::Silent; CHECK(f.start());
        std::string err;
        CHECK(!native::tciProbe("127.0.0.1", f.port, 600, err) && err.find("TCI") != std::string::npos);
        f.stop();
    }
    {
        std::string err;
        CHECK(!native::tciProbe("127.0.0.1", 1, 500, err) && err.find("TCI") != std::string::npos);
    }

    Fake srv;
    CHECK(srv.start());
    NetRadio nr;
    nr.type = NetRadio::Tci;
    nr.host = "127.0.0.1";
    nr.port = srv.port;
    CHECK(netRadioDefaultPort(NetRadio::Tci) == 50001 && std::string(netRadioTypeName(NetRadio::Tci)) == "TCI (network)");
    CHECK(netRadiosFromString(netRadiosToString({nr})).size() == 1 && netRadiosFromString(netRadiosToString({nr}))[0] == nr);
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));

    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return 1; }
    DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "tci" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port) && d.name == netRadioDeviceName(nr));
    CHECK(d.minFreqHz == 10000 && d.maxFreqHz == 30e6 && d.maxRateHz == 384000 && d.rateRanges.size() == 4 && d.gainMaxDb == 0);
    bool haveRx = false;
    for (const auto& s : d.settings) if (s.key == "receiver") haveRx = s.values.size() == 2;
    CHECK(haveRx);

    auto src = native::makeTci(d);
    IqRing ring(1u << 20);
    TuneSettings t;
    t.sampleRate = 100e3; t.centerHz = 7.1e6;
    t.radio["receiver"] = "1";
    err.clear();
    CHECK(src->start(t, ring, err));
    CHECK(eventually([&] { return srv.has("iq_start:1"); }));
    CHECK(srv.has("iq_samplerate:192000") && srv.has("dds:1,7100000") && srv.has("vfo:1,0,7100000"));
    CHECK(eventually([&] { return std::fabs(src->sampleRate() - 192000) < 1; }));
    {   // the fake sends blocks of the receiver asked for in iq_start
        std::vector<cf32> buf(4096);
        CHECK(readIq(ring, buf));
    }
    src->stop();
    CHECK(eventually([&] { return srv.has("iq_stop:1"); }, 15000));   // the fake server reads the command a moment after stop() returns
    // transmit and audio commands are never sent
    CHECK(!srv.anyPrefix("trx") && !srv.anyPrefix("tune") && !srv.anyPrefix("drive") && !srv.anyPrefix("audio") && !srv.anyPrefix("cw_") && !srv.anyPrefix("tx_"));

    // receiver 0 in each sample format
    for (int f : {3, 0, 1}) {
        srv.fmt = f;
        {   std::lock_guard<std::mutex> lk(srv.mu); srv.log.clear(); }
        auto s2 = native::makeTci(d);
        IqRing r2(1u << 20);
        TuneSettings t2;
        t2.sampleRate = 48e3; t2.centerHz = 14.2e6;
        err.clear();
        CHECK(s2->start(t2, r2, err));
        CHECK(eventually([&] { return srv.has("iq_start:0"); }));
        CHECK(srv.has("iq_samplerate:48000") && srv.has("dds:0,14200000"));
        std::vector<cf32> buf(8192);
        CHECK(readIq(r2, buf));
        bool ok = true;
        for (size_t i = 2048; i < buf.size(); i++) ok = ok && std::fabs(buf[i].real() - 0.5f) < 1e-3f && std::fabs(buf[i].imag() + 0.25f) < 1e-3f;
        CHECK(ok);
        if (f == 3) {
            // retune live: only what changed is sent; a new rate is sent and followed
            t2.centerHz = 14.3e6;
            err.clear();
            CHECK(s2->retune(t2, err));
            CHECK(eventually([&] { return srv.has("dds:0,14300000"); }));
            CHECK(srv.count("iq_samplerate:48000") == 1);
            t2.sampleRate = 384e3;
            CHECK(s2->retune(t2, err));
            CHECK(eventually([&] { return srv.has("iq_samplerate:384000"); }));
            CHECK(eventually([&] { return std::fabs(s2->sampleRate() - 384000) < 1; }));
            // malformed blocks: ignored, the stream goes on with finite samples
            r2.clear();
            srv.badBurst = 1;
            CHECK(readIq(r2, buf));
            bool finite = true;
            for (auto& v : buf) finite = finite && std::isfinite(v.real()) && std::isfinite(v.imag());
            CHECK(finite);
            // the server drops the connection and it is opened again, with the settings in use
            const int before = srv.connections;
            srv.dropNow = 1;
            CHECK(eventually([&] { return srv.connections > before; }, 8000));
            r2.clear();
            CHECK(readIq(r2, buf));
            CHECK(eventually([&] { return srv.count("iq_samplerate:384000") >= 2 && srv.count("dds:0,14300000") >= 2; }));
        }
        s2->stop();
    }
    srv.stop();

    // a server whose radio is stopped sends no IQ until "start": the client asks for it once
    {
        Fake f2; f2.sendStop = 1; CHECK(f2.start());
        NetRadio n2; n2.type = NetRadio::Tci; n2.host = "127.0.0.1"; n2.port = f2.port;
        std::string e2;
        CHECK(probeNetRadio(n2, 2000, e2));
        const auto dv = netRadioDevices({n2});
        CHECK(dv.size() == 1);
        if (!dv.empty()) {
            auto s3 = native::makeTci(dv[0]);
            IqRing r3(1u << 20);
            TuneSettings t3; t3.sampleRate = 48e3; t3.centerHz = 7.1e6;
            CHECK(s3->start(t3, r3, e2));
            std::vector<cf32> b3(2048);
            CHECK(readIq(r3, b3));
            CHECK(f2.count("start:") == 1);
            s3->stop();
        }
        f2.stop();
    }

    if (fails) { fprintf(stderr, "%d checks failed\n", fails); return 1; }
    printf("test_tci ok\n");
    return 0;
}
