// rtl_tcp source against a fake server that sends the stream in odd, random-sized TCP chunks (1 to 4099 bytes) after a greeting dribbled one
// byte at a time: no IQ pair may be split, swapped or lost (byte i of the stream is i mod 251, so sample k must be (2k mod 251, 2k+1 mod 251)).
// Also a greeting with an unknown tuner type and a server that sends a partial greeting then closes (a clear error, no hang).
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <csignal>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Srv {
    sock_t ls = kBadSock; int port = 0; std::thread th; std::atomic<bool> run{true};
    int mode = 0;   // 0 odd chunks, 1 partial greeting then close
    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 2) != 0) return false;
        socklen_t len = sizeof a; getsockname(ls, (sockaddr*)&a, &len); port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        std::mt19937 rng(7);
        while (run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd); timeval tv{0, 100000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) continue;
            const unsigned char hdr[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
            if (mode == 1) { sockSendAll(c, hdr, 7); sockShutdown(c); sockClose(c); continue; }
            for (int i = 0; i < 12; i++) { sockSendAll(c, hdr + i, 1); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            uint64_t pos = 0;
            std::vector<unsigned char> b;
            while (run) {
                const size_t n = 1 + rng() % 4099;
                b.resize(n);
                for (size_t i = 0; i < n; i++) b[i] = (unsigned char)((pos + i) % 251);
                if (!sockSendAll(c, b.data(), n)) break;
                pos += n;
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            sockShutdown(c); sockClose(c);
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }
};

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    Srv srv;
    CHECK(srv.start());
    DeviceInfo d;
    d.kind = DeviceInfo::Native; d.board = "rtltcp"; d.nativeArgs = "127.0.0.1:" + std::to_string(srv.port); d.serial = d.nativeArgs;
    {
        auto src = makeSource(d);
        CHECK(src != nullptr);
        IqRing ring(1u << 22);
        TuneSettings s; s.sampleRate = 2.048e6; s.centerHz = 100e6; s.gainDb = 30;
        std::string err;
        CHECK(src->start(s, ring, err));
        std::vector<cf32> all;
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 400 && all.size() < 300000; i++) {
            const size_t g = ring.read(buf.data(), buf.size());
            all.insert(all.end(), buf.begin(), buf.begin() + g);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        src->stop();
        CHECK(all.size() > 100000);
        // the stream must be one unbroken byte sequence: sample k+1 follows sample k by 2 in the ramp (mod 251)
        const auto val = [](float x) { return (int)std::lround(x * 128.f + 127.4f); };
        size_t bad = 0;
        for (size_t k = 1; k < all.size(); k++) {
            const int i0 = val(all[k - 1].real()), q0 = val(all[k - 1].imag()), i1 = val(all[k].real()), q1 = val(all[k].imag());
            if (q0 != (i0 + 1) % 251 || i1 != (i0 + 2) % 251 || q1 != (i0 + 3) % 251) bad++;
        }
        fprintf(stderr, "samples %zu, broken pairs %zu\n", all.size(), bad);
        CHECK(bad == 0);
    }
    srv.mode = 1;
    {
        auto src = makeSource(d);
        IqRing ring(1u << 16);
        TuneSettings s; s.sampleRate = 2.048e6; s.centerHz = 100e6; s.gainDb = 30;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!src->start(s, ring, err));
        CHECK(!err.empty());
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15));
        src->stop();
    }
    srv.stop();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("rtltcp chunks: all passed\n");
    return 0;
}
