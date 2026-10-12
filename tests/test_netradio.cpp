// Network radios (netradio.h): the rtl_tcp, SpyServer and Hermes Lite 2 clients against fake servers on localhost, a user-added Pluto
// address, the saved list.
#include "../core/src/tcp_client.h"
#include "dect2/netradio.h"
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
    s.centerHz = 3e9;   // beyond the radio's range: its top
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor(101, 1800000000));

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
    CHECK(srv.waitFor(101, 101000000));
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
                        unsigned char r[60] = {0xEF, 0xFE, (unsigned char)(streaming ? 0x03 : 0x02), 0x00, 0x1C, 0xC0, 0xA2, 0x12, 0x34, 73, 6};
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
                for (int f = 0; f < 2; f++) {
                    unsigned char* c = p + 8 + 512 * f;
                    c[0] = c[1] = c[2] = 0x7F;
                    for (int k = 0; k < 63; k++) {
                        unsigned char* q = c + 8 + 8 * k;
                        q[0] = 0xE0; q[1] = 0; q[2] = 0;   // -0.25
                        q[3] = 0x40; q[4] = 0; q[5] = 0;   // +0.5
                    }
                }
                sockSendTo(s, p, sizeof p, client);
            }
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (s != kBadSock) sockClose(s); }
};

static void testHermesLite2() {
    FakeHl2 hl;
    CHECK(hl.start());
    if (!hl.port) return;
    NetRadio nr; nr.type = NetRadio::HermesLite2; nr.host = "127.0.0.1"; nr.port = hl.port;
    std::string err;
    CHECK(probeNetRadio(nr, 1000, err));
    const auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { hl.stop(); return; }
    const DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "hermeslite2" && d.nativeArgs == "127.0.0.1:" + std::to_string(hl.port));
    CHECK(d.name == netRadioDeviceName(nr));
    CHECK(d.rateRanges.size() == 4 && d.minRateHz == 48000 && d.maxRateHz == 384000 && d.gainMinDb == -12 && d.gainMaxDb == 48);
    NetRadio dflt; dflt.type = NetRadio::HermesLite2; dflt.host = "192.168.1.50";
    CHECK(netRadioDeviceName(dflt) == "Hermes Lite 2 192.168.1.50 (network)");   // the usual port is left out of the name

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
    s.centerHz = 100e6;                    // beyond its range: said, and its top
    err.clear();
    CHECK(src->retune(s, err) && !err.empty());
    CHECK(hl.waitFor(0x02, 38400000));
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
    const auto v = netRadioDevices({c});
    CHECK(v.size() == 1 && v[0].kind == DeviceInfo::Soapy && v[0].soapyArgs == "driver=remote,remote=tcp://10.0.0.7:55132");
#else
    CHECK(netRadioDevices({c}).empty());
#endif
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // the fake server writes to clients that went away
#endif
    testPrefs();
    testPluto();
    testRtlTcp();
    testRtlTcpOther();
    testSpyServer();
    testHermesLite2();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("netradio: all passed\n");
    return 0;
}
