// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live rooms: classification of failures and the availability check, against a fake HTTP server on loopback.
#include "rooms/rooms_net.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace roomsnet;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static plat::HttpResult R(int status, int net = 0, const char* body = "") {
    plat::HttpResult r; r.status = status; r.net = net; r.body = body; r.detail = "x"; return r;
}

#ifndef _WIN32
// a tiny server: answers every request with `status` and `body`, or (hang) never answers
struct Fake {
    int fd = -1, port = 0;
    std::atomic<int> status{200};
    std::atomic<bool> hang{false}, stop{false};
    std::string body;
    std::thread th;
    Fake() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(fd, (sockaddr*)&a, sizeof a); listen(fd, 8);
        socklen_t l = sizeof a; getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!stop) {
                pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 50) <= 0) continue;
                int c = accept(fd, nullptr, nullptr);
                if (c < 0) continue;
                char buf[2048]; (void)!read(c, buf, sizeof buf);
                if (hang) { std::this_thread::sleep_for(std::chrono::milliseconds(2500)); close(c); continue; }
                char h[256];
                int n = snprintf(h, sizeof h, "HTTP/1.1 %d X\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status.load(), body.size());
                (void)!write(c, h, n); (void)!write(c, body.data(), body.size());
                close(c);
            }
        });
    }
    ~Fake() { stop = true; th.join(); close(fd); }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
};

// a minimal GET client: the production code uses curl or NSURLSession, this stands in for it
static plat::HttpResult rawGet(const std::string& url, int timeoutSec) {
    plat::HttpResult r;
    const size_t c = url.rfind(':'), sl = url.find('/', c);
    const int port = std::atoi(url.substr(c + 1, sl - c - 1).c_str());
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { close(s); r.net = 2; r.detail = "connection refused"; return r; }
    const char* req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    (void)!write(s, req, strlen(req));
    pollfd p{s, POLLIN, 0};
    if (poll(&p, 1, timeoutSec * 1000) <= 0) { close(s); r.net = 3; r.detail = "timed out"; return r; }
    char buf[4096]; std::string all; ssize_t n;
    while ((n = read(s, buf, sizeof buf)) > 0) all.append(buf, (size_t)n);
    close(s);
    if (all.compare(0, 5, "HTTP/") != 0) { r.net = 4; return r; }
    r.status = std::atoi(all.c_str() + all.find(' ') + 1);
    const size_t b = all.find("\r\n\r\n");
    if (b != std::string::npos) r.body = all.substr(b + 4);
    return r;
}
#endif

int main() {
    // classification
    CHECK(classify(R(200)) == Fail::None, "200");
    CHECK(classify(R(302)) == Fail::None, "302");
    CHECK(classify(R(404)) == Fail::NotFound, "404");
    CHECK(classify(R(410)) == Fail::NotFound, "410");
    CHECK(classify(R(500)) == Fail::Unavailable, "500");
    CHECK(classify(R(503, 0, "{\"error\":\"not configured\"}")) == Fail::Unavailable, "503 not configured");
    CHECK(classify(R(503, 0, "{\"error\":\"full\"}")) == Fail::Full, "503 full");
    CHECK(classify(R(429, 0, "{\"error\":\"rate\"}")) == Fail::RateLimited, "429");
    CHECK(classify(R(400)) == Fail::Refused, "400");
    CHECK(classify(R(403)) == Fail::Refused, "403");
    CHECK(classify(R(0, 1)) == Fail::Dns, "dns");
    CHECK(classify(R(0, 2)) == Fail::Offline, "offline");
    CHECK(classify(R(0, 3)) == Fail::Timeout, "timeout");
    CHECK(classify(R(0, 4)) == Fail::Other, "other");
    CHECK(serverMissing(Fail::Dns) && serverMissing(Fail::NotFound) && serverMissing(Fail::Unavailable), "missing kinds");
    CHECK(!serverMissing(Fail::Full) && !serverMissing(Fail::Offline) && !serverMissing(Fail::Timeout) && !serverMissing(Fail::RateLimited), "not missing kinds");
    CHECK(describe("http://h/", R(404)).find("HTTP 404") != std::string::npos, "describe status");
    CHECK(describe("http://h/", R(0, 1)).find("x") != std::string::npos, "describe detail");

    // cache
    {
        AvailCache c;
        const auto t0 = AvailCache::Clock::now();
        Avail ok; ok.ok = true;
        CHECK(!c.fresh("o", t0), "empty cache");
        c.store("o", ok, t0);
        CHECK(c.fresh("o", t0 + std::chrono::minutes(9)), "ok fresh for 9 min");
        CHECK(!c.fresh("o", t0 + std::chrono::minutes(11)), "ok stale after 10 min");
        CHECK(!c.fresh("other", t0), "another origin");
        Avail gone; gone.why = Fail::NotFound;
        c.store("o", gone, t0);
        CHECK(c.fresh("o", t0 + std::chrono::hours(5)), "missing server stays until reset");
        c.reset();
        CHECK(!c.fresh("o", t0), "reset");
        Avail off; off.why = Fail::Offline;
        c.store("o", off, t0);
        CHECK(!c.fresh("o", t0), "offline is not kept");
        CHECK(c.value().why == Fail::Offline, "but its reason can be shown");
    }

#ifndef _WIN32
    Fake srv;
    const Transport t = [](const std::string&, const std::string& url, const std::string&, int to) { return rawGet(url, to); };
    srv.status = 200;
    CHECK(checkAvailable(srv.origin(), t).ok, "200 is available");
    srv.status = 404;
    { Avail a = checkAvailable(srv.origin(), t); CHECK(!a.ok && a.why == Fail::NotFound && a.detail.find("404") != std::string::npos, "404"); }
    srv.status = 500;
    { Avail a = checkAvailable(srv.origin(), t); CHECK(!a.ok && a.why == Fail::Unavailable, "500"); }
    srv.status = 503; srv.body = "{\"error\":\"full\"}";
    { Avail a = checkAvailable(srv.origin(), t); CHECK(!a.ok && a.why == Fail::Full, "full"); }
    srv.status = 429;
    { Avail a = checkAvailable(srv.origin(), t); CHECK(!a.ok && a.why == Fail::RateLimited, "rate"); }
    srv.hang = true;
    { Avail a = checkAvailable(srv.origin(), t, 1); CHECK(!a.ok && a.why == Fail::Timeout, "hang gives a timeout"); }
    srv.hang = false;
    const std::string dead = srv.origin();
    srv.stop = true; srv.th.join(); close(srv.fd);
    { Avail a = checkAvailable(dead, t); CHECK(!a.ok && a.why == Fail::Offline, "closed port gives no connection"); }
    srv.th = std::thread([] {}); srv.fd = socket(AF_INET, SOCK_STREAM, 0);   // so the destructor has something to join and close
#endif
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
