// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SDRplay nRSP-ST source (source_nrspst.cpp) against a fake SDRconnect on localhost that speaks the public WebSocket API 1.0.3: the
// upgrade handshake, get_property / set_property / device_stream_enable / iq_stream_enable, property_changed pushes and binary type-2 IQ.
// Checks the commands the source sends (rate, frequency, gain as an LNA state, antenna, device choice), the IQ it delivers, retuning, a
// reconnect, a server that is not SDRconnect or does not answer, and malformed input (bad JSON, short and odd-sized frames, unknown types, a
// frame over the size limit, fragmentation, pings, random bytes in the frame header). No real nRSP-ST was available: the fake is what the
// spec says, nothing more.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/ws_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
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
    enum Mode { Sdrconnect, Plain, Silent, NoAccept } mode = Sdrconnect;
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true};
    std::atomic<int> dropNow{0}, badBurst{0}, hugeFrame{0}, fragmented{0}, connections{0};
    std::mutex mu;
    std::vector<std::string> log;   // "event|property|value" of each client message
    bool streaming = false;

    bool has(const std::string& l) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x == l) return true; return false; }
    int count(const std::string& l) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& x : log) n += x == l; return n; }
    bool hasPrefix(const std::string& p) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x.compare(0, p.size(), p) == 0) return true; return false; }
    std::string last(const std::string& p) { std::lock_guard<std::mutex> lk(mu); std::string r; for (auto& x : log) if (x.compare(0, p.size(), p) == 0) r = x; return r; }

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

    static std::string jstr(const std::map<std::string, std::string>& m, const char* k) { auto it = m.find(k); return it == m.end() ? "" : it->second; }

    static void wsSend(sock_t c, int op, const std::string& p, bool fin = true) {
        std::string f;
        f += (char)((fin ? 0x80 : 0) | op);
        if (p.size() < 126) f += (char)p.size();
        else if (p.size() < 65536) { f += (char)126; f += (char)(p.size() >> 8); f += (char)(p.size() & 255); }
        else { f += (char)127; for (int i = 7; i >= 0; i--) f += (char)((uint64_t)p.size() >> (8 * i) & 255); }
        sockSendAll(c, (f + p).data(), f.size() + p.size());
    }
    static std::string iqFrame(int16_t i, int16_t q, int n, int type = 2) {
        std::string p;
        p += (char)type; p += (char)0;
        for (int k = 0; k < n; k++) { p += (char)(i & 255); p += (char)(i >> 8 & 255); p += (char)(q & 255); p += (char)(q >> 8 & 255); }
        return p;
    }
    static std::string pc(const char* event, const char* prop, const std::string& v) { return native::nrspMessage(event, prop, v); }

    // the reply to a get_property
    std::string answer(const std::string& prop) {
        static const std::map<std::string, std::string> v = {
            {"api_version", "1.0.3"}, {"valid_devices", "nRSP-ST 1234ABCD,RSPdx 99"}, {"active_device", "nRSP-ST 1234ABCD"},
            {"valid_antennas", "Antenna A,Antenna B,Antenna C"}, {"active_antenna", "Antenna A"}, {"lna_state_min", "0"}, {"lna_state_max", "9"},
            {"can_control", "true"}, {"device_sample_rate", "2000000"}};
        auto it = v.find(prop);
        return it == v.end() ? "" : pc("get_property_response", prop.c_str(), it->second);
    }

    // exactly n bytes from the client; timeoutIdle: give up (false) when nothing arrives at all
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
        const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                              (mode == NoAccept ? std::string("AAAA") : wsAcceptKey(key)) + "\r\n\r\n";
        sockSendAll(c, r.data(), r.size());
        { std::lock_guard<std::mutex> lk(mu); streaming = false; }
        int tick = 0;
        while (run) {
            if (dropNow.exchange(0)) return;
            if (hugeFrame.exchange(0)) { wsSend(c, 2, std::string(17u << 20, 'x')); }   // over the 16 MiB limit (4.0; was 1 MiB)
            if (badBurst.exchange(0)) sendBad(c);
            if (fragmented.exchange(0)) {   // a ping between the fragments of an IQ message
                const std::string f = iqFrame(1000, -1000, 400);
                wsSend(c, 2, f.substr(0, 700), false);
                wsSend(c, 9, "hi");
                wsSend(c, 0, f.substr(700), true);
            }
            bool s;
            { std::lock_guard<std::mutex> lk(mu); s = streaming; }
            if (s && (++tick % 5) == 0) wsSend(c, 2, iqFrame(16384, -8192, 1024));
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
            if (op != 1 || mode == Silent) continue;
            std::map<std::string, std::string> kv;
            if (!native::nrspParseMessage(p, kv)) continue;
            const std::string ev = jstr(kv, "event_type"), pr = jstr(kv, "property"), va = jstr(kv, "value");
            { std::lock_guard<std::mutex> lk(mu); log.push_back(ev + "|" + pr + "|" + va); if (ev == "iq_stream_enable") streaming = va == "true"; }
            if (ev == "get_property") { const std::string a = answer(pr); if (!a.empty()) wsSend(c, 1, a); }
            else if (ev == "set_property" && pr == "device_sample_rate") wsSend(c, 1, pc("property_changed", "device_sample_rate", va));
        }
    }
    void sendBad(sock_t c) {
        wsSend(c, 1, "{not json");
        wsSend(c, 1, "[1,2,3]");
        wsSend(c, 1, "{\"event_type\":\"property_changed\"");
        wsSend(c, 1, "{\"a\":{\"b\":[1,{\"c\":2}]},\"event_type\":\"property_changed\",\"property\":\"overload\",\"value\":\"true\"}");   // nested value skipped, fine
        wsSend(c, 2, std::string("\x02", 1));              // too short
        wsSend(c, 2, iqFrame(5, 5, 3) + std::string("\x01\x02", 2));   // odd size: 3 samples kept
        wsSend(c, 2, iqFrame(1, 1, 8, 9));                 // unknown type
        wsSend(c, 2, iqFrame(1, 1, 8, 1));                 // audio: ignored
        wsSend(c, 2, iqFrame(1, 1, 8, 5));                 // secondary device: ignored
        wsSend(c, 10, "pong");
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

static void jsonTests() {
    std::map<std::string, std::string> m;
    CHECK(native::nrspParseMessage("{\"event_type\":\"property_changed\",\"property\":\"device_center_frequency\",\"device\":\"primary\",\"value\":\"100000000\"}", m));
    CHECK(m["event_type"] == "property_changed" && m["value"] == "100000000" && m["device"] == "primary");
    CHECK(native::nrspParseMessage("{ \"value\" : 12.5 , \"x\" : true }", m) && m["value"] == "12.5" && m["x"] == "true");
    CHECK(native::nrspParseMessage("{\"v\":\"a\\\"b\\u0041\"}", m) && m["v"] == "a\"bA");
    const std::string e = native::nrspMessage("set_property", "device_center_frequency", "101000000");
    CHECK(native::nrspParseMessage(e, m) && m["event_type"] == "set_property" && m["property"] == "device_center_frequency" && m["value"] == "101000000");
    // fuzz-ish: random mutations and truncations of valid messages never crash or hang, and an object that is accepted has only string keys
    std::mt19937 rng(7);
    const std::string base = "{\"event_type\":\"get_property_response\",\"property\":\"valid_antennas\",\"device\":\"primary\",\"value\":\"A,B\"}";
    for (int i = 0; i < 20000; i++) {
        std::string s = base.substr(0, rng() % (base.size() + 1));
        for (int k = 0, n = rng() % 4; k < n && !s.empty(); k++) s[rng() % s.size()] = (char)rng();
        native::nrspParseMessage(s, m);
    }
    CHECK(!native::nrspParseMessage(std::string(100000, '{'), m));
    CHECK(!native::nrspParseMessage("{\"a\":" + std::string(50, '[') + std::string(50, ']') + "}", m));
    CHECK(!native::nrspParseMessage("", m) && !native::nrspParseMessage("{\"a\"}", m) && !native::nrspParseMessage("{\"a\":\"" + std::string(5000, 'x') + "\"}", m));
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    jsonTests();

    // not SDRconnect: a server that does not speak WebSocket, one that does not answer api_version, one with a wrong accept key, and nothing listening
    {
        Fake f; f.mode = Fake::Plain; CHECK(f.start());
        std::string err;
        CHECK(!native::nrspStProbe("127.0.0.1", f.port, 1000, err) && err.find("WebSocket") != std::string::npos);
        f.stop();
    }
    {
        Fake f; f.mode = Fake::Silent; CHECK(f.start());
        std::string err;
        const bool pr = native::nrspStProbe("127.0.0.1", f.port, 600, err); if (pr || err.find("api_version") == std::string::npos) fprintf(stderr, "silent: %s\n", err.c_str());
        CHECK(!pr && err.find("api_version") != std::string::npos);
        f.stop();
    }
    {
        Fake f; f.mode = Fake::NoAccept; CHECK(f.start());
        std::string err;
        CHECK(!native::nrspStProbe("127.0.0.1", f.port, 600, err) && err.find("does not match") != std::string::npos);
        f.stop();
    }
    {
        std::string err;
        CHECK(!native::nrspStProbe("127.0.0.1", 1, 500, err) && err.find("SDRconnect") != std::string::npos);
    }

    Fake srv;
    CHECK(srv.start());
    NetRadio nr;
    nr.type = NetRadio::NrspSt;
    nr.host = "127.0.0.1";
    nr.port = srv.port;
    CHECK(netRadioDefaultPort(NetRadio::NrspSt) == 5454 && std::string(netRadioTypeName(NetRadio::NrspSt)) == "SDRplay nRSP-ST (SDRconnect)");
    CHECK(netRadiosFromString(netRadiosToString({nr})).size() == 1 && netRadiosFromString(netRadiosToString({nr}))[0] == nr);
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));

    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return 1; }
    DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "nrspst" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port) && d.name == netRadioDeviceName(nr));
    CHECK(d.gainMinDb == 0 && d.gainMaxDb == 9 && d.maxFreqHz == 2e9);
    bool haveAnt = false, haveDev = false;
    for (const auto& s : d.settings) {
        if (s.key == "antenna") haveAnt = s.values.size() == 4 && s.values[3] == "Antenna C";
        if (s.key == "device") haveDev = s.values.size() == 3 && s.values[1] == "nRSP-ST 1234ABCD";
    }
    CHECK(haveAnt && haveDev);

    auto src = native::makeNrspSt(d);
    IqRing ring(1u << 20);
    TuneSettings t;
    t.sampleRate = 4e6; t.centerHz = 100e6; t.gainDb = 3;
    t.radio["antenna"] = "Antenna B";
    t.radio["device"] = "nRSP-ST 1234ABCD";
    t.radio["mode"] = "Full IQ";
    err.clear();
    CHECK(src->start(t, ring, err));
    if (!err.empty()) fprintf(stderr, "start: %s\n", err.c_str());
    CHECK(eventually([&] { return srv.has("iq_stream_enable||true") || srv.has("iq_stream_enable||true"); }));
    CHECK(srv.has("selected_device_name||nRSP-ST 1234ABCD:Full IQ"));
    CHECK(srv.has("set_property|device_sample_rate|4000000") && srv.has("set_property|device_center_frequency|100000000") && srv.has("set_property|device_vfo_frequency|100000000"));
    CHECK(srv.has("set_property|lna_state|6") && srv.has("set_property|active_antenna|Antenna B") && srv.has("device_stream_enable||true"));
    CHECK(std::fabs(src->sampleRate() - 4e6) < 1);
    std::vector<cf32> buf(8192);
    CHECK(readIq(ring, buf));
    bool iqOk = true;
    for (size_t i = 4096; i < buf.size(); i++) iqOk = iqOk && std::fabs(buf[i].real() - 0.5f) < 1e-4f && std::fabs(buf[i].imag() + 0.25f) < 1e-4f;
    CHECK(iqOk);

    // retune live: only what changed is sent
    t.centerHz = 101e6; t.gainDb = 9;
    err.clear();
    CHECK(src->retune(t, err));
    CHECK(eventually([&] { return srv.has("set_property|device_center_frequency|101000000") && srv.has("set_property|lna_state|0"); }));
    CHECK(srv.count("set_property|device_sample_rate|4000000") == 1 && srv.count("set_property|active_antenna|Antenna B") == 1);
    t.sampleRate = 6e6;
    CHECK(src->retune(t, err));
    CHECK(eventually([&] { return srv.has("set_property|device_sample_rate|6000000"); }));
    CHECK(eventually([&] { return std::fabs(src->sampleRate() - 6e6) < 1; }));   // SDRconnect's property_changed names the rate it runs

    // malformed messages, a fragmented IQ message with a ping in it: the stream goes on, only the good samples arrive
    ring.clear();
    srv.badBurst = 1;
    srv.fragmented = 1;
    CHECK(readIq(ring, buf));
    bool finite = true;
    for (auto& v : buf) finite = finite && std::isfinite(v.real()) && std::isfinite(v.imag());
    CHECK(finite);

    // a message over the size limit: the connection is dropped, then opened again and the samples go on
    srv.hugeFrame = 1;
    CHECK(eventually([&] { return srv.connections >= 2; }, 6000));
    ring.clear();
    CHECK(readIq(ring, buf));
    CHECK(eventually([&] { return srv.count("set_property|device_sample_rate|6000000") >= 2; }));   // the new connection got the settings in use

    // the server goes away and comes back
    const int before = srv.connections;
    srv.dropNow = 1;
    CHECK(eventually([&] { return srv.connections > before; }, 8000));
    ring.clear();
    CHECK(readIq(ring, buf));
    src->stop();

    srv.stop();

    // a second fake, to see the probe leave nothing behind
    {
        Fake f; CHECK(f.start());
        std::string e;
        CHECK(native::nrspStProbe("127.0.0.1", f.port, 1500, e));
        f.stop();
    }
    if (fails) { fprintf(stderr, "%d checks failed\n", fails); return 1; }
    printf("test_nrspst ok\n");
    return 0;
}
