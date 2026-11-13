// Throughput of the SDRplay nRSP-ST source (source_nrspst.cpp: SDRconnect's WebSocket IQ, ws_client.h) into the input ring, against a fake
// SDRconnect on localhost. A Discord report: other programs run the nRSP-ST's "Full IQ" network mode at 0-10 Msps, OnAir stalled above
// about 5 Msps. Here the fake sends 16-bit IQ in messages of 1024, 4096, 16384 and 262144 samples (the last is over the 1 MiB that 3.9 took):
//   - as fast as the source takes them: the rate the WebSocket parser, the conversion and the ring sustain (must be well above 10 Msps)
//   - paced at 10 Msps for some seconds, with a consumer reading the ring as the engine does: nothing dropped, every sample in order
// The sample values are a counter, so a lost, repeated or reordered sample is found. Simulated server only: no real nRSP-ST was available.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/ws_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

using namespace dect2;
using Clock = std::chrono::steady_clock;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FastFake {
    sock_t ls = kBadSock;
    int port = 0;
    std::thread acceptTh, sendTh;
    std::atomic<bool> run{true}, streaming{false};
    std::mutex sendMu;
    size_t perMsg = 4096;
    double rate = 0;          // 0: as fast as the client takes it
    std::atomic<uint64_t> sent{0};
    sock_t c = kBadSock;

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
        acceptTh = std::thread([this] { serve(); });
        return true;
    }
    void stop() {
        run = false;
        if (c != kBadSock) sockShutdown(c);
        sockShutdown(ls);
        sockClose(ls);
        if (acceptTh.joinable()) acceptTh.join();
        if (sendTh.joinable()) sendTh.join();
        if (c != kBadSock) sockClose(c);
    }
    bool sendFrame(int op, const void* p, size_t n) {
        unsigned char h[10];
        size_t hl = 2;
        h[0] = (unsigned char)(0x80 | op);
        if (n < 126) h[1] = (unsigned char)n;
        else if (n < 65536) { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hl = 4; }
        else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (unsigned char)((uint64_t)n >> (8 * (7 - i))); hl = 10; }
        std::lock_guard<std::mutex> lk(sendMu);
        return sockSendAll(c, h, hl) && sockSendAll(c, p, n);
    }
    bool rd(void* d, size_t n) {
        char* p = (char*)d;
        size_t got = 0;
        while (got < n) {
            const long long k = sockRecv(c, p + got, n - got);
            if (k <= 0) return false;
            got += (size_t)k;
        }
        return true;
    }
    void serve() {
        c = accept(ls, nullptr, nullptr);
        if (c == kBadSock) return;
        std::string head;
        char ch;
        while (head.size() < 4096) {
            if (sockRecv(c, &ch, 1) <= 0) return;
            head += ch;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        const size_t k = head.find("Sec-WebSocket-Key: ");
        if (k == std::string::npos) return;
        const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
        const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
        sockSendAll(c, r.data(), r.size());
        const std::map<std::string, std::string> props = {
            {"api_version", "1.0.3"}, {"valid_devices", "nRSP-ST 1234ABCD"}, {"active_device", "nRSP-ST 1234ABCD"}, {"valid_antennas", "Antenna A"},
            {"active_antenna", "Antenna A"}, {"lna_state_min", "0"}, {"lna_state_max", "9"}, {"can_control", "true"}, {"device_sample_rate", "10000000"}};
        while (run) {   // the client's messages: answers, and the stream starts on iq_stream_enable
            unsigned char h[2];
            if (!rd(h, 2)) return;
            size_t len = h[1] & 127;
            if (len == 126) { unsigned char x[2]; if (!rd(x, 2)) return; len = (size_t)x[0] << 8 | x[1]; }
            unsigned char m[4];
            if (!rd(m, 4)) return;
            std::string p(len, 0);
            if (len && !rd(&p[0], len)) return;
            for (size_t i = 0; i < len; i++) p[i] ^= (char)m[i & 3];
            if ((h[0] & 15) == 8) return;
            std::map<std::string, std::string> kv;
            if (!native::nrspParseMessage(p, kv)) continue;
            const std::string ev = kv["event_type"], pr = kv["property"], va = kv["value"];
            if (ev == "get_property" && props.count(pr)) { const std::string a = native::nrspMessage("get_property_response", pr, props.at(pr)); sendFrame(1, a.data(), a.size()); }
            if (ev == "iq_stream_enable" && va == "true" && !streaming.exchange(true)) sendTh = std::thread([this] { blast(); });
        }
    }
    void blast() {
        std::vector<unsigned char> f(2 + 4 * perMsg);
        f[0] = 2; f[1] = 0;
        uint64_t k = 0;
        const auto t0 = Clock::now();
        while (run) {
            if (rate > 0) {
                const double due = std::chrono::duration<double>(Clock::now() - t0).count() * rate;
                if ((double)k >= due) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
            }
            unsigned char* p = f.data() + 2;
            for (size_t i = 0; i < perMsg; i++, k++) {
                const int16_t re = (int16_t)(k & 0x7FFF), im = (int16_t)((k >> 15) & 0x7FFF);
                p[4 * i] = (unsigned char)(re & 255); p[4 * i + 1] = (unsigned char)((uint16_t)re >> 8);
                p[4 * i + 2] = (unsigned char)(im & 255); p[4 * i + 3] = (unsigned char)((uint16_t)im >> 8);
            }
            if (!sendFrame(2, f.data(), f.size())) return;
            sent += perMsg;
        }
    }
};

struct Taken { uint64_t samples = 0, bad = 0; double sec = 0; uint64_t dropped = 0, offered0 = 0, offered1 = 0; };

// start the source against the fake, read the ring like the engine's sample thread for `secs` after the first sample, stop
static Taken runOnce(size_t perMsg, double rate, double secs) {
    Taken t;
    FastFake srv;
    srv.perMsg = perMsg;
    srv.rate = rate;
    if (!srv.start()) { CHECK(false, "fake server did not start"); return t; }
    NetRadio nr;
    nr.type = NetRadio::NrspSt;
    nr.host = "127.0.0.1";
    nr.port = srv.port;
    const auto devs = netRadioDevices({nr});
    if (devs.empty()) { CHECK(false, "no device"); srv.stop(); return t; }
    auto src = native::makeNrspSt(devs[0]);
    IqRing ring(1u << 24);   // the engine's ring
    TuneSettings ts;
    ts.sampleRate = 10e6; ts.centerHz = 100e6; ts.rateChosen = true;
    std::string err;
    if (!src->start(ts, ring, err)) { CHECK(false, "start: %s", err.c_str()); srv.stop(); return t; }
    std::vector<cf32> buf(1 << 16);
    uint64_t expect = 0;
    bool first = true;
    Clock::time_point t0 = Clock::now(), giveUp = t0 + std::chrono::seconds(15);
    for (;;) {
        const size_t n = ring.read(buf.data(), buf.size());
        if (!n) {
            if (Clock::now() > giveUp) break;
            ring.waitData(50000, std::chrono::microseconds(5000));
            continue;
        }
        if (first) {
            first = false; t0 = Clock::now(); giveUp = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(secs));
            int64_t ns; size_t blk;
            ring.lastWrite(t.offered0, ns, blk);
        }
        if (rate <= 0) { t.samples += n; if (Clock::now() > giveUp) break; continue; }   // as fast as it goes: the source's rate, not this check's
        for (size_t i = 0; i < n; i++, expect++) {
            const long re = std::lround(buf[i].real() * 32768.f), im = std::lround(buf[i].imag() * 32768.f);
            if ((uint64_t)re != (expect & 0x7FFF) || (uint64_t)im != ((expect >> 15) & 0x7FFF)) { t.bad++; expect = (uint64_t)re | (uint64_t)im << 15; }
        }
        t.samples += n;
        if (Clock::now() > giveUp) break;
    }
    t.sec = std::chrono::duration<double>(Clock::now() - t0).count();
    t.dropped = ring.dropped();
    { int64_t ns; size_t blk; ring.lastWrite(t.offered1, ns, blk); }
    src->stop();
    srv.stop();
    return t;
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    const double secs = getenv("NRSP_RATE_SECONDS") ? atof(getenv("NRSP_RATE_SECONDS")) : 3.0;
    printf("nRSP-ST source into the ring (fake SDRconnect on localhost, 16-bit IQ):\n");
    // 262144 samples is a message of 1 MiB and 2 bytes: refused as bad data up to 3.9 (a server that sends ~40 messages a second at 10 Msps)
    for (size_t perMsg : {1024u, 4096u, 16384u, 262144u}) {
        // as fast as it goes: what the source writes into the ring (offered: written and, when the reader here falls behind, dropped)
        const Taken a = runOnce(perMsg, 0, secs);
        const double msps = a.sec > 0 ? (double)(a.offered1 - a.offered0) / a.sec / 1e6 : 0;
        printf("  %6zu samples a message, as fast as it goes: %6.1f Msps (%.0f MB/s) into the ring\n", perMsg, msps, msps * 4);
        CHECK(msps >= 15.0, "%zu a message: only %.1f Msps, needs 10 Msps with headroom", perMsg, msps);
        const Taken p = runOnce(perMsg, 10e6, secs);
        const double pm = p.sec > 0 ? p.samples / p.sec / 1e6 : 0;
        printf("  %6zu samples a message, paced at 10 Msps:    %6.2f Msps, %llu out of order, %llu dropped\n", perMsg, pm, (unsigned long long)p.bad, (unsigned long long)p.dropped);
        CHECK(pm >= 9.7 && pm <= 10.3 && p.bad == 0 && p.dropped == 0, "%zu a message at 10 Msps: %.2f Msps, %llu out of order, %llu dropped", perMsg, pm,
              (unsigned long long)p.bad, (unsigned long long)p.dropped);
    }
    printf(fails ? "nrspst rate: FAILED\n" : "nrspst rate: ok\n");
    return fails ? 1 : 0;
}
