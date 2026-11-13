// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Outbound feed targets: Beast over TCP and AIS over UDP to loopback servers, fan-out to two targets, off by default,
// reconnect after the server goes away.
#include "dect2/net/feeds.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static sock_t listenOn(int& port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return kBadSock;
    socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
    sockSetNonBlocking(ls, true);
    return ls;
}
static sock_t acceptWait(sock_t ls, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c != kBadSock) { sockRecvTimeout(c, 200); return c; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return kBadSock;
}
static std::string readSome(sock_t c, size_t want, int ms) {
    std::string o; char b[512];
    for (int i = 0; i < ms / 20 && o.size() < want; i++) {
        long long k = recv(c, b, sizeof b, 0);
        if (k > 0) o.append(b, (size_t)k); else if (k == 0) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return o;
}
static FeedAdsbFrame frame() {
    FeedAdsbFrame f;
    const uint8_t b[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    memcpy(f.bytes, b, 14); f.bits = 112; f.timeSec = 1; f.levelDbfs = -20;
    return f;
}
template <class F> static bool waitFor(F f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
}

int main() {
    netInit();
    int p1 = 0, p2 = 0;
    sock_t l1 = listenOn(p1), l2 = listenOn(p2);
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ua{}; ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(u, (sockaddr*)&ua, sizeof ua);
    socklen_t ul = sizeof ua; getsockname(u, (sockaddr*)&ua, &ul);
    sockRecvTimeout(u, 300);
    CHECK(l1 != kBadSock && l2 != kBadSock, "loopback listeners");

    FeedConfig c;
    FeedTarget a; a.name = "A"; a.kind = FeedKind::AdsbBeast; a.host = "127.0.0.1"; a.port = p1; a.key = "mykey";
    FeedTarget b = a; b.name = "B"; b.port = p2; b.key.clear();
    FeedTarget ai; ai.name = "AIS"; ai.kind = FeedKind::AisNmea; ai.proto = FeedProto::Udp; ai.host = "127.0.0.1"; ai.port = ntohs(ua.sin_port);
    c.targets = {a, b, ai};   // all disabled
    Feeds::get().configure(c);
    CHECK(!Feeds::get().wantAdsb() && !Feeds::get().wantAis(), "nothing wanted while every target is off");
    Feeds::get().adsb(frame());
    CHECK(acceptWait(l1, 300) == kBadSock, "no connection while disabled");

    c.targets[0].enabled = c.targets[1].enabled = c.targets[2].enabled = true;
    Feeds::get().configure(c);
    CHECK(Feeds::get().wantAdsb() && Feeds::get().wantAis(), "wanted once enabled");
    sock_t c1 = acceptWait(l1, 3000), c2 = acceptWait(l2, 3000);
    CHECK(c1 != kBadSock && c2 != kBadSock, "both TCP targets connect");
    if (c1 == kBadSock || c2 == kBadSock) { printf("feed_targets: %d failures\n", ++fails); return 1; }
    CHECK(readSome(c1, 6, 1000) == "mykey\n", "key line sent first");
    Feeds::get().adsb(frame());
    const std::string r1 = readSome(c1, 23, 2000), r2 = readSome(c2, 23, 2000);
    CHECK(r1.size() >= 23 && r1[0] == 0x1A && r1[1] == '3', "target A got a Beast frame (%zu bytes)", r1.size());
    CHECK(r1 == r2, "both targets got the same bytes (fan-out)");
    Feeds::get().ais({"!AIVDM,1,1,,A,15M67FC000G?ufbE`FepT@3n00Sa,0*5C"});
    char buf[256]; long long k = recv(u, buf, sizeof buf, 0);
    CHECK(k > 10 && std::string(buf, (size_t)k).rfind("!AIVDM", 0) == 0, "AIS datagram arrived");
    CHECK(waitFor([&] { return FeedTargets::get().status(0).sent >= 1; }, 1000), "sent counter");
    FeedTargetStatus s = FeedTargets::get().status(0);
    CHECK(s.connected && s.state.find("connected") == 0, "status connected: %s", s.state.c_str());

    // the server goes away: the target notices and reconnects with a wait
    sockClose(c1);
    CHECK(waitFor([&] { Feeds::get().adsb(frame()); return FeedTargets::get().status(0).reconnects >= 1; }, 3000), "reconnect counted after the server closed");
    sock_t c1b = acceptWait(l1, 8000);
    CHECK(c1b != kBadSock, "target reconnects");
    if (c1b != kBadSock) sockClose(c1b);

    // disabling one stops its data
    c.targets[1].enabled = false;
    Feeds::get().configure(c);
    CHECK(waitFor([&] { return FeedTargets::get().status(1).state == "off"; }, 1000), "disabled target shows off");

    Feeds::get().stop();
    if (c2 != kBadSock) sockClose(c2);
    printf(fails ? "feed_targets: %d failures\n" : "feed_targets: OK\n", fails);
    return fails ? 1 : 0;
}
