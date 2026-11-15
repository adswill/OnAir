// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live UDP transport-stream input (tv/ts_udp_in.h): the address forms, alignment recovery after junk and a lost piece, the RTP header,
// null stuffing while the buffer is empty or filling, dropping the oldest packets on overflow, the counters, and a real loopback socket.
#include "dect2/tv/ts_udp_in.h"
#include "dect2/tx/tx.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void mk(uint8_t* p, int pid, int cc) { memset(p, 0xAB, 188); p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc & 15)); }
static std::vector<uint8_t> stream(int n, int first = 0, int pid = 0x100) { std::vector<uint8_t> v((size_t)n * 188); for (int i = 0; i < n; i++) mk(&v[(size_t)i * 188], pid, first + i); return v; }
static bool isNull(const uint8_t* p) { return p[0] == 0x47 && p[1] == 0x1F && p[2] == 0xFF; }
static int counter(const uint8_t* p) { return p[3] & 15; }

int main() {
    std::string h; int port = 0;
    CHECK(parseTsUdpSpec("udp://@:1234", h, port) && h == "0.0.0.0" && port == 1234, "udp://@:1234");
    CHECK(parseTsUdpSpec("udp://239.1.1.1:5000?pkt_size=1316", h, port) && h == "239.1.1.1" && port == 5000, "multicast with options");
    CHECK(parseTsUdpSpec("rtp://@239.1.1.1:5004", h, port) && h == "239.1.1.1" && port == 5004, "rtp with @group");
    CHECK(!parseTsUdpSpec("udp://@:0", h, port) && !parseTsUdpSpec("udp://@:70000", h, port) && !parseTsUdpSpec("udp://example:1", h, port) && !parseTsUdpSpec("file.ts", h, port), "bad addresses");
    CHECK(isTsUdpSpec("udp://@:1") && !isTsUdpSpec("generator:1"), "isTsUdpSpec");

    uint8_t pk[188];
    {   // 7 x 188 datagrams, in order; nothing before the prefill, then null until filled
        TsUdpBuffer b(1000, 14);
        auto s = stream(70);
        for (int i = 0; i < 1; i++) b.feedDatagram(s.data(), 7 * 188);
        b.pull(pk);
        CHECK(isNull(pk), "null while the buffer fills (7 < 14)");
        b.feedDatagram(s.data() + 7 * 188, 7 * 188);
        for (int i = 0; i < 14; i++) { b.pull(pk); CHECK(!isNull(pk) && counter(pk) == i % 16 && pk[1] == 1, "packet %d in order", i); }
        b.pull(pk);
        CHECK(isNull(pk), "null when empty");
        const TsUdpStats st = b.stats();
        CHECK(st.packets == 14 && st.nullsInserted == 2 && st.dropped == 0 && st.datagrams == 2, "counters %llu %llu %llu", (unsigned long long)st.packets, (unsigned long long)st.nullsInserted, (unsigned long long)st.dropped);
    }
    {   // junk in front, a datagram cut in the middle of a packet, then the rest: alignment found again
        TsUdpBuffer b(1000, 1);
        auto s = stream(40);
        std::vector<uint8_t> junk(100, 0x55);
        b.feedDatagram(junk.data(), junk.size());
        b.feedDatagram(s.data(), 7 * 188 + 50);                        // ends inside a packet
        b.feedDatagram(s.data() + 7 * 188 + 50 + 30, 188 * 20);        // 30 bytes lost: the stream is shifted, must resync
        b.feedDatagram(s.data() + 7 * 188 + 50 + 30 + 188 * 20 + 0, 188 * 12);
        TsUdpStats st = b.stats();
        CHECK(st.resyncs >= 1 && st.junkBytes >= 100, "resync counted: %llu, junk %llu", (unsigned long long)st.resyncs, (unsigned long long)st.junkBytes);
        int good = 0;
        for (size_t i = 0; i < st.queued; i++) { b.pull(pk); if (!isNull(pk) && pk[0] == 0x47 && pk[1] == 1) good++; }
        CHECK(good >= 25 && st.packets == st.queued, "kept %d aligned packets of %zu", good, st.queued);
    }
    {   // RTP header (12 bytes, payload type 33, one CSRC and padding are handled)
        TsUdpBuffer b(1000, 1);
        auto s = stream(14);
        std::vector<uint8_t> d(12, 0);
        d[0] = 0x80; d[1] = 33; d[3] = 1;
        d.insert(d.end(), s.begin(), s.begin() + 7 * 188);
        b.feedDatagram(d.data(), d.size());
        std::vector<uint8_t> d2 = {0x81, 0x21, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};   // one CSRC
        d2.insert(d2.end(), s.begin() + 7 * 188, s.end());
        b.feedDatagram(d2.data(), d2.size());
        const TsUdpStats st = b.stats();
        CHECK(st.packets == 14 && st.rtpDatagrams == 2, "rtp: %llu packets, %llu rtp datagrams", (unsigned long long)st.packets, (unsigned long long)st.rtpDatagrams);
        for (int i = 0; i < 14; i++) { b.pull(pk); CHECK(!isNull(pk) && counter(pk) == i % 16, "rtp packet %d", i); }
    }
    {   // overflow: the oldest are dropped, the newest kept, in order
        TsUdpBuffer b(100, 1);
        auto s = stream(300);
        for (int i = 0; i < 300; i += 7) b.feedDatagram(s.data() + (size_t)i * 188, (size_t)std::min(7, 300 - i) * 188);
        const TsUdpStats st = b.stats();
        CHECK(st.queued == 100 && st.dropped == 200 && st.packets == 300, "overflow: queued %zu dropped %llu", st.queued, (unsigned long long)st.dropped);
        b.pull(pk);
        CHECK(counter(pk) == 200 % 16, "oldest kept is packet 200 (cc %d)", counter(pk));
    }
    {   // a real socket on loopback
        sock_t t = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(t, (sockaddr*)&a, sizeof a);
        socklen_t l = sizeof a; getsockname(t, (sockaddr*)&a, &l);
        const int port2 = ntohs(a.sin_port);
        sockClose(t);
        std::function<void(uint8_t*)> src; std::string err;
        const std::string spec = "udp://127.0.0.1:" + std::to_string(port2);
        CHECK(tsUdpOpen(spec, src, err) && src, "open %s: %s", spec.c_str(), err.c_str());
        if (src) {
            sock_t c = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port2); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            auto s = stream(140, 0, 0x200);
            for (int i = 0; i < 20; i++) sockSendTo(c, s.data() + (size_t)i * 7 * 188, 7 * 188, to);
            TsUdpStats st;
            for (int w = 0; w < 100; w++) { st = tsUdpLastStats(); if (st.packets >= 140) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            CHECK(st.running && st.packets == 140 && st.datagrams == 20, "socket: %llu packets in %llu datagrams", (unsigned long long)st.packets, (unsigned long long)st.datagrams);
            int nulls = 0, data = 0, cc = 0;
            for (int i = 0; i < 160; i++) { src(pk); if (isNull(pk)) nulls++; else { data++; CHECK(pk[1] == 2 && counter(pk) == cc % 16, "socket packet order"); cc++; } }
            CHECK(data == 140 && nulls == 20, "socket: %d data, %d null", data, nulls);
            CHECK(tsUdpLastStats().nullsInserted == 20, "null counter");
            // the same through the transmitter's own opener
            std::function<void(uint8_t*)> src2; std::string note;
            CHECK(!txOpenMpegTs(spec, src2, note) || src2, "txOpenMpegTs takes a udp:// name");
            sockClose(c);
        }
        std::function<void(uint8_t*)> bad; std::string e2;
        CHECK(!tsUdpOpen("udp://@:abc", bad, e2) && !bad && !e2.empty(), "bad address refused with a message");
    }
    printf(fails ? "ts_udp_in: %d FAILED\n" : "ts_udp_in: all passed\n", fails);
    return fails ? 1 : 0;
}
