// Network radios (netradio.h): the rtl_tcp, SpyServer and Hermes Lite 2 clients against fake servers on localhost, a user-added Pluto
// address, the saved list.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include "dect2/net/share.h"
#include "dect2/engine/device_claims.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// A fake rtl_tcp server: greets with "RTL0" (R820T, 29 gains), streams I=200, Q=50 and records every command it gets
struct FakeServer {
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true}, dropNow{false};
    std::atomic<int> connections{0};
    std::mutex mu;
    std::vector<std::pair<int, uint32_t>> cmds;
    unsigned char hdr[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};   // the greeting (another radio's server: its own)

    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) return false;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 2) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    bool has(int cmd, uint32_t v) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& c : cmds) if (c.first == cmd && c.second == v) return true;
        return false;
    }
    bool waitFor(int cmd, uint32_t v, int ms = 3000) {
        for (int i = 0; i < ms / 10; i++) { if (has(cmd, v)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    }
    void loop() {
        while (run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 100000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) continue;
            connections++;
            sockSendAll(c, hdr, sizeof hdr);
            std::vector<unsigned char> chunk(4096);
            for (size_t i = 0; i < chunk.size(); i += 2) { chunk[i] = 200; chunk[i + 1] = 50; }
            std::vector<unsigned char> pend;
            while (run && !dropNow) {
                // the commands that came in
                fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
                timeval t0{0, 0};
                if (select((int)c + 1, &r2, nullptr, nullptr, &t0) > 0) {
                    unsigned char b[256];
                    const long long k = sockRecv(c, b, sizeof b);
                    if (k <= 0) break;
                    pend.insert(pend.end(), b, b + k);
                    while (pend.size() >= 5) {
                        const uint32_t v = (uint32_t)pend[1] << 24 | (uint32_t)pend[2] << 16 | (uint32_t)pend[3] << 8 | pend[4];
                        { std::lock_guard<std::mutex> lk(mu); cmds.push_back({pend[0], v}); }
                        pend.erase(pend.begin(), pend.begin() + 5);
                    }
                }
                if (!sockSendAll(c, chunk.data(), chunk.size())) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            sockShutdown(c);
            sockClose(c);
            dropNow = false;
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }
};

static void testRtlTcp() {
    FakeServer srv;
    CHECK(srv.start());
    if (!srv.port) return;
    NetRadio nr; nr.type = NetRadio::RtlTcp; nr.host = "127.0.0.1"; nr.port = srv.port;
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return; }
    const DeviceInfo& d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "rtltcp" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port));
    CHECK(d.name == netRadioDeviceName(nr));
    // wait for the probe's connection to be over before the source connects
    for (int i = 0; i < 100 && srv.connections < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int before = srv.connections;

    auto src = makeSource(d);
    CHECK(src != nullptr);
    if (!src) { srv.stop(); return; }
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 2.048e6;
    s.centerHz = 100e6;
    s.gainDb = 30;
    s.radio["ppm"] = "3";
    err.clear();
    CHECK(src->start(s, ring, err));
    CHECK(srv.waitFor(0x03, 1));            // manual gain
    CHECK(srv.waitFor(0x08, 0));            // AGC off
    CHECK(srv.waitFor(0x02, 2048000));      // rate
    CHECK(srv.waitFor(0x01, 100000000));    // frequency
    CHECK(srv.waitFor(0x05, 3));            // ppm
    CHECK(srv.waitFor(0x04, 297));          // 30 dB -> the R820T's 29.7 dB
    CHECK(std::fabs(src->sampleRate() - 2.048e6) < 1);

    // the samples arrive, as the server sent them
    std::vector<cf32> buf(4096);
    size_t got = 0;
    for (int i = 0; i < 300 && got < buf.size(); i++) { got += ring.read(buf.data() + got, buf.size() - got); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(got == buf.size());
    bool okVals = got > 0;
    for (size_t i = 0; i < got; i++) if (std::fabs(buf[i].real() - (200 - 127.4f) / 128.f) > 1e-5f || std::fabs(buf[i].imag() - (50 - 127.4f) / 128.f) > 1e-5f) okVals = false;
    CHECK(okVals);

    // live changes
    s.centerHz = 101.5e6; s.gainDb = 40;
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor(0x01, 101500000));
    CHECK(srv.waitFor(0x04, 402));
    s.sampleRate = 1.024e6;
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor(0x02, 1024000));

    // the server drops the connection: the source connects again, sends its settings again and counts the samples it missed
    const uint64_t dropped0 = ring.dropped();
    { std::lock_guard<std::mutex> lk(srv.mu); srv.cmds.clear(); }
    srv.dropNow = true;
    bool back = false;
    for (int i = 0; i < 500 && !back; i++) { back = srv.connections >= before + 2; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(back);
    CHECK(srv.waitFor(0x01, 101500000));
    CHECK(srv.waitFor(0x02, 1024000));
    CHECK(ring.dropped() > dropped0);
    ring.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(ring.available() > 0);   // streaming again
    src->stop();
    srv.stop();

    // nothing listening there any more: a clear error, quickly
    err.clear();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!probeNetRadio(nr, 2000, err));
    CHECK(!err.empty());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    fprintf(stderr, "unreachable: %s\n", err.c_str());
}

// rtl_tcp servers of other radios: set by hand on a real RTL0 server, and found from an rsp_tcp-style greeting
static void testRtlTcpOther() {
    for (int found = 0; found < 2; found++) {
        FakeServer srv;
        if (found) { const unsigned char h[12] = {'R', 'S', 'P', '0', 0, 0, 0, 0, 0, 0, 0, 0}; memcpy(srv.hdr, h, 12); }
        CHECK(srv.start());
        if (!srv.port) return;
        NetRadio nr; nr.type = NetRadio::RtlTcp; nr.host = "127.0.0.1"; nr.port = srv.port;
        std::string err;
        CHECK(probeNetRadio(nr, 2000, err));   // any 4-character magic is a greeting
        auto devs = netRadioDevices({nr});
        CHECK(devs.size() == 1);
        if (devs.empty()) { srv.stop(); return; }
        DeviceInfo d = devs[0];
        TuneSettings s;
        if (!found) {
            CHECK(d.maxRateHz == 2.56e6 && !d.rateRanges.empty());   // an RTL-SDR until told otherwise
            s.radio["server"] = "other";
        }
        applyRadioOptionCaps(d, s);
        CHECK(d.maxRateHz == 20e6 && d.minRateHz == 0.25e6 && d.rateRanges.empty());
        TuneSettings plain;
        DeviceInfo d2 = devs[0];
        applyRadioOptionCaps(d2, plain);
        CHECK((d2.maxRateHz == 20e6) == (found == 1));   // found from the greeting: without the setting too
        for (int i = 0; i < 100 && srv.connections < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto src = makeSource(d);
        CHECK(src != nullptr);
        if (!src) { srv.stop(); return; }
        IqRing ring(1u << 20);
        s.sampleRate = 10e6;
        s.centerHz = 433.92e6;
        s.gainDb = 30;
        err.clear();
        CHECK(src->start(s, ring, err));
        CHECK(srv.waitFor(0x02, 10000000));   // the rate as asked, above an RTL-SDR's
        CHECK(srv.waitFor(0x04, 300));        // the gain as asked, not an R820T step
        s.sampleRate = 0.6e6;                 // in the RTL-SDR's rate gap
        CHECK(src->retune(s, err));
        CHECK(srv.waitFor(0x02, 600000));
        s.sampleRate = 25e6;
        CHECK(!src->retune(s, err) && !err.empty());
        src->stop();
        srv.stop();
    }
}

// A fake SpyServer: answers HELLO with DEVICE_INFO (an Airspy: 10 Msps, 9 decimation stages, 21 gains, 24 to 1800 MHz) and CLIENT_SYNC,
// records every setting and, once streaming is on, sends IQ messages in the format asked for (I = +0.5625 / 0.5, Q = -0.609 / -0.25)
struct FakeSpy {
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true}, dropNow{false}, canControl{true};
    std::atomic<int> connections{0};
    std::mutex mu;
    std::vector<std::pair<uint32_t, uint32_t>> sets;

    static void put(std::vector<unsigned char>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((unsigned char)(v >> (8 * i))); }
    static uint32_t get(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
    static bool message(sock_t c, uint32_t type, uint32_t seq, const std::vector<unsigned char>& body) {
        std::vector<unsigned char> m;
        put(m, (2u << 24) | 1700u); put(m, type); put(m, type >= 100 ? 1 : 0); put(m, seq); put(m, (uint32_t)body.size());
        m.insert(m.end(), body.begin(), body.end());
        return sockSendAll(c, m.data(), m.size());
    }
    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) return false;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 2) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    bool has(uint32_t id, uint32_t v) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& c : sets) if (c.first == id && c.second == v) return true;
        return false;
    }
    bool hasId(uint32_t id) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& c : sets) if (c.first == id) return true;
        return false;
    }
    bool waitFor(uint32_t id, uint32_t v, int ms = 3000) {
        for (int i = 0; i < ms / 10; i++) { if (has(id, v)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    }
    void loop() {
        while (run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 100000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) continue;
            connections++;
            std::vector<unsigned char> pend;
            bool streaming = false;
            uint32_t fmt = 1, seq = 0;
            while (run && !dropNow) {
                fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
                timeval t0{0, 0};
                if (select((int)c + 1, &r2, nullptr, nullptr, &t0) > 0) {
                    unsigned char b[256];
                    const long long k = sockRecv(c, b, sizeof b);
                    if (k <= 0) break;
                    pend.insert(pend.end(), b, b + k);
                    while (pend.size() >= 8 && pend.size() >= 8 + get(pend.data() + 4)) {
                        const uint32_t type = get(pend.data()), n = get(pend.data() + 4);
                        if (type == 0) {   // HELLO: the device and the sync
                            std::vector<unsigned char> di, cs;
                            for (uint32_t v : {1u, 0x1234abcdu, 10000000u, 8000000u, 9u, 1u, 21u, 24000000u, 1800000000u, 12u, 0u, 0u}) put(di, v);
                            for (uint32_t v : {canControl ? 1u : 0u, 8u, 100000000u, 100000000u, 100000000u, 99000000u, 101000000u, 99000000u, 101000000u}) put(cs, v);
                            message(c, 0, seq++, di);
                            message(c, 1, seq++, cs);
                        } else if (type == 2 && n >= 8) {
                            const uint32_t id = get(pend.data() + 8), v = get(pend.data() + 12);
                            { std::lock_guard<std::mutex> lk(mu); sets.push_back({id, v}); }
                            if (id == 1) streaming = v != 0;
                            if (id == 100) fmt = v;
                        }
                        pend.erase(pend.begin(), pend.begin() + 8 + n);
                    }
                }
                if (streaming) {
                    std::vector<unsigned char> body;
                    for (int i = 0; i < 1024; i++) {
                        if (fmt == 2) { const int16_t re = 16384, im = -8192; body.push_back((unsigned char)re); body.push_back((unsigned char)(re >> 8)); body.push_back((unsigned char)im); body.push_back((unsigned char)((uint16_t)im >> 8)); }
                        else { body.push_back(200); body.push_back(50); }
                    }
                    if (!message(c, fmt == 2 ? 101 : 100, seq++, body)) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            sockShutdown(c);
            sockClose(c);
            dropNow = false;
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }
};

static bool readIq(IqRing& ring, std::vector<cf32>& buf) {
    size_t got = 0;
    for (int i = 0; i < 300 && got < buf.size(); i++) { got += ring.read(buf.data() + got, buf.size() - got); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return got == buf.size();
}

// SpyServer messages from hand-written bytes (no socket)
static void testSpyServerBytes() {
    // HELLO: type 0, body = protocol version 2.0.1700 (0x020006a4 little endian) and "OnAir"
    const std::vector<unsigned char> hello = {0xa4, 0x06, 0x00, 0x02, 'O', 'n', 'A', 'i', 'r'};
    const std::vector<unsigned char> want = {0, 0, 0, 0, 9, 0, 0, 0, 0xa4, 0x06, 0x00, 0x02, 'O', 'n', 'A', 'i', 'r'};
    CHECK(native::spyServerEncodeCommand(0, hello) == want);
    // SET_SETTING (2): setting 101 (IQ frequency) = 100 MHz = 0x05f5e100
    const std::vector<unsigned char> setf = {101, 0, 0, 0, 0x00, 0xe1, 0xf5, 0x05};
    const std::vector<unsigned char> wantF = {2, 0, 0, 0, 8, 0, 0, 0, 101, 0, 0, 0, 0x00, 0xe1, 0xf5, 0x05};
    CHECK(native::spyServerEncodeCommand(2, setf) == wantF);
    CHECK(native::spyServerEncodeCommand(2, {}).size() == 8);
    // a server header: version, type 100 (uint8 IQ) with flags 0x0001, stream type, sequence, body size 65536
    const unsigned char h[20] = {0xa4, 0x06, 0, 2, 100, 0, 1, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0, 0, 1, 0};
    uint32_t type = 0, flags = 0, size = 0;
    CHECK(native::spyServerDecodeHeader(h, type, flags, size));
    CHECK(type == 100 && flags == 1 && size == 65536);
    const unsigned char big[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0};   // 2 MB: refused
    CHECK(!native::spyServerDecodeHeader(big, type, flags, size));
    // DEVICE_INFO: Airspy HF+ (2), serial 7, max rate 768000, max bandwidth 600000, 6 decimation stages, 3 gain stages, max gain 3, ...
    const uint32_t words[12] = {2, 7, 768000, 600000, 6, 3, 3, 100000, 31000000, 16, 1, 0};
    unsigned char body[48];
    for (int i = 0; i < 12; i++) for (int k = 0; k < 4; k++) body[4 * i + k] = (unsigned char)(words[i] >> (8 * k));
    uint32_t dev = 0, rate = 0, dec = 0, gain = 0;
    CHECK(native::spyServerDecodeDeviceInfo(body, sizeof body, dev, rate, dec, gain));
    CHECK(dev == 2 && rate == 768000 && dec == 6 && gain == 3);
    CHECK(!native::spyServerDecodeDeviceInfo(body, 20, dev, rate, dec, gain));
}

// "Share my radio": the rtl_tcp server of OnAir against a raw socket and OnAir's own rtl_tcp client on localhost
static void testShare() {
    ShareServer& sh = ShareServer::get();
    ShareConfig c; c.on = true; c.port = 0; c.lan = false; c.allowControl = false;
    sh.configure(c);
    CHECK(sh.status().listening);
    const int port = sh.config().port;
    CHECK(port > 0);
    if (!sh.status().listening || port <= 0) { c.on = false; sh.configure(c); return; }

    // a raw client: the 12-byte greeting, then the samples exactly as offered
    netInit();
    std::string err;
    sock_t k = tcpConnect("127.0.0.1", port, 2000, err);
    CHECK(k != kBadSock);
    if (k == kBadSock) { c.on = false; sh.configure(c); return; }
    sockRecvTimeout(k, 2000);
    unsigned char hdr[12];
    CHECK(sockRecvAll(k, hdr, sizeof hdr));
    CHECK(memcmp(hdr, "RTL0", 4) == 0);
    for (int i = 0; i < 100 && sh.status().clients.empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(sh.status().clients.size() == 1);
    std::vector<cf32> iq(5000);
    for (size_t i = 0; i < iq.size(); i++) iq[i] = cf32(((int)(i % 200) + 28 - 127.4f) / 128.f, ((int)(i % 150) + 40 - 127.4f) / 128.f);
    sh.offer(iq.data(), iq.size());
    std::vector<unsigned char> got(iq.size() * 2);
    CHECK(sockRecvAll(k, got.data(), got.size()));
    bool same = true;
    for (size_t i = 0; i < iq.size(); i++) if (got[2 * i] != (i % 200) + 28 || got[2 * i + 1] != (i % 150) + 40) same = false;
    CHECK(same);
    // commands are not carried out while "allow remote control" is off
    const unsigned char setF[5] = {0x01, 0x05, 0xf5, 0xe1, 0x00};
    CHECK(sockSendAll(k, setF, 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(sh.takeCommands().empty());
    sockClose(k);

    // OnAir's own client: its frequency and gain commands arrive once control is allowed; the samples come back as sent
    c.port = port;
    c.allowControl = true;
    sh.configure(c);   // only the control flag changed: the server keeps running
    CHECK(sh.config().port == port);
    NetRadio nr; nr.type = NetRadio::RtlTcp; nr.host = "127.0.0.1"; nr.port = port;
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (!devs.empty()) {
        auto src = makeSource(devs[0]);
        IqRing ring(1u << 20);
        TuneSettings s; s.sampleRate = 2.048e6; s.centerHz = 100e6; s.gainDb = 30;
        err.clear();
        CHECK(src->start(s, ring, err));
        bool freq = false, gain = false;
        for (int i = 0; i < 200 && !(freq && gain); i++) {
            for (const ShareCommand& m : sh.takeCommands()) { if (m.cmd == 0x01 && m.value == 100000000u) freq = true; if (m.cmd == 0x04) gain = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(freq && gain);
        for (int round = 0; round < 20; round++) { sh.offer(iq.data(), iq.size()); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        std::vector<cf32> buf(4000);
        CHECK(readIq(ring, buf));
        bool okv = true;   // every pair is one of ours
        for (const cf32& v : buf) {
            const int ii = (int)std::lround(v.real() * 128.f + 127.4f), qq = (int)std::lround(v.imag() * 128.f + 127.4f);
            if (ii < 28 || ii >= 228 || qq < 40 || qq >= 190) okv = false;
        }
        CHECK(okv);
        src->stop();
    }
    c.on = false;
    sh.configure(c);
    CHECK(!sh.status().listening);
}

static void testSpyServer() {
    FakeSpy srv;
    CHECK(srv.start());
    if (!srv.port) return;
    NetRadio nr; nr.type = NetRadio::SpyServer; nr.host = "127.0.0.1"; nr.port = srv.port;
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return; }
    DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "spyserver" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port));
    CHECK(d.name == netRadioDeviceName(nr));
    // the server's decimation steps as the rates, its tuning range, its gain steps
    CHECK(d.rateRanges.size() == 9 && d.rateRanges.front().first == 10e6 / 256 && d.rateRanges.back().first == 10e6);
    CHECK(d.maxRateHz == 10e6 && d.minFreqHz == 24e6 && d.maxFreqHz == 1800e6 && d.gainMaxDb == 21);
    for (int i = 0; i < 100 && srv.connections < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int before = srv.connections;

    auto src = makeSource(d);
    CHECK(src != nullptr);
    if (!src) { srv.stop(); return; }
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 2.2e6;   // the 2.5 Msps step (decimation 2)
    s.centerHz = 100e6;
    s.gainDb = 10;
    err.clear();
    CHECK(src->start(s, ring, err));
    CHECK(srv.waitFor(0, 1));             // IQ only
    CHECK(srv.waitFor(100, 1));           // 8-bit, the default
    CHECK(srv.waitFor(102, 2));           // decimation
    CHECK(srv.waitFor(101, 100000000));   // frequency
    CHECK(srv.waitFor(2, 10));            // gain index
    CHECK(srv.waitFor(1, 1));             // streaming on
    CHECK(std::fabs(src->sampleRate() - 2.5e6) < 1);
    std::vector<cf32> buf(4096);
    CHECK(readIq(ring, buf));
    bool okVals = true;
    for (auto& v : buf) if (std::fabs(v.real() - 72 / 128.f) > 1e-5f || std::fabs(v.imag() + 78 / 128.f) > 1e-5f) okVals = false;
    CHECK(okVals);

    // live changes: 16-bit, another frequency and rate
    s.radio["format"] = "16";
    s.centerHz = 433.92e6; s.sampleRate = 10e6; s.gainDb = 30;   // a gain above the server's steps: its top one
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor(100, 2));
    CHECK(srv.waitFor(101, 433920000));
    CHECK(srv.waitFor(102, 0));
    CHECK(srv.waitFor(2, 21));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ring.clear();
    CHECK(readIq(ring, buf));
    okVals = true;
    for (size_t i = buf.size() / 2; i < buf.size(); i++) if (std::fabs(buf[i].real() - 0.5f) > 1e-5f || std::fabs(buf[i].imag() + 0.25f) > 1e-5f) okVals = false;
    CHECK(okVals);
    s.centerHz = 3e9;   // beyond the radio's specified range: sent as asked
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor(101, 3000000000u));

    // the server drops the connection: the source connects again and sends its settings again
    { std::lock_guard<std::mutex> lk(srv.mu); srv.sets.clear(); }
    srv.dropNow = true;
    bool back = false;
    for (int i = 0; i < 500 && !back; i++) { back = srv.connections >= before + 2; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(back);
    CHECK(srv.waitFor(100, 2));
    CHECK(srv.waitFor(1, 1));
    src->stop();

    // read only: the frequency stays within the band the server's radio receives, the gain is left alone, and the app is told
    srv.canControl = false;
    { std::lock_guard<std::mutex> lk(srv.mu); srv.sets.clear(); }
    auto ro = makeSource(d);
    s.radio.clear();
    s.centerHz = 200e6; s.sampleRate = 1e6;
    err.clear();
    CHECK(ro->start(s, ring, err));
    CHECK(err.find("read only") != std::string::npos);
    CHECK(srv.waitFor(101, 200000000));   // as asked: the read-only server decides, the note says so
    CHECK(srv.waitFor(1, 1));
    CHECK(!srv.hasId(2));
    s.centerHz = 99.5e6;
    err.clear();
    CHECK(ro->retune(s, err) && err.empty());
    CHECK(srv.waitFor(101, 99500000));
    ro->stop();
    srv.stop();

    // an rtl_tcp server is not a SpyServer
    FakeServer rtl;
    CHECK(rtl.start());
    NetRadio wrong; wrong.type = NetRadio::SpyServer; wrong.host = "127.0.0.1"; wrong.port = rtl.port;
    err.clear();
    CHECK(!probeNetRadio(wrong, 1000, err) && !err.empty());
    rtl.stop();
}

// A fake Hermes Lite 2 on a UDP port of localhost: answers discovery (board 6, gateware 73.2), streams EP6 packets to whoever sent start
// (the first 24-bit word of each sample -0.25, the second +0.5) and keeps the last value of each control register it was sent
struct FakeHl2 {
    sock_t s = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true}, streaming{false};
    std::atomic<int> starts{0}, stops{0};
    std::mutex mu;
    std::map<int, uint32_t> regs;
    unsigned char board = 6, gateware = 73;   // the discovery reply's board id (the Red Pitaya server's: 1) and gateware version
    unsigned char nr = 4;                     // the receivers its reply says it has (byte 0x13; the Red Pitaya server's says nothing: 0)
    int layoutNr = 1, lag = 0;                // the layout it sends: like a real HL2, four more packets of the old one after a change

    bool start() {
        netInit();
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kBadSock) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(s, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    bool has(int reg, uint32_t v) { std::lock_guard<std::mutex> lk(mu); auto it = regs.find(reg); return it != regs.end() && it->second == v; }
    bool waitFor(int reg, uint32_t v, int ms = 3000) {
        for (int i = 0; i < ms / 10; i++) { if (has(reg, v)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    }
    void loop() {
        sockaddr_in client{};
        uint32_t seq = 0;
        while (run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
            timeval tv{0, 1000};
            if (select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0) {
                unsigned char b[1500];
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const long long n = recvfrom(s, (char*)b, sizeof b, 0, (sockaddr*)&from, &fl);
                if (n >= 4 && b[0] == 0xEF && b[1] == 0xFE) {
                    if (b[2] == 0x02 && n == 63) {   // discovery
                        unsigned char r[60] = {0xEF, 0xFE, (unsigned char)(streaming ? 0x03 : 0x02), 0x00, 0x1C, 0xC0, 0xA2, 0x12, 0x34, gateware, board};
                        r[0x13] = nr;
                        r[0x15] = 2;
                        sockSendTo(s, r, sizeof r, from);
                    } else if (b[2] == 0x04) {
                        if (b[3] & 1) { client = from; streaming = true; starts++; } else { streaming = false; stops++; }
                    } else if (b[2] == 0x01 && b[3] == 0x02 && n == 1032) {
                        std::lock_guard<std::mutex> lk(mu);
                        for (int f = 0; f < 2; f++) {
                            const unsigned char* c = b + 8 + 512 * f;
                            if (c[0] != 0x7F || c[1] != 0x7F || c[2] != 0x7F) continue;
                            regs[c[3] >> 1] = (uint32_t)c[4] << 24 | (uint32_t)c[5] << 16 | (uint32_t)c[6] << 8 | c[7];
                        }
                    }
                }
            }
            if (streaming) {
                unsigned char p[1032] = {0xEF, 0xFE, 0x01, 0x06, (unsigned char)(seq >> 24), (unsigned char)(seq >> 16), (unsigned char)(seq >> 8), (unsigned char)seq};
                seq++;
                int want = 1;   // the receivers register 0x00 asks for (C4[6:3]): that many in every sample slot
                {
                    std::lock_guard<std::mutex> lk(mu);
                    auto it = regs.find(0x00);
                    if (it != regs.end()) want = (int)((it->second >> 3) & 0x0f) + 1;
                }
                if (want != layoutNr && lag == 0) lag = 4;
                if (lag > 0 && --lag == 0) layoutNr = want;
                const int nrx = layoutNr;
                const int slot = 6 * nrx + 2, per = 504 / slot;
                for (int f = 0; f < 2; f++) {
                    unsigned char* c = p + 8 + 512 * f;
                    c[0] = c[1] = c[2] = 0x7F;
                    for (int k = 0; k < per; k++) {
                        for (int rx = 0; rx < nrx; rx++) {   // Q -0.25, I 0.5 for RX1, 0.375 for RX2, 0.25 for RX3, ...
                            unsigned char* q = c + 8 + slot * k + 6 * rx;
                            const int32_t iv = (int32_t)((0.5 - 0.125 * rx) * 8388608.0);
                            q[0] = 0xE0; q[1] = 0; q[2] = 0;
                            q[3] = (unsigned char)(iv >> 16); q[4] = (unsigned char)(iv >> 8); q[5] = (unsigned char)iv;
                        }
                    }
                }
                sockSendTo(s, p, sizeof p, client);
            }
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (s != kBadSock) sockClose(s); }
};

// A Red Pitaya running the HPSDR-compatible SDR transceiver, as its server source answers: board id 1 (Hermes), gateware 32; port 1024
// is only replaced by the fake's own. Frame layout, registers and start/stop are the HL2's.
static void testRedPitaya() {
    FakeHl2 rp;
    rp.board = 1; rp.gateware = 32; rp.nr = 0;
    CHECK(rp.start());
    if (!rp.port) return;
    NetRadio nr; nr.type = NetRadio::RedPitaya; nr.host = "127.0.0.1"; nr.port = rp.port;
    std::string err;
    CHECK(probeNetRadio(nr, 1000, err));
    // a Hermes Lite 2 (board 6) is not a Red Pitaya, and the other way round
    FakeHl2 hl;
    CHECK(hl.start());
    if (hl.port) {
        NetRadio wrongRp = nr; wrongRp.port = hl.port;
        err.clear();
        CHECK(!probeNetRadio(wrongRp, 300, err) && !err.empty());
        NetRadio wrongHl; wrongHl.type = NetRadio::HermesLite2; wrongHl.host = "127.0.0.1"; wrongHl.port = rp.port;
        err.clear();
        CHECK(!probeNetRadio(wrongHl, 300, err) && !err.empty());
    }
    hl.stop();
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 3);   // its three receivers
    if (devs.empty()) { rp.stop(); return; }
    const DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "redpitaya" && d.nativeArgs == "127.0.0.1:" + std::to_string(rp.port));
    CHECK(d.name == netRadioDeviceName(nr) && d.name.find("Red Pitaya") == 0);
    CHECK(d.rateRanges.size() == 4 && d.minRateHz == 48000 && d.maxRateHz == 384000 && d.maxFreqHz == 61.44e6 && d.gainMinDb == 0 && d.gainMaxDb == 0);
    NetRadio dflt; dflt.type = NetRadio::RedPitaya; dflt.host = "192.168.1.60";
    CHECK(netRadioDeviceName(dflt) == "Red Pitaya RX1 192.168.1.60 (network)");
    CHECK(devs.size() == 3 && devs[2].name == "Red Pitaya RX3 127.0.0.1:" + std::to_string(rp.port) + " (network)" && devs[2].nativeArgs == d.nativeArgs + "/rx3");
    CHECK(netRadioDefaultPort(NetRadio::RedPitaya) == 1024 && std::string(netRadioTypeName(NetRadio::RedPitaya)) == "Red Pitaya");
    const auto back = netRadiosFromString(netRadiosToString({dflt, nr}));
    CHECK(back.size() == 2 && back[0] == dflt && back[1] == nr);

    auto src = makeSource(d);
    CHECK(src != nullptr);
    if (!src) { rp.stop(); return; }
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 100e3;   // the 192 ksps step
    s.centerHz = 14.074e6;
    s.gainDb = 20;          // ignored: it has no gain register here
    err.clear();
    CHECK(src->start(s, ring, err));
    CHECK(std::fabs(src->sampleRate() - 192e3) < 1);
    CHECK(rp.waitFor(0x00, 0x02000004));   // 192 ksps, no filter board bits, duplex, one receiver
    CHECK(rp.waitFor(0x02, 14074000));
    CHECK(rp.waitFor(0x01, 14074000));
    std::vector<cf32> buf(4096);
    CHECK(readIq(ring, buf));
    bool okVals = true;
    for (auto& v : buf) if (std::fabs(v.real() - 0.5f) > 1e-6f || std::fabs(v.imag() + 0.25f) > 1e-6f) okVals = false;
    CHECK(okVals);
    // live changes: up to 61.44 MHz, 384 ksps; the HL2's LNA register is never sent
    s.centerHz = 50.1e6; s.sampleRate = 384e3;
    CHECK(src->retune(s, err));
    CHECK(rp.waitFor(0x00, 0x03000004));
    CHECK(rp.waitFor(0x02, 50100000));
    CHECK(std::fabs(src->sampleRate() - 384e3) < 1);
    {
        std::lock_guard<std::mutex> lk(rp.mu);
        CHECK(rp.regs.find(0x0a) == rp.regs.end());
    }
    // the swap setting mirrors the spectrum: I and Q the other way round
    s.radio["swapiq"] = "1";
    CHECK(src->retune(s, err));
    for (int i = 0; i < 100; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    ring.clear();
    CHECK(readIq(ring, buf));
    okVals = true;
    for (auto& v : buf) if (std::fabs(v.real() + 0.25f) > 1e-6f || std::fabs(v.imag() - 0.5f) > 1e-6f) okVals = false;
    CHECK(okVals);
    s.centerHz = 70e6;   // beyond its specified range: said, and sent as asked
    err.clear();
    CHECK(src->retune(s, err) && !err.empty());
    CHECK(rp.waitFor(0x02, 70000000));
    src->stop();
    for (int i = 0; i < 100 && rp.stops < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(rp.stops >= 1 && !rp.streaming);
    rp.stop();
    for (const auto& f : discoverRedPitaya(200)) CHECK(!f.host.empty());
}

static void testHermesLite2() {
    FakeHl2 hl;
    CHECK(hl.start());
    if (!hl.port) return;
    NetRadio nr; nr.type = NetRadio::HermesLite2; nr.host = "127.0.0.1"; nr.port = hl.port;
    std::string err;
    CHECK(probeNetRadio(nr, 1000, err));
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 4);   // the receivers its discovery reply said it has
    if (devs.empty()) { hl.stop(); return; }
    const DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "hermeslite2" && d.nativeArgs == "127.0.0.1:" + std::to_string(hl.port));
    CHECK(d.name == netRadioDeviceName(nr));
    CHECK(d.rateRanges.size() == 4 && d.minRateHz == 48000 && d.maxRateHz == 384000 && d.gainMinDb == -12 && d.gainMaxDb == 48);
    NetRadio dflt; dflt.type = NetRadio::HermesLite2; dflt.host = "192.168.1.50";
    CHECK(netRadioDeviceName(dflt) == "Hermes Lite 2 RX1 192.168.1.50 (network)");   // the usual port is left out of the name

    auto src = makeSource(d);
    CHECK(src != nullptr);
    if (!src) { hl.stop(); return; }
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 150e3;   // the 192 ksps step
    s.centerHz = 7.074e6;
    s.gainDb = 20;
    err.clear();
    CHECK(src->start(s, ring, err));
    CHECK(std::fabs(src->sampleRate() - 192e3) < 1);
    CHECK(hl.waitFor(0x00, 0x02080004));   // 192 ksps, the 40 m filter, duplex, one receiver
    CHECK(hl.waitFor(0x02, 7074000));
    CHECK(hl.waitFor(0x01, 7074000));
    CHECK(hl.waitFor(0x0a, 0x40 | 32));    // +20 dB
    std::vector<cf32> buf(4096);
    CHECK(readIq(ring, buf));
    bool okVals = true;
    for (auto& v : buf) if (std::fabs(v.real() - 0.5f) > 1e-6f || std::fabs(v.imag() + 0.25f) > 1e-6f) okVals = false;
    CHECK(okVals);

    // live changes: frequency, rate, gain, no filter board
    s.centerHz = 14.2e6; s.sampleRate = 384e3; s.gainDb = 60; s.radio["filters"] = "0";
    CHECK(src->retune(s, err));
    CHECK(hl.waitFor(0x00, 0x03000004));
    CHECK(hl.waitFor(0x02, 14200000));
    CHECK(hl.waitFor(0x0a, 0x40 | 60));    // its top, +48 dB
    CHECK(std::fabs(src->sampleRate() - 384e3) < 1);
    s.centerHz = 100e6;                    // beyond its specified range: said, and sent as asked
    err.clear();
    CHECK(src->retune(s, err) && !err.empty());
    CHECK(hl.waitFor(0x02, 100000000));
    src->stop();
    for (int i = 0; i < 100 && hl.stops < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(hl.stops >= 1 && !hl.streaming);
    hl.stop();

    // nobody there (the fake's socket is closed now): no answer
    err.clear();
    CHECK(!probeNetRadio(nr, 300, err) && !err.empty());
    // discovery on the real networks: whatever answers has an address
    for (const auto& f : discoverHermesLite2(200)) CHECK(!f.host.empty());
}

// whether every sample in a fresh read of the ring is (I, -0.25); all: also every sample the ring holds already (nothing of another
// receiver, nothing of another layout got in)
static bool ringIs(IqRing& ring, float i, bool all = false) {
    std::vector<cf32> buf(2048);
    if (all) {
        std::vector<cf32> held(ring.available());
        held.resize(ring.read(held.data(), held.size()));
        for (auto& v : held) if (std::fabs(v.real() - i) > 1e-6f || std::fabs(v.imag() + 0.25f) > 1e-6f) return false;
    }
    ring.clear();
    if (!readIq(ring, buf)) return false;
    for (auto& v : buf) if (std::fabs(v.real() - i) > 1e-6f || std::fabs(v.imag() + 0.25f) > 1e-6f) return false;
    return true;
}

// Several receivers (slices) of one Hermes Lite 2: one stream, the receiver count of the highest one open, each its own NCO and samples
static void testHermesLite2Slices() {
    FakeHl2 hl;
    CHECK(hl.start());
    if (!hl.port) return;
    NetRadio nr; nr.type = NetRadio::HermesLite2; nr.host = "127.0.0.1"; nr.port = hl.port;
    std::string err;
    CHECK(probeNetRadio(nr, 1000, err));
    auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 4);
    if (devs.size() != 4) { hl.stop(); return; }
    const std::string hp = "127.0.0.1:" + std::to_string(hl.port);
    for (int k = 0; k < 4; k++) {
        CHECK(devs[k].name == "Hermes Lite 2 RX" + std::to_string(k + 1) + " " + hp + " (network)");
        CHECK(devs[k].nativeArgs == (k == 0 ? hp : hp + "/rx" + std::to_string(k + 1)) && devs[k].serial == devs[k].nativeArgs);
    }
    CHECK(deviceClaimKey(devs[0]) != deviceClaimKey(devs[1]));   // each one can go to its own OnAir receiver

    // RX2 alone: two receivers in the stream (RX1's NCO where RX2 is), RX2's samples
    auto rx2 = makeSource(devs[1]);
    IqRing r2(1u << 20);
    TuneSettings s2; s2.sampleRate = 150e3; s2.centerHz = 7.074e6; s2.gainDb = 20;
    CHECK(rx2->start(s2, r2, err));
    CHECK(std::fabs(rx2->sampleRate() - 192e3) < 1);
    CHECK(hl.waitFor(0x00, 0x02080004 | 1 << 3));   // 192 ksps, the 40 m filter, two receivers
    CHECK(hl.waitFor(0x03, 7074000));
    CHECK(hl.waitFor(0x02, 7074000));
    CHECK(ringIs(r2, 0.375f, true));   // the fake's first packets have its old layout (one receiver): not taken
    const int starts = hl.starts;

    // RX1 joins: the rate stays the one RX2 set (said when it is lower than asked), the filter follows the highest frequency
    auto rx1 = makeSource(devs[0]);
    IqRing r1(1u << 20);
    TuneSettings s1; s1.sampleRate = 384e3; s1.centerHz = 14.2e6; s1.gainDb = 20;
    err.clear();
    CHECK(rx1->start(s1, r1, err) && err.find("share one sample rate") != std::string::npos);
    CHECK(std::fabs(rx1->sampleRate() - 192e3) < 1);
    CHECK(hl.waitFor(0x00, 0x02100004 | 1 << 3));   // the 20 m filter
    CHECK(hl.waitFor(0x02, 14200000) && hl.waitFor(0x01, 14200000) && hl.waitFor(0x03, 7074000));
    CHECK(hl.starts == starts);   // the receiver count did not change: no restart
    CHECK(ringIs(r1, 0.5f) && ringIs(r2, 0.375f));

    // RX4 joins: four receivers, while it streams (no restart); the packets of the old layout are not taken by anyone
    auto rx4 = makeSource(devs[3]);
    IqRing r4(1u << 20);
    TuneSettings s4 = s2; s4.centerHz = 3.573e6;
    err.clear();
    CHECK(rx4->start(s4, r4, err));
    CHECK(hl.waitFor(0x00, 0x02100004 | 3 << 3));
    CHECK(hl.starts == starts);
    CHECK(hl.waitFor(0x05, 3573000) && hl.waitFor(0x04, 14200000));   // RX3, not open: where RX1 is
    CHECK(ringIs(r4, 0.125f, true) && ringIs(r1, 0.5f, true) && ringIs(r2, 0.375f, true));
    // a rate change of one receiver while others are open is not made
    s4.sampleRate = 384e3;
    err.clear();
    CHECK(rx4->retune(s4, err) && !err.empty());
    CHECK(std::fabs(rx4->sampleRate() - 192e3) < 1);
    // RX1's own frequency
    s1.centerHz = 10.1e6;
    CHECK(rx1->retune(s1, err));
    CHECK(hl.waitFor(0x02, 10100000) && hl.waitFor(0x03, 7074000));

    // RX4 goes: back to two receivers; then RX1 and RX2: the stream stops
    rx4->stop();
    CHECK(hl.waitFor(0x00, 0x02100004 | 1 << 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ringIs(r1, 0.5f, true) && ringIs(r2, 0.375f, true));
    CHECK(hl.starts == starts);
    rx1->stop();
    CHECK(hl.streaming);
    rx2->stop();
    for (int i = 0; i < 100 && hl.streaming; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!hl.streaming);

    // the last one alone sets the rate again
    auto again = makeSource(devs[1]);
    s2.sampleRate = 384e3;
    CHECK(again->start(s2, r2, err));
    CHECK(std::fabs(again->sampleRate() - 384e3) < 1);
    again->stop();

    // a receiver the radio does not have: said, and not tried again
    DeviceInfo rx5 = devs[3];
    rx5.nativeArgs = hp + "/rx5";
    auto five = makeSource(rx5);
    err.clear();
    CHECK(!five->start(s2, r2, err) && err.find("4 receivers") != std::string::npos);
    // a radio that says it has two: two entries
    FakeHl2 two;
    two.nr = 2;
    CHECK(two.start());
    NetRadio n2 = nr; n2.port = two.port;
    CHECK(probeNetRadio(n2, 1000, err));
    CHECK(netRadioDevices({n2}).size() == 2);
    two.stop();
    hl.stop();
}

static void testPluto() {
    NetRadio p; p.type = NetRadio::Pluto; p.host = "10.1.2.3";
    auto v = netRadioDevices({p});
    CHECK(v.size() == 1);
    if (v.size() == 1) {
        CHECK(v[0].kind == DeviceInfo::Native && v[0].board == "pluto");
        CHECK(v[0].nativeArgs == "ip:10.1.2.3");
        CHECK(v[0].steadyRateHz == 0);            // a real network: the converter's full rate is offered
        CHECK(v[0].maxRateHz > 60e6);
    }
    p.host = "ip:192.168.2.1";                    // typed by hand: a Pluto+ on Ethernet often has the USB-gadget address too (#54), so no USB limit
    v = netRadioDevices({p});
    CHECK(v.size() == 1 && v[0].nativeArgs == "ip:192.168.2.1" && v[0].steadyRateHz == 0);
    p.host = "pluto.lan"; p.port = 30432;
    v = netRadioDevices({p});
    CHECK(v.size() == 1 && v[0].nativeArgs == "ip:pluto.lan:30432");
    // registered radios are what listRadios() appends
    setNetworkRadios({p});
    CHECK(networkRadios().size() == 1 && networkRadios()[0] == p);
    setNetworkRadios({});
}

// A fake SoapyRemote server: a sound card first, then a Mirics stick (the case of issue #84)
static std::vector<std::map<std::string, std::string>> fakeEnumerate(const std::string&) {
    return {{{"driver", "remote"}, {"remote:driver", "audio"}, {"remote:label", "HDA Intel"}},
            {{"driver", "remote"}, {"remote:driver", "mirisdr"}, {"remote:label", "Mirics MSi2500"}, {"remote:serial", "1"}}};
}
static void testSoapyRemote() {
    NetRadio r; r.type = NetRadio::SoapyRemote; r.host = "tcp://192.168.0.13"; r.port = 55133;
    CHECK(soapyRemoteArgs(r) == "driver=remote,remote=tcp://192.168.0.13:55133,remote:timeout=5000000");
    r.remote = "mirisdr";
    CHECK(soapyRemoteArgs(r) == "driver=remote,remote=tcp://192.168.0.13:55133,remote:timeout=5000000,remote:driver=mirisdr");
    r.remote = "driver=sdrplay, serial=7";
    CHECK(soapyRemoteArgs(r) == "driver=remote,remote=tcp://192.168.0.13:55133,remote:timeout=5000000,remote:driver=sdrplay,remote:serial=7");
    const auto back = netRadiosFromString(netRadiosToString({r}));
    CHECK(back.size() == 1 && back[0].remote == "driver=sdrplay, serial=7" && back[0].port == 55133);
    NetRadio plain = r; plain.remote.clear();
    CHECK(!(plain == r));
    const auto kw = fakeEnumerate("");
    const auto off = soapyRemoteOffers(kw);
    CHECK(off.size() == 1 && off[0].driver == "mirisdr" && off[0].serial == "1" && off[0].label == "Mirics MSi2500");
    CHECK(soapyRemoteOffers(kw, true).size() == 2);
    setSoapyEnumerator(fakeEnumerate);
    const auto l = soapyRemoteList(plain);
    CHECK(l.size() == 1 && l[0].driver == "mirisdr");
    const std::string help = soapyRemoteFailureHelp(soapyRemoteArgs(plain));
    CHECK(help.find("mirisdr") != std::string::npos && help.find("audio") != std::string::npos && help.find("Remote driver") != std::string::npos);
    CHECK(soapyRemoteFailureHelp("driver=rtlsdr").empty());
#ifdef DECT2_HAVE_SOAPY
    const auto v = netRadioDevices({plain});
    CHECK(v.size() == 1 && v[0].soapyArgs == "driver=remote,remote=tcp://192.168.0.13:55133,remote:timeout=5000000,remote:driver=mirisdr,remote:serial=1");
    CHECK(v[0].name.find("mirisdr") != std::string::npos && v[0].name.find("192.168.0.13:55133") != std::string::npos);
    const auto v2 = netRadioDevices({r});
    CHECK(v2.size() == 1 && v2[0].soapyArgs == soapyRemoteArgs(r));
#endif
    setSoapyEnumerator(nullptr);
    // the client module missing: the help says where to get it (also from the probe, before any connection is tried)
    setSoapyRemoteModuleOverride(0);
    CHECK(!soapyRemoteModuleAvailable());
    CHECK(std::string(soapyRemoteModuleHelp()).find("SoapyRemote") != std::string::npos && std::string(soapyRemoteModuleHelp()).find("install") != std::string::npos);
    // the message is chosen for the platform: a damaged install on Windows/macOS, an apt package on Linux; the server side is always named
    for (int pf = 0; pf < 3; ++pf) CHECK(std::string(soapyRemoteModuleHelpFor(pf)).find("SoapySDRServer --bind") != std::string::npos);
    CHECK(std::string(soapyRemoteModuleHelpFor(0)).find("missing from this installation") != std::string::npos && std::string(soapyRemoteModuleHelpFor(0)).find("apt") == std::string::npos);
    CHECK(std::string(soapyRemoteModuleHelpFor(1)).find("missing from this installation") != std::string::npos);
    CHECK(std::string(soapyRemoteModuleHelpFor(2)).find("apt install soapysdr-module-remote") != std::string::npos);
    CHECK(std::string(soapyRemoteModuleHelpFor(-1)) == soapyRemoteModuleHelp());
    CHECK(soapyRemoteFailureHelp(soapyRemoteArgs(plain)) == soapyRemoteModuleHelp());
    std::string perr;
    CHECK(!probeNetRadio(plain, 300, perr) && perr == soapyRemoteModuleHelp());
    setSoapyRemoteModuleOverride(1);
    CHECK(soapyRemoteModuleAvailable());
    setSoapyRemoteModuleOverride(-1);
}

static void testPrefs() {
    NetRadio a; a.type = NetRadio::Pluto; a.host = "192.168.2.1";
    NetRadio b; b.type = NetRadio::RtlTcp; b.host = "raspberrypi.local"; b.port = 1234;
    NetRadio c; c.type = NetRadio::SoapyRemote; c.host = "10.0.0.7"; c.port = 55132;
    NetRadio e; e.type = NetRadio::SpyServer; e.host = "spy.lan"; e.port = 5555;
    NetRadio h; h.type = NetRadio::HermesLite2; h.host = "192.168.1.50"; h.port = 1024;
    const std::string s = netRadiosToString({a, b, c, e, h});
    const auto back = netRadiosFromString(s);
    CHECK(back.size() == 5 && back[0] == a && back[1] == b && back[2] == c && back[3] == e && back[4] == h);
    CHECK(netRadioDefaultPort(NetRadio::SpyServer) == 5555 && netRadioDefaultPort(NetRadio::HermesLite2) == 1024);
    CHECK(netRadiosFromString("junk;rtltcp,,1;bogus,h,1").empty());
    CHECK(netRadioDefaultPort(NetRadio::RtlTcp) == 1234 && netRadioDefaultPort(NetRadio::SoapyRemote) == 55132 && netRadioDefaultPort(NetRadio::Pluto) == 0);
#ifdef DECT2_HAVE_SOAPY
    setSoapyEnumerator([](const std::string&) { return std::vector<std::map<std::string, std::string>>(); });   // a server that lists nothing: the plain entry
    const auto v = netRadioDevices({c});
    CHECK(v.size() == 1 && v[0].kind == DeviceInfo::Soapy && v[0].soapyArgs == "driver=remote,remote=tcp://10.0.0.7:55132,remote:timeout=5000000");
    setSoapyEnumerator(nullptr);
#else
    CHECK(netRadioDevices({c}).empty());
#endif
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // the fake server writes to clients that went away
#endif
    testPrefs();
    testSoapyRemote();
    testPluto();
    testRtlTcp();
    testRtlTcpOther();
    testSpyServer();
    testSpyServerBytes();
    testShare();
    testHermesLite2();
    testHermesLite2Slices();
    testRedPitaya();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("netradio: all passed\n");
    return 0;
}
