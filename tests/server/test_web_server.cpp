// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web network server on loopback with real sockets: static files (and every path traversal trick refused), the placeholder page and its
// headers (CSP, no CORS), /web/v1/info, the WebSocket handshake (version, origin, subprotocol), hello / welcome, wrong password (throttled),
// messages before hello, rate limiting, the admin API (token, kick, policy with the legal notice, invites), busy refusal, keepalive.
#include "../../core/src/net/netcompat.h"
#include "../../server/web/web_server.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#ifndef _WIN32
#include <sys/select.h>
#endif
using namespace onair_web;
using dect2::sock_t;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static sock_t dial(int port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { dect2::sockClose(s); return dect2::kBadSock; }
    return s;
}
static std::string readFor(sock_t s, int ms, size_t stopAt = 0) {
    std::string r;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 20000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[8192];
        const long long k = dect2::sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        r.append(b, (size_t)k);
        if (stopAt && r.size() >= stopAt) break;
    }
    return r;
}
static std::string http(int port, const std::string& req, int ms = 1500) {
    sock_t s = dial(port);
    if (s == dect2::kBadSock) return "";
    dect2::sockSend(s, req.data(), req.size());
    std::string r = readFor(s, ms);
    dect2::sockClose(s);
    return r;
}
static std::string get(int port, const std::string& path, const std::string& extra = "") { return http(port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n"); }
static int status(const std::string& r) { return r.size() > 12 ? atoi(r.c_str() + 9) : 0; }

// server frames (unmasked) out of a byte stream
static std::vector<std::pair<int, std::string>> frames(std::string& buf) {
    std::vector<std::pair<int, std::string>> v;
    while (buf.size() >= 2) {
        const int op = (uint8_t)buf[0] & 15;
        size_t len = (uint8_t)buf[1] & 127, p = 2;
        if (len == 126) { if (buf.size() < 4) break; len = ((size_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3]; p = 4; }
        else if (len == 127) { if (buf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)buf[2 + i]; p = 10; }
        if (buf.size() < p + len) break;
        v.push_back({op, buf.substr(p, len)});
        buf.erase(0, p + len);
    }
    return v;
}

struct Ws {
    sock_t s = dect2::kBadSock;
    std::string buf;
    int status = 0;
    std::string head;
    bool open(int port, const std::string& extra = "") {
        s = dial(port);
        const std::string req = "GET /web/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
        dect2::sockSend(s, req.data(), req.size());
        std::string r;
        for (int i = 0; i < 50 && r.find("\r\n\r\n") == std::string::npos; i++) r += readFor(s, 20, 1);   // just the head, so the keepalive clock is not used up here
        const size_t e = r.find("\r\n\r\n");
        if (e == std::string::npos) { head = r; return false; }
        head = r.substr(0, e);
        status = atoi(head.c_str() + 9);
        buf = r.substr(e + 4);
        return status == 101;
    }
    void send(const std::string& t, int op = 1) { const std::string f = wsMaskedFrame(op, t, 0xA1B2C3D4); dect2::sockSend(s, f.data(), f.size()); }
    std::vector<std::pair<int, std::string>> recv(int ms) { buf += readFor(s, ms); return frames(buf); }
    void close() { if (s != dect2::kBadSock) dect2::sockClose(s); s = dect2::kBadSock; }
    ~Ws() { close(); }
};
static bool anyText(const std::vector<std::pair<int, std::string>>& v, const std::string& sub) { for (auto& f : v) if (f.first == 1 && f.second.find(sub) != std::string::npos) return true; return false; }
static int closeCode(const std::vector<std::pair<int, std::string>>& v) { for (auto& f : v) if (f.first == 8 && f.second.size() >= 2) return ((uint8_t)f.second[0] << 8) | (uint8_t)f.second[1]; return 0; }

static void testStaticPaths(const std::string& root) {
    CHECK(!staticPath(root, "/index.html").empty(), "index");
    CHECK(!staticPath(root, "/").empty(), "/ is index.html");
    CHECK(!staticPath(root, "/js/app.js").empty(), "sub folder");
    for (const char* bad : {"/../secret.txt", "/js/../../secret.txt", "/%2e%2e/secret.txt", "/%2E%2E%2Fsecret.txt", "/js/%2e%2e/%2e%2e/secret.txt", "/.hidden",
                            "/js/.git/config", "/js\\..\\..\\secret.txt", "/index.html%00.js", "//etc/passwd", "/etc/passwd%", "/link/secret.txt",
                            "/a b.html", "/js/app.js?x", "relative.html", "/%c0%ae%c0%ae/secret.txt", "/js//app.js", "/nothere.html"})
        CHECK(staticPath(root, bad).empty(), "must refuse %s", bad);
    CHECK(privateAddress("192.168.1.5") && privateAddress("10.1.2.3") && privateAddress("172.20.0.1") && privateAddress("127.0.0.1") && !privateAddress("8.8.8.8") &&
          !privateAddress("172.32.0.1") && !privateAddress("192.169.0.1") && !privateAddress("1.2.3") && !privateAddress("::1"), "private addresses");
}

int main() {
    dect2::netInit();
    // a web root with a file, a sub folder, a dot file, and a symlink pointing out of it
    const fs::path tmp = fs::temp_directory_path() / ("onair_web_test_" + std::to_string(std::random_device{}()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / "root" / "js");
    fs::create_directories(tmp / "outside");
    std::ofstream(tmp / "root" / "index.html") << "<!doctype html><title>t</title>hello";
    std::ofstream(tmp / "root" / "js" / "app.js") << "console.log(1)";
    std::ofstream(tmp / "root" / ".hidden") << "x";
    std::ofstream(tmp / "secret.txt") << "secret";
    std::ofstream(tmp / "outside" / "secret.txt") << "secret";
    std::error_code ec;
    fs::create_directory_symlink(tmp / "outside", tmp / "root" / "link", ec);
    testStaticPaths((tmp / "root").string());

    WebHub hub;
    HubLimits L; L.maxUsers = 2; L.maxPerIp = 2;
    hub.setLimits(L);
    hub.registerDecoder(DecoderFactory{"aprs", "APRS", 24000, true, nullptr});
    std::vector<cf32> iq(24000, cf32(0, 0));
    hub.pushIq(iq.data(), iq.size(), 145e6, 2.4e6);
    WebServer srv(hub);
    WebServerConfig c;
    c.port = 0;
    c.webRoot = (tmp / "root").string();
    c.auth.mode = AuthMode::Password;
    c.auth.password = "correct horse";
    c.auth.adminToken = "0123456789abcdef0123456789abcdef";
    c.auth.invites.push_back(InviteToken{"inviteinviteinvite01", "friend", 0});
    c.pingSec = 1; c.keepaliveSec = 3;
    c.msgPerSec = 5; c.msgBurst = 5;
    CHECK(srv.start(c), "start: %s", srv.status().lastError.c_str());
    const int port = srv.status().port;
    bool changed = false;
    srv.setOnChange([&](const WebPolicy&, const std::vector<InviteToken>&) { changed = true; });

    // static files and headers
    std::string r = get(port, "/");
    CHECK(status(r) == 200 && r.find("hello") != std::string::npos, "index: %s", r.substr(0, 80).c_str());
    CHECK(r.find("Content-Security-Policy: default-src 'self'") != std::string::npos && r.find("X-Frame-Options: DENY") != std::string::npos, "security headers");
    CHECK(r.find("Access-Control-Allow-Origin") == std::string::npos, "no CORS");
    CHECK(status(get(port, "/js/app.js")) == 200 && get(port, "/js/app.js").find("text/javascript") != std::string::npos, "js file");
    for (const char* bad : {"/../secret.txt", "/%2e%2e/secret.txt", "/link/secret.txt", "/.hidden", "/js/%2e%2e/%2e%2e/secret.txt"}) {
        r = get(port, bad);
        CHECK(status(r) == 404 && r.find("secret") == std::string::npos, "%s refused (%d)", bad, status(r));
    }
    CHECK(status(http(port, "DELETE / HTTP/1.1\r\nHost: x\r\n\r\n")) == 405, "DELETE refused");
    CHECK(status(http(port, "GET / HTTP/1.1\r\nHost: x\r\n" + std::string(9000, 'a') + "\r\n\r\n")) == 431, "huge head");
    CHECK(status(http(port, "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")) == 400, "chunked refused");
    CHECK(status(http(port, "GET nothing HTTP/1.1\r\n\r\n")) == 400, "bad target");
    r = get(port, "/web/v1/info");
    CHECK(status(r) == 200 && r.find("\"auth\":\"password\"") != std::string::npos && r.find("\"max_users\":2") != std::string::npos, "info: %s", r.c_str());
    CHECK(status(get(port, "/web/v1/nothing")) == 404, "unknown api");

    // Behind a reverse proxy on this computer (web_trust_proxy=1): the proxy ends TLS, sends the page's https origin and an X-Forwarded-For.
    // The page's own https origin must be let in then (it was refused when X-Forwarded-For was present), and still refused without trust_proxy.
    {
        auto wsStatus = [&](int p) {
            return status(http(p, "GET /web/v1/ws HTTP/1.1\r\nHost: pi.example.ts.net\r\nOrigin: https://pi.example.ts.net\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nX-Forwarded-For: 203.0.113.9\r\n\r\n"));
        };
        WebServer px(hub);
        WebServerConfig pc = c;
        pc.trustProxy = true;
        CHECK(px.start(pc), "proxy server start: %s", px.status().lastError.c_str());
        CHECK(wsStatus(px.status().port) != 403, "trust_proxy: the page's https origin is let in with X-Forwarded-For (%d)", wsStatus(px.status().port));
        px.stop();
        CHECK(wsStatus(port) == 403, "no trust_proxy: the https origin is refused");
    }

    // WebSocket handshake rules
    {
        Ws w;
        CHECK(!w.open(port, "Origin: https://evil.example\r\n") && w.status == 403, "foreign origin refused (%d)", w.status);
    }
    {
        Ws w;
        CHECK(w.open(port, "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\nSec-WebSocket-Protocol: onair-web.v1\r\n"), "own origin: %s", w.head.c_str());
        CHECK(w.head.find("Sec-WebSocket-Protocol: onair-web.v1") != std::string::npos, "subprotocol echoed");
        w.send("{\"type\":\"tune\",\"freq_hz\":145e6}");
        auto f = w.recv(500);
        CHECK(anyText(f, "send hello first") && closeCode(f) == 4001, "tune before hello closes 4001 (%d)", closeCode(f));
    }
    {
        Ws w;
        CHECK(w.open(port), "open");
        const auto t0 = std::chrono::steady_clock::now();
        w.send("{\"type\":\"hello\",\"proto\":1,\"password\":\"wrong one\"}");
        auto f = w.recv(800);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(anyText(f, "wrong password") && closeCode(f) == 4001 && ms >= 290, "wrong password: closed after a delay (%lld ms, code %d)", (long long)ms, closeCode(f));
    }
    {
        Ws w;
        CHECK(w.open(port), "open");
        w.send("{\"type\":\"transmit\",\"on\":true}");
        w.send("{\"type\":\"hello\",\"proto\":1,\"password\":\"correct horse\",\"name\":\"test\"}");
        auto f = w.recv(500);
        CHECK(anyText(f, "\"code\":\"forbidden\""), "transmit is forbidden");
        CHECK(anyText(f, "\"type\":\"welcome\"") && anyText(f, "\"class\":\"member\""), "welcome after the password");
        w.send("{\"type\":\"tune\",\"freq_hz\":145500000,\"mode\":\"nfm\"}");
        f = w.recv(300);
        CHECK(anyText(f, "\"type\":\"state\"") && anyText(f, "145500000"), "state");
        // the admin API sees the session and can kick it
        r = get(port, "/web/v1/admin/users");
        CHECK(status(r) == 401, "admin without token (%d)", status(r));
        r = get(port, "/web/v1/admin/users", "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n");
        CHECK(status(r) == 200 && r.find("\"name\":\"test\"") != std::string::npos && r.find("127.0.0.1") == std::string::npos, "admin users: %s", r.c_str());
        const size_t ip = r.find("\"id\":");
        const int id = ip == std::string::npos ? 0 : atoi(r.c_str() + ip + 5);
        const std::string body = "{\"id\":" + std::to_string(id) + ",\"reason\":\"test\"}";
        r = http(port, "POST /web/v1/admin/kick HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer 0123456789abcdef0123456789abcdef\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body);
        CHECK(status(r) == 200, "kick: %s", r.c_str());
        f = w.recv(500);
        CHECK(anyText(f, "\"type\":\"kicked\"") && closeCode(f) == 4040, "kicked (%d)", closeCode(f));
        {   // block: the address is refused at once, the list shows it, unblock lets it back (the tests all come from 127.0.0.1)
            const std::string auth = "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\nContent-Type: application/json\r\n";
            auto post = [&](const std::string& path, const std::string& b) { return http(port, "POST " + path + " HTTP/1.1\r\nHost: x\r\n" + auth + "Content-Length: " + std::to_string(b.size()) + "\r\n\r\n" + b); };
            Ws w2;
            w2.open(port);
            w2.send("{\"type\":\"hello\",\"proto\":1,\"password\":\"correct horse\",\"name\":\"bad\"}");
            w2.recv(400);
            std::string ur = get(port, "/web/v1/admin/users", "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n");
            const size_t ip2 = ur.find("\"id\":");
            const int id2 = ip2 == std::string::npos ? 0 : atoi(ur.c_str() + ip2 + 5);
            CHECK(id2 > 0, "second session: %s", ur.c_str());
            r = post("/web/v1/admin/ban", "{\"id\":" + std::to_string(id2) + ",\"minutes\":5,\"reason\":\"spam\"}");
            CHECK(status(r) == 200, "ban: %s", r.c_str());
            auto f2 = w2.recv(500);
            CHECK(anyText(f2, "\"type\":\"kicked\"") && anyText(f2, "spam"), "the banned session is kicked");
            Ws w3;
            w3.open(port);
            w3.send("{\"type\":\"hello\",\"proto\":1,\"password\":\"correct horse\",\"name\":\"again\"}");
            CHECK(anyText(w3.recv(500), "\"code\":\"forbidden\""), "the blocked address is refused");
            r = get(port, "/web/v1/admin/bans", "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n");
            CHECK(status(r) == 200 && r.find("\"minutes_left\":") != std::string::npos, "bans: %s", r.c_str());
            const size_t bt = r.find("\"address\":\"");
            const std::string tag = bt == std::string::npos ? "" : r.substr(bt + 11, r.find('"', bt + 11) - bt - 11);
            r = post("/web/v1/admin/unban", "{\"address\":\"" + tag + "\"}");
            CHECK(status(r) == 200, "unban: %s", r.c_str());
            r = post("/web/v1/admin/unban", "{\"address\":\"" + tag + "\"}");
            CHECK(status(r) == 404, "unban twice: %s", r.c_str());
            r = post("/web/v1/admin/ban", "{\"id\":999999}");
            CHECK(status(r) == 404, "ban unknown: %s", r.c_str());
        }
    }
    {
        // the admin policy needs the legal notice for voice / pagers
        const std::string auth = "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\nContent-Type: application/json\r\n";
        auto post = [&](const std::string& path, const std::string& b) { return http(port, "POST " + path + " HTTP/1.1\r\nHost: x\r\n" + auth + "Content-Length: " + std::to_string(b.size()) + "\r\n\r\n" + b); };
        r = post("/web/v1/admin/policy", "{\"decoder\":\"pocsag\",\"on\":true}");
        CHECK(status(r) == 400 && r.find("legal notice") != std::string::npos, "pager without the notice: %s", r.c_str());
        r = post("/web/v1/admin/policy", std::string("{\"decoder\":\"pocsag\",\"on\":true,\"accept_legal_notice\":\"") + kLegalNoticeVersion + "\"}");
        CHECK(status(r) == 200 && r.find("\"pocsag\":{\"on\":true") != std::string::npos && changed, "pager with the notice: %s", r.c_str());
        r = get(port, "/web/v1/admin/legal", auth);
        CHECK(status(r) == 200 && r.find(kLegalNoticeVersion) != std::string::npos, "legal text");
        r = post("/web/v1/admin/invite", "{\"label\":\"Ana\",\"max_minutes\":15}");
        CHECK(status(r) == 200 && r.find("\"token\":\"") != std::string::npos, "invite made: %s", r.c_str());
        const size_t tp = r.find("\"token\":\"");
        const std::string tok = tp == std::string::npos ? "" : r.substr(tp + 9, 32);
        Ws w;
        w.open(port);
        w.send("{\"type\":\"hello\",\"proto\":1,\"invite\":\"" + tok + "\"}");
        CHECK(anyText(w.recv(400), "\"class\":\"invited\""), "the new invite works");
        r = post("/web/v1/admin/invite/revoke", "{\"token\":\"" + tok + "\"}");
        CHECK(status(r) == 200, "revoke: %s", r.c_str());
        r = http(port, "POST /web/v1/admin/kick HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer 0123456789abcdef0123456789abcdef\r\nContent-Length: 2\r\n\r\n{}");
        CHECK(status(r) == 415, "POST needs JSON content type (%d)", status(r));
        // rate limit: more than the burst in a moment
        for (int i = 0; i < 12; i++) w.send("{\"type\":\"ping\",\"t\":1}");
        auto f = w.recv(500);
        CHECK(anyText(f, "rate_limited") && closeCode(f) == 4008, "rate limited and closed (%d)", closeCode(f));
    }
    {
        // busy: two users at most
        // the connections of the blocks above have to be gone first (their threads see the close within a few ms)
        for (int i = 0; i < 100 && srv.status().connections > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(srv.status().connections == 0, "old connections still open: %d", srv.status().connections);
        for (int i = 0; i < 100 && hub.userCount() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Ws a, b, d;
        CHECK(a.open(port) && b.open(port) && d.open(port), "three opens: %s", a.head.c_str());
        const std::string hi = "{\"type\":\"hello\",\"proto\":1,\"password\":\"correct horse\"}";
        a.send(hi); CHECK(anyText(a.recv(300), "welcome"), "first");
        b.send(hi); CHECK(anyText(b.recv(300), "welcome"), "second");
        d.send(hi);
        auto f = d.recv(400);
        CHECK(anyText(f, "\"code\":\"busy\"") && closeCode(f) == 4029, "third: busy (%d)", closeCode(f));
        // keepalive: a client that answers nothing (no pong) is closed after keepaliveSec; pings arrive every pingSec
        auto g = a.recv(4000);
        bool ping = false;
        for (auto& x : g) if (x.first == 9) ping = true;
        CHECK(ping, "server pings");
        CHECK(closeCode(g) == 1001, "silent client closed by the keepalive (%d)", closeCode(g));
    }
    {
        // the hello must come within the timeout; a binary message is refused
        Ws w;
        w.open(port);
        w.send("abc", 2);
        CHECK(closeCode(w.recv(400)) == 1003, "binary refused");
    }
    srv.stop();
    fs::remove_all(tmp);
    return testSummary("web_server");
}
