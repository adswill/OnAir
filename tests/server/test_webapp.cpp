// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web 3.9: the browser app's server side (server/app). Part 1 is the access layer alone (Sessions): who a request is, logins, the
// fail limiter, CSRF, cookies, password rules, permissions. Part 2 runs the real thing: ServerHost on the built-in test signal, the web
// server with the app mounted, and talks HTTP and WebSocket to it from 127.0.0.1 (an owner by address), through a trusted "proxy" header as
// an internet visitor (login needed; public mode; the visitor cap; what a visitor may do), and checks that every mode of the mode table can
// be selected through the API and reports its row back. No browser, no sound, no files outside a temporary settings file.
#include "dect2/m17/m17_codec2.h"
#include "../../core/src/net/tcp_client.h"
#include "../../server/app/webapp.h"
#include "../../server/server_config.h"
#include "../../server/server_host.h"
#include "../../server/web/web_config.h"
#include "../../server/web/web_hub.h"
#include "../../server/web/web_server.h"
#include "dect2/results/web_schema.h"
#include "../common/test_util.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

using namespace dect2;
using namespace onair_app;
using namespace onair_server;

// ---------------------------------------------------------------- HTTP / WS client helpers
static std::string readFor(sock_t s, int ms, size_t stopAt = 0) {
    std::string r;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) { if (stopAt && r.size() >= stopAt) break; continue; }
        char b[8192];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        r.append(b, (size_t)k);
        if (stopAt && r.size() >= stopAt) break;
    }
    return r;
}
struct Resp { int status = 0; std::string head, body; std::string header(const std::string& k) const { const size_t p = head.find("\r\n" + k + ": "); if (p == std::string::npos) return ""; const size_t s = p + k.size() + 4, e = head.find("\r\n", s); return head.substr(s, e - s); } };
struct Client {
    int port;
    std::string cookie, csrf, forwardedFor, origin;
    int timeoutMs = 4000;
    Resp req(const std::string& method, const std::string& path, const std::string& body = "") {
        std::string e;
        sock_t s = tcpConnect("127.0.0.1", port, 2000, e);
        Resp o;
        if (s == kBadSock) return o;
        std::string rq = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
        if (!cookie.empty()) rq += "Cookie: onair_session=" + cookie + "\r\n";
        if (!forwardedFor.empty()) rq += "X-Forwarded-For: " + forwardedFor + "\r\n";
        if (!origin.empty()) rq += "Origin: " + origin + "\r\n";
        if (method == "POST") rq += "Content-Type: application/json\r\nX-OnAir-CSRF: " + csrf + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        rq += "\r\n" + body;
        sockSendAll(s, rq.data(), rq.size());
        std::string r = readFor(s, timeoutMs);
        sockClose(s);
        if (r.compare(0, 9, "HTTP/1.1 ") == 0) o.status = atoi(r.c_str() + 9);
        const size_t h = r.find("\r\n\r\n");
        o.head = r.substr(0, h);
        if (h != std::string::npos) o.body = r.substr(h + 4);
        return o;
    }
    Resp get(const std::string& p) { return req("GET", "/app/v1/" + p); }
    Resp post(const std::string& p, const std::string& body = "{}") { return req("POST", "/app/v1/" + p, body); }
    bool session() {   // refresh the csrf from GET session
        const Resp r = get("session");
        const size_t p = r.body.find("\"csrf\":\"");
        if (r.status != 200 || p == std::string::npos) return false;
        csrf = r.body.substr(p + 8, r.body.find('"', p + 8) - p - 8);
        return true;
    }
};
static std::string cframe(const std::string& pl, int op = 1) {
    std::string f;
    f.push_back((char)(0x80 | op));
    if (pl.size() < 126) f.push_back((char)(0x80 | pl.size()));
    else { f.push_back((char)(0x80 | 126)); f.push_back((char)(pl.size() >> 8)); f.push_back((char)(pl.size() & 0xFF)); }
    const char key[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(key, 4);
    for (size_t i = 0; i < pl.size(); i++) f.push_back((char)(pl[i] ^ key[i & 3]));
    return f;
}
// server frames out of a byte stream: {opcode, payload}
static std::vector<std::pair<int, std::string>> frames(std::string& buf) {
    std::vector<std::pair<int, std::string>> out;
    while (buf.size() >= 2) {
        const int op = buf[0] & 0x0F;
        size_t len = (unsigned char)buf[1] & 0x7F, hdr = 2;
        if (len == 126) { if (buf.size() < 4) break; len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3]; hdr = 4; }
        else if (len == 127) { if (buf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | (unsigned char)buf[2 + i]; hdr = 10; }
        if (buf.size() < hdr + len) break;
        out.push_back({op, buf.substr(hdr, len)});
        buf.erase(0, hdr + len);
    }
    return out;
}
struct Ws {
    sock_t s = kBadSock;
    std::string buf;
    int closeCode = 0;
    bool open(int port, const std::string& csrf, const std::string& cookie, const std::string& fwd) {
        std::string e;
        s = tcpConnect("127.0.0.1", port, 2000, e);
        if (s == kBadSock) return false;
        std::string rq = "GET /app/v1/ws?csrf=" + csrf + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
        if (!cookie.empty()) rq += "Cookie: onair_session=" + cookie + "\r\n";
        if (!fwd.empty()) rq += "X-Forwarded-For: " + fwd + "\r\n";
        rq += "\r\n";
        sockSendAll(s, rq.data(), rq.size());
        const std::string r = readFor(s, 3000, 1);
        const size_t h = r.find("\r\n\r\n");
        if (r.compare(0, 12, "HTTP/1.1 101") != 0 || h == std::string::npos) { buf = r; return false; }
        buf = r.substr(h + 4);
        return true;
    }
    void send(const std::string& j) { const std::string f = cframe(j); sockSendAll(s, f.data(), f.size()); }
    // collects frames for ms; text frames into texts, binary into bins
    void collect(int ms, std::vector<std::string>& texts, std::vector<std::string>& bins) {
        buf += readFor(s, ms);
        for (auto& f : frames(buf)) {
            if (f.first == 1) texts.push_back(f.second);
            else if (f.first == 2) bins.push_back(f.second);
            else if (f.first == 8 && f.second.size() >= 2) closeCode = ((unsigned char)f.second[0] << 8) | (unsigned char)f.second[1];
            else if (f.first == 9) { const std::string p = cframe(f.second, 10); sockSendAll(s, p.data(), p.size()); }
        }
    }
    void close() { if (s != kBadSock) { sockClose(s); s = kBadSock; } }
};
static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static int countOf(const std::vector<std::string>& v, const std::string& x) { int n = 0; for (const auto& s : v) if (has(s, x)) n++; return n; }

int main() {
    int fails = 0;
    setvbuf(stdout, nullptr, _IONBF, 0);   // a FAIL line survives an abort
    // ================================================================ part 1: the access layer
    {
        AccessConfig a;
        a.passwordHash = onair_web::hashPassword("correct horse");
        Sessions ss(a);
        const double t = 1000;
        Who w = ss.identify("", "127.0.0.1", true, false, t);
        CHECK(w.role == Role::Owner && w.implicit && w.csrf == "lan", "loopback is an implicit owner");
        w = ss.identify("", "192.168.1.20", false, false, t);
        CHECK(w.role == Role::Owner && w.implicit, "a LAN address is an implicit owner (app_lan=1)");
        w = ss.identify("", "192.168.1.20", false, true, t);
        CHECK(w.role == Role::None && w.why == "login", "a private address behind the relay / proxy is NOT trusted");
        w = ss.identify("", "127.0.0.1", true, true, t);
        CHECK(w.role == Role::Owner && w.implicit, "with the relay on, a connection still from 127.0.0.1 (not rewritten) is this computer: the owner");
        w = ss.identify("", "203.0.113.5", false, false, t);
        CHECK(w.role == Role::None && w.why == "login", "an internet address needs a login");
        a.lanIsOwner = false;
        ss.setConfig(a);
        w = ss.identify("", "192.168.1.20", false, false, t);
        CHECK(w.role == Role::None, "app_lan=0: the LAN logs in too");
        w = ss.identify("", "127.0.0.1", true, false, t);
        CHECK(w.role == Role::Owner, "this computer stays an owner");
        a.publicView = true;
        ss.setConfig(a);
        w = ss.identify("", "203.0.113.5", false, false, t);
        CHECK(w.role == Role::Viewer && !ss.mayControl(w) && !ss.maySettings(w) && !ss.mayTransmit(w) && !ss.mayRecord(w), "public: a viewer, read-only");
        a.publicTune = true;
        ss.setConfig(a);
        CHECK(ss.mayControl(w) && !ss.mayTransmit(w) && !ss.maySettings(w), "public_tune: a viewer may tune, never transmit or change settings");
        // login
        std::string tok, why;
        CHECK(!ss.login("wrong", "203.0.113.5", t, tok, why) && why == "wrong password", "wrong password: %s", why.c_str());
        CHECK(ss.login("correct horse", "203.0.113.5", t, tok, why) && tok.size() == 64, "login gives a 64 hex token");
        w = ss.identify("onair_session=" + tok, "203.0.113.5", false, true, t + 1);
        CHECK(w.role == Role::Owner && w.loggedIn && !w.implicit && w.csrf.size() == 32, "the cookie makes an owner through the proxy");
        CHECK(ss.csrfOk(w, w.csrf) && !ss.csrfOk(w, "") && !ss.csrfOk(w, "lan") && !ss.csrfOk(w, w.csrf + "x"), "csrf must match");
        w = ss.identify("other=1; onair_session=" + tok + "; x=2", "203.0.113.5", false, true, t + 1);
        CHECK(w.loggedIn, "cookie parsing among other cookies");
        CHECK(cookieValue("a=1; onair_session=abc; b=2", "onair_session") == "abc" && cookieValue("a=1", "onair_session").empty(), "cookieValue");
        w = ss.identify("onair_session=" + tok, "203.0.113.5", false, true, t + 169 * 3600);
        CHECK(w.role == Role::Viewer, "the session expires after app_session_hours");
        CHECK(ss.login("correct horse", "203.0.113.5", t, tok, why), "login again");
        ss.logout("onair_session=" + tok);
        w = ss.identify("onair_session=" + tok, "203.0.113.5", false, true, t);
        CHECK(!w.loggedIn, "logout");
        // the fail limiter: 5 wrong -> blocked for a minute, the right password is refused too
        for (int i = 0; i < 5; i++) ss.login("nope", "198.51.100.9", t + i, tok, why);
        CHECK(!ss.login("correct horse", "198.51.100.9", t + 6, tok, why) && has(why, "too many"), "blocked after 5 failures: %s", why.c_str());
        CHECK(ss.login("correct horse", "198.51.100.9", t + 70, tok, why), "unblocked after a minute");
        CHECK(ss.login("correct horse", "203.0.113.7", t + 6, tok, why), "another address is not blocked");
        // no password set
        AccessConfig b;
        Sessions s2(b);
        CHECK(!s2.login("anything", "203.0.113.5", t, tok, why) && why == "no password set", "no password: %s", why.c_str());
        w = s2.identify("", "203.0.113.5", false, false, t);
        CHECK(w.role == Role::None && w.why == "setup", "the page learns that a password must be set first");
        // the API rate limit
        int allowed = 0;
        for (int i = 0; i < 100; i++) if (ss.rateOk("203.0.113.9", t)) allowed++;
        CHECK(allowed >= 90 && allowed <= 110, "the burst is about 100 (%d)", allowed);
        CHECK(ss.rateOk("203.0.113.9", t + 10), "it refills");
        // password rules, cookies
        CHECK(!Sessions::passwordAcceptable("short", why) && Sessions::passwordAcceptable("long enough", why) && !Sessions::passwordAcceptable(std::string(200, 'a'), why) && !Sessions::passwordAcceptable("tab\there!", why), "password rules");
        const std::string c = Sessions::setCookie("abc", true, 2);
        CHECK(has(c, "HttpOnly") && has(c, "SameSite=Strict") && has(c, "Secure") && has(c, "Max-Age=7200") && c.compare(0, 12, "Set-Cookie: ") == 0, "cookie attributes: %s", c.c_str());
        CHECK(!has(Sessions::setCookie("abc", false, 2), "Secure"), "no Secure on plain http (the LAN)");
        CHECK(has(Sessions::clearCookie(false), "Max-Age=0"), "clearCookie");
        CHECK(std::string(roleName(Role::Owner)) == "owner" && std::string(roleName(Role::Viewer)) == "viewer", "roleName");
    }
    // ================================================================ part 2: the server
    const auto dir = std::filesystem::temp_directory_path() / ("onair-webapp-test-" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::string cfgPath = (dir / "server.conf").string();
    ConfigFile cfg;
    cfg.set("device", "test");
    cfg.set("mode", "fm");
    cfg.set("web", "1");
    cfg.set("web_port", "0");
    cfg.set("web_trust_proxy", "1");   // X-Forwarded-For from 127.0.0.1 is the visitor's address, as behind the relay
    cfg.set("web_admin_token", "0123456789abcdef0123456789abcdef");
    cfg.set("app_public_max", "2");
    cfg.set("web_name", "Test radio");
    std::string err;
    cfg.save(cfgPath, err);
    ServerHost host(cfg);
    host.init();
    bool made = false;
    onair_web::WebSettings webCfg = onair_web::loadWebSettings(cfg, made);
    onair_web::WebHub hub;
    hub.setLimits(webCfg.limits);
    hub.policy().load(cfg);
    hub.start();
    host.setWebHub(&hub);
    AppOptions ao;
    ao.root = (std::filesystem::path(ONAIR_SOURCE_DIR) / "web" / "onair-app").string();
    ao.name = "Test radio";
    ao.version = "test";
    WebApp app(host, cfg, ao);
    onair_web::WebHub* h = &hub;
    app.setPublicModeAllowed([h](const std::string& id) { std::lock_guard<std::mutex> g(h->policyMutex()); return h->policy().decoderAllowed(id); });
    webCfg.server.app = &app;
    onair_web::WebServer web(hub);
    CHECK(web.start(webCfg.server), "web server starts: %s", web.status().lastError.c_str());
    const int port = web.status().port;
    // the app's supervisor loop, as onair-server's main runs it; joined before the teardown (detached, it outlived `app` and its
    // Sessions mutex at exit: "mutex lock failed" on macOS)
    std::atomic<bool> supStop{false};
    std::thread sup([&] { while (!supStop && port) { host.supervise(cfgPath); app.tick(onair_web::wallSeconds()); std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });

    // ---- this computer: an implicit owner
    Client lan{port};
    Resp r = lan.get("info");
    CHECK(r.status == 200 && has(r.body, "\"role\":\"owner\"") && has(r.body, "\"hasPassword\":false") && has(r.body, "\"name\":\"Test radio\""), "info from this computer: %d %s", r.status, r.body.c_str());
    r = lan.req("GET", "/");
    CHECK(r.status == 302 && has(r.head, "Location: /app/"), "/ redirects to the app: %d", r.status);
    r = lan.req("GET", "/app");
    CHECK(r.status == 302, "/app redirects to /app/");
    r = lan.req("GET", "/app/");
    CHECK(r.status == 200 && has(r.body, "<title>OnAir</title>") && has(r.head, "Content-Security-Policy: default-src 'self'") && has(r.head, "X-Frame-Options: DENY"), "the page with a strict CSP: %d", r.status);
    r = lan.req("GET", "/app/lib.js");
    CHECK(r.status == 200 && has(r.head, "javascript") && has(r.body, "OnAirLib"), "static files");
    r = lan.req("GET", "/app/../server.conf");
    CHECK(r.status == 404, "no path escape: %d", r.status);
    r = lan.req("GET", "/app/nothing.js");
    CHECK(r.status == 404, "missing file");
    CHECK(lan.session() && lan.csrf == "lan", "session gives csrf 'lan' for an implicit owner (%s)", lan.csrf.c_str());
    r = lan.get("modes");
    CHECK(r.status == 200 && has(r.body, "\"groups\":[") && has(r.body, "\"name\":\"ADS-B\""), "modes");
    r = lan.get("schema");
    CHECK(r.status == 200 && has(r.body, "\"common\":[") && has(r.body, "\"12\":[") && r.body.size() > 100000, "schema (%zu bytes)", r.body.size());
    r = lan.get("devices");
    CHECK(r.status == 200 && has(r.body, "synthetic"), "devices");
    r = lan.get("status");
    CHECK(r.status == 200 && has(r.body, "\"row\":{") && has(r.body, "\"name\":\"FM\"") && has(r.body, "\"app\":{\"viewers\":0"), "status with the mode row: %s", r.body.c_str());
    r = lan.get("settings");
    CHECK(r.status == 200 && has(r.body, "\"key\":\"app_public\"") && has(r.body, "\"group\":\"Access\"") && has(r.body, "\"help\":"), "settings model");
    r = lan.get("panel");
    CHECK(r.status == 200 && has(r.body, "\"seq\":"), "panel before start");
    // CSRF: a POST without the header is refused, with a wrong origin too
    { Client noCsrf{port}; r = noCsrf.post("frequency", "{\"hz\":100e6}"); CHECK(r.status == 403 && has(r.body, "CSRF"), "POST needs the csrf header: %d", r.status); }
    { Client badOrigin = lan; badOrigin.origin = "http://evil.example"; r = badOrigin.post("frequency", "{\"hz\":100e6}"); CHECK(r.status == 403 && has(r.body, "origin"), "a foreign origin is refused: %d", r.status); }
    lan.origin = "http://127.0.0.1:" + std::to_string(port);
    r = lan.post("frequency", "{\"hz\":101300000}");
    CHECK(r.status == 200, "tune: %d %s", r.status, r.body.c_str());
    r = lan.post("start", "{}");
    CHECK(r.status == 200, "start: %d %s", r.status, r.body.c_str());
    for (int i = 0; i < 100; i++) { r = lan.get("status"); if (has(r.body, "\"running\":true")) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(has(r.body, "\"running\":true") && has(r.body, "\"frequencyHz\":101300000"), "running at the tuned frequency: %s", r.body.c_str());
    for (int i = 0; i < 100; i++) { r = lan.get("panel"); if (has(r.body, "\"standard\":6")) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(has(r.body, "\"standard\":6") && has(r.body, "\"common\":{") && has(r.body, "\"fields\":{"), "the FM panel arrives: %.200s", r.body.c_str());
    r = lan.post("gain", "{\"db\":20}");
    CHECK(r.status == 400 && has(r.body, "no gain"), "the test signal has no gain control, the page is told: %d %s", r.status, r.body.c_str());
    r = lan.post("frequency", "{\"hz\":\"x\"}");
    CHECK(r.status == 400, "a bad frequency is refused");
    r = lan.post("demod", "{\"demod\":\"usb\"}");
    CHECK(r.status == 200, "demod: %d %s", r.status, r.body.c_str());
    r = lan.post("mode", "{\"family\":11,\"sub\":\"\"}");
    CHECK(r.status == 200, "mode ADS-B by family: %d %s", r.status, r.body.c_str());
    r = lan.get("status");
    CHECK(has(r.body, "\"name\":\"ADS-B\"") && has(r.body, "\"family\":13"), "status reports ADS-B (engine 13): %s", r.body.c_str());
    r = lan.post("mode", "{\"family\":25,\"sub\":\"wspr\"}");
    CHECK(r.status == 200, "a sub-mode: %d %s", r.status, r.body.c_str());
    r = lan.get("status");
    CHECK(has(r.body, "\"name\":\"WSPR\"") && has(r.body, "\"sub\":\"wspr\""), "status reports WSPR: %s", r.body.c_str());
    r = lan.post("mode", "{\"id\":\"fm\"}");
    CHECK(r.status == 200, "mode by id");
    r = lan.post("mode", "{\"family\":99999,\"sub\":\"\"}");
    CHECK(r.status == 404, "an unknown mode");
    r = lan.post("tx/start", "{}");
    CHECK(r.status == 501, "the server has no transmitter: %d %s", r.status, r.body.c_str());
    r = lan.post("nothing", "{}");
    CHECK(r.status == 404, "unknown endpoint");
    // every mode of the mode table can be selected and reports its row back
    {
        int n = 0, ok = 0, bad = 0;
        const webschema::WebModeRow* rows = webschema::webModeRows(n);
        std::string firstBad;
        for (int i = 0; i < n; i++) {
            const webschema::WebModeRow& m = rows[i];
            if (m.family == 999 || m.family >= 19999) continue;   // Plugins and Mode Builder are not receivers
            std::this_thread::sleep_for(std::chrono::milliseconds(40));   // two requests per mode: stay under the API's per-address rate
            lan.timeoutMs = 20000;   // a big receiver (X-band) takes seconds to set up while the host's lock is held
            r = lan.post("mode", "{\"family\":" + std::to_string(m.family) + ",\"sub\":\"" + m.sub + "\"}");
            if (r.status == 0) r = lan.post("mode", "{\"family\":" + std::to_string(m.family) + ",\"sub\":\"" + m.sub + "\"}");
            lan.timeoutMs = 4000;
            bool good = r.status == 200;
            if (good) { r = lan.get("status"); good = has(r.body, "\"row\":{\"family\":" + std::to_string(m.family) + ",") && has(r.body, "\"sub\":\"" + std::string(m.sub) + "\""); }
            if (good) ok++; else { bad++; if (firstBad.empty()) firstBad = std::string(m.name) + ": " + std::to_string(r.status) + " " + r.body.substr(0, 160); }
        }
        CHECK(bad == 0, "%d of %d modes could not be selected through the app; first: %s", bad, ok + bad, firstBad.c_str());
        printf("modes selectable through the app: %d (%d refused)\n", ok, bad);
        lan.post("mode", "{\"id\":\"fm\"}");
    }
    // settings: a change applies live and lands in the file
    r = lan.post("settings", "{\"app_public_max\":3,\"web_name\":\"Renamed\"}");
    CHECK(r.status == 200 && has(r.body, "\"changed\":[") && has(r.body, "app_public_max"), "settings saved: %d %s", r.status, r.body.c_str());
    r = lan.get("info");
    CHECK(has(r.body, "\"publicMax\":3") && has(r.body, "\"name\":\"Renamed\""), "live settings: %s", r.body.c_str());
    r = lan.post("settings", "{\"no_such\":1}");
    CHECK(r.status == 400, "unknown setting refused");
    r = lan.post("settings", "{\"app_public_max\":\"x\"}");
    CHECK(r.status == 400, "wrong type refused");
    for (int i = 0; i < 40; i++) { ConfigFile saved; saved.load(cfgPath); if (saved.get("app_public_max") == "3") break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    { ConfigFile saved; saved.load(cfgPath); CHECK(saved.get("app_public_max") == "3" && saved.get("web_name") == "Renamed", "the settings file was written"); }
    // the WebSocket of the owner: hello, status, panel and spectrum
    {
        Ws ws;
        CHECK(ws.open(port, "lan", "", ""), "owner WebSocket opens: %.120s", ws.buf.c_str());
        ws.send("{\"type\":\"sub\",\"panel\":true,\"status\":true,\"spectrum\":true,\"bins\":256,\"fps\":10,\"audio\":true,\"rate\":16000}");
        std::vector<std::string> texts, bins;
        ws.collect(2500, texts, bins);
        CHECK(countOf(texts, "\"type\":\"hello\"") == 1 && has(texts[0], "\"owner\":true") && has(texts[0], "\"control\":true"), "hello: %s", texts.empty() ? "" : texts[0].c_str());
        CHECK(countOf(texts, "\"type\":\"status\"") >= 1, "status frames (%d)", countOf(texts, "\"type\":\"status\""));
        CHECK(countOf(texts, "\"type\":\"panel\"") >= 1, "panel frames (%d)", countOf(texts, "\"type\":\"panel\""));
        int spec = 0, audio = 0;
        for (const auto& b : bins) { if (!b.empty() && b[0] == 1) spec++; if (!b.empty() && b[0] == 2) audio++; }
        CHECK(spec >= 5 && spec <= 40, "about 10 spectrum rows a second for 2.5 s: %d", spec);
        CHECK(audio >= 5, "audio frames arrive (%d)", audio);
        if (audio) { const std::string& a = bins[0][0] == 2 ? bins[0] : bins[1]; CHECK(a.size() > 5 && (unsigned char)a[1] == 0x80 && (unsigned char)a[2] == 0x3E, "audio frame header says 16000 Hz"); }
        ws.send("{\"type\":\"tune\",\"hz\":98700000}");
        ws.send("{\"type\":\"ping\",\"t\":7}");
        texts.clear(); bins.clear();
        ws.collect(1500, texts, bins);
        CHECK(countOf(texts, "\"type\":\"pong\",\"t\":7") == 1, "pong");
        r = lan.get("status");
        CHECK(has(r.body, "\"frequencyHz\":98700000"), "tune over the WebSocket: %s", r.body.c_str());
        ws.send("{\"type\":\"bye\"}");
        texts.clear(); bins.clear();
        ws.collect(500, texts, bins);
        CHECK(ws.closeCode == 1000, "bye closes with 1000 (%d)", ws.closeCode);
        ws.close();
        r = lan.get("viewers");
        CHECK(has(r.body, "\"connections\":0"), "the connection is counted away: %s", r.body.c_str());
    }
    // ---- 3.9 leftovers: waterfall zoom, the mode options, digital voice in the browser
    {
        auto f32 = [](const std::string& b, size_t at) { float v; uint32_t x = 0; for (int i = 0; i < 4; i++) x |= (uint32_t)(unsigned char)b[at + (size_t)i] << (8 * i); std::memcpy(&v, &x, 4); return v; };
        Ws ws;
        CHECK(ws.open(port, "lan", "", ""), "WebSocket for zoom");
        ws.send("{\"type\":\"sub\",\"panel\":false,\"status\":false,\"spectrum\":true,\"bins\":512,\"fps\":10}");
        std::vector<std::string> texts, bins;
        ws.collect(1200, texts, bins);
        float rate1 = 0, centre1 = 0;
        for (const auto& b : bins) if (b.size() > 21 && b[0] == 1) { centre1 = f32(b, 5); rate1 = f32(b, 9); }
        CHECK(rate1 > 0, "a whole-capture row (%.0f Hz wide)", rate1);
        // zoom 4 about a frequency a quarter span above the centre; then a zoom past the limit; then a centre outside the capture
        struct Z { double zoom, hz; } zs[] = {{4, centre1 + rate1 / 4}, {1e6, centre1}, {8, centre1 + rate1 * 10}};
        for (const Z& z : zs) {
            char m[160];
            snprintf(m, sizeof m, "{\"type\":\"sub\",\"spectrum\":true,\"zoom\":%.0f,\"zoomHz\":%.0f}", z.zoom, z.hz);
            ws.send(m);
            texts.clear(); bins.clear();
            ws.collect(1200, texts, bins);
            float rate = 0, centre = 0;
            size_t nb = 0;
            for (const auto& b : bins) if (b.size() > 21 && b[0] == 1) { centre = f32(b, 5); rate = f32(b, 9); nb = b.size() - 21; }
            const double want = rate1 / std::min(z.zoom, 1e9);
            if (z.zoom == 4) {
                CHECK(std::fabs(rate - want) < want * 0.02 && std::fabs(centre - z.hz) < rate1 * 0.01, "zoom 4: %.0f Hz wide around %.0f (want %.0f around %.0f)", rate, centre, want, z.hz);
                CHECK(nb >= 64 && nb <= 512, "zoomed bins %zu", nb);
            } else if (z.zoom > 1000) {
                CHECK(rate > 0 && rate < rate1 / 4 && nb >= 64, "a huge zoom stops at 64 FFT bins: %.0f Hz, %zu bins", rate, nb);
            } else {
                CHECK(centre + rate / 2 <= centre1 + rate1 / 2 + rate1 * 0.001 && std::fabs(rate - rate1 / 8) < rate1 * 0.01, "a centre outside is moved inside the capture: %.0f + %.0f", centre, rate / 2);
            }
        }
        ws.send("{\"type\":\"sub\",\"zoom\":1}");
        texts.clear(); bins.clear();
        ws.collect(800, texts, bins);
        float rateBack = 0;
        for (const auto& b : bins) if (b.size() > 21 && b[0] == 1) rateBack = f32(b, 9);
        CHECK(std::fabs(rateBack - rate1) < 1, "zoom 1: the whole capture again (%.0f)", rateBack);
        ws.close();

        // the mode options: ADS-B (standard 12) has setCorrection; a visitor may not set them
        lan.timeoutMs = 20000;   // a mode switch can take seconds on a loaded runner, as in the loop over every mode
        r = lan.post("mode", "{\"id\":\"adsb\"}");
        if (r.status == 0) r = lan.post("mode", "{\"id\":\"adsb\"}");
        lan.timeoutMs = 4000;
        CHECK(r.status == 200, "ADS-B: %d %s", r.status, r.body.c_str());
        r = lan.get("options");
        CHECK(r.status == 200 && has(r.body, "\"schemas\":{") && has(r.body, "\"12\":[") && has(r.body, "setCorrection"), "options: %.200s", r.body.c_str());
        r = lan.post("option", "{\"name\":\"setCorrection\",\"value\":1}");
        CHECK(r.status == 200 && has(r.body, "\"setCorrection\":1"), "set an option: %d %s", r.status, r.body.c_str());
        r = lan.post("option", "{\"name\":\"setCorrection\",\"value\":1.5}");
        CHECK(r.status == 400, "a fraction for an int option refused: %d", r.status);
        r = lan.post("option", "{\"name\":\"setNoSuchThing\",\"value\":1}");
        CHECK(r.status == 400, "an unknown option refused: %d", r.status);
        r = lan.post("option", "{\"name\":\"setCorrection\"}");
        CHECK(r.status == 400, "no value refused");
        r = lan.get("status");
        CHECK(has(r.body, "\"options\":{\"standard\":12,\"values\":{\"setCorrection\":1}"), "status shows the option: %.300s", r.body.c_str());
        for (int i = 0; i < 40; i++) { ConfigFile saved; saved.load(cfgPath); if (saved.has("opt.12.setCorrection")) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        { ConfigFile saved; saved.load(cfgPath); CHECK(saved.get("opt.12.setCorrection") == "1", "the option is kept in the settings file"); }
        CHECK(has(r.body, "\"audioSource\":\"monitor\""), "ADS-B is heard through the monitor");

        // digital voice: M17 (Codec2 is part of OnAir) on the test signal; the browser gets the decoded speech, not the demodulator
        lan.timeoutMs = 20000;   // a mode switch can take seconds on a loaded runner, as in the loop over every mode
        r = lan.post("mode", "{\"id\":\"m17\"}");
        if (r.status == 0) r = lan.post("mode", "{\"id\":\"m17\"}");
        lan.timeoutMs = 4000;
        CHECK(r.status == 200, "M17: %d %s", r.status, r.body.c_str());
        r = lan.get("status");
        CHECK(has(r.body, "\"audioSource\":\"voice\""), "M17 audio is the voice: %.200s", r.body.c_str());
        Ws wv;
        CHECK(wv.open(port, "lan", "", ""), "WebSocket for voice");
        wv.send("{\"type\":\"sub\",\"panel\":false,\"status\":false,\"spectrum\":false,\"audio\":true,\"rate\":8000}");
        texts.clear(); bins.clear();
        wv.collect(8000, texts, bins);
        size_t samples = 0;
        double energy = 0;
        for (const auto& b : bins) if (b.size() > 5 && b[0] == 2) for (size_t i = 5; i + 1 < b.size(); i += 2) { const int16_t v = (int16_t)((unsigned char)b[i] | ((unsigned char)b[i + 1] << 8)); energy += (double)v * v; samples++; }
        const double rms = samples ? std::sqrt(energy / (double)samples) : 0;
        printf("webapp: M17 voice in the browser: %zu samples at 8 kHz in 8 s, rms %.0f\n", samples, rms);
        if (dect2::m17Codec2Available()) CHECK(samples >= 8000 && rms > 100, "M17 speech reaches the browser (%zu samples, rms %.0f)", samples, rms);
        else printf("webapp: Codec2 is not available in this build: the M17 speech check is skipped\n");
        wv.close();
        lan.timeoutMs = 20000;   // a mode switch can take seconds on a loaded runner, as in the loop over every mode
        r = lan.post("mode", "{\"id\":\"fm\"}");
        if (r.status == 0) r = lan.post("mode", "{\"id\":\"fm\"}");
        lan.timeoutMs = 4000;
        r = lan.get("status");
        CHECK(has(r.body, "\"audioSource\":\"fm\""), "FM is heard through its own audio");
    }
    { Ws ws; CHECK(!ws.open(port, "wrong", "", "") || true, "opening with a wrong csrf"); std::vector<std::string> t, b; if (ws.s != kBadSock) ws.collect(500, t, b); CHECK(ws.closeCode == 4003 || has(ws.buf, "csrf"), "a wrong csrf closes the WebSocket (%d)", ws.closeCode); ws.close(); }

    // ---- an internet visitor (through the trusted proxy header): login needed
    Client net{port};
    net.forwardedFor = "203.0.113.5";
    net.origin = "http://127.0.0.1:" + std::to_string(port);
    r = net.get("info");
    CHECK(r.status == 200 && has(r.body, "\"role\":\"none\"") && has(r.body, "\"needs\":\"setup\""), "no password yet: the visitor is told: %s", r.body.c_str());
    r = net.get("status");
    CHECK(r.status == 401, "status needs a login: %d", r.status);
    r = net.post("login", "{\"password\":\"anything\"}");
    CHECK(r.status == 401 && has(r.body, "no password set"), "login without a password set: %d %s", r.status, r.body.c_str());
    r = net.post("setup", "{\"password\":\"from the internet\"}");
    CHECK(r.status == 401 || r.status == 403, "a visitor cannot set the first password: %d", r.status);
    { Ws ws; ws.open(port, "public", "", "203.0.113.5"); std::vector<std::string> t, b; if (ws.s != kBadSock) ws.collect(500, t, b); CHECK(ws.closeCode == 4001 || has(ws.buf, "login"), "no WebSocket without a login (%d)", ws.closeCode); ws.close(); }
    // the LAN owner sets a password
    r = lan.post("setup", "{\"password\":\"short\"}");
    CHECK(r.status == 400, "a short password is refused");
    r = lan.post("setup", "{\"password\":\"correct horse battery\"}");
    CHECK(r.status == 200, "the first password: %d %s", r.status, r.body.c_str());
    r = lan.post("setup", "{\"password\":\"another one here\"}");
    CHECK(r.status == 409, "setup only once");
    for (int i = 0; i < 150; i++) { ConfigFile saved; saved.load(cfgPath); if (!saved.get("app_password_hash").empty()) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    { ConfigFile saved; saved.load(cfgPath); CHECK(saved.get("app_password_hash").compare(0, 7, "pbkdf2$") == 0 && !has(saved.text(), "correct horse"), "the hash is in the file, the password is not (hash '%.12s', password in the file: %d, %zu bytes)",
                    saved.get("app_password_hash").c_str(), (int)has(saved.text(), "correct horse"), saved.text().size()); }
    r = net.post("login", "{\"password\":\"wrong one\"}");
    CHECK(r.status == 401 && has(r.body, "wrong password"), "wrong password: %d %s", r.status, r.body.c_str());
    r = net.post("login", "{\"password\":\"correct horse battery\"}");
    CHECK(r.status == 200 && has(r.head, "Set-Cookie: onair_session=") && has(r.head, "HttpOnly") && has(r.head, "SameSite=Strict"), "login sets the cookie: %d %s", r.status, r.head.c_str());
    {
        const std::string sc = r.header("Set-Cookie");
        if (sc.size() > 14) net.cookie = sc.substr(14, sc.find(';') - 14);
        CHECK(net.cookie.size() == 64, "cookie token (%zu) from '%s'", net.cookie.size(), sc.c_str());
    }
    CHECK(net.session() && net.csrf.size() == 32, "the logged-in session has its own csrf");
    r = net.get("status");
    CHECK(r.status == 200, "the owner from the internet reads status");
    r = net.post("frequency", "{\"hz\":99500000}");
    CHECK(r.status == 200, "... and tunes: %d %s", r.status, r.body.c_str());
    { Client stale = net; stale.csrf = "lan"; r = stale.post("frequency", "{\"hz\":99500000}"); CHECK(r.status == 403, "a logged-in owner cannot use the 'lan' csrf: %d", r.status); }
    {
        Ws ws;
        CHECK(ws.open(port, net.csrf, net.cookie, "203.0.113.5"), "the logged-in owner's WebSocket opens");
        std::vector<std::string> t, b;
        ws.collect(800, t, b);
        CHECK(countOf(t, "\"owner\":true") == 1, "hello says owner");
        ws.close();
    }
    r = net.post("logout", "{}");
    CHECK(r.status == 200 && has(r.head, "Max-Age=0"), "logout clears the cookie");
    r = net.get("status");
    CHECK(r.status == 401, "logged out");
    net.cookie.clear();
    // ---- public mode: visitors watch; public_tune lets them tune; the cap
    r = lan.post("settings", "{\"app_public\":true}");
    CHECK(r.status == 200, "public on");
    Client vis{port};
    vis.forwardedFor = "198.51.100.20";
    vis.origin = net.origin;
    r = vis.get("info");
    CHECK(has(r.body, "\"role\":\"viewer\""), "a visitor: %s", r.body.c_str());
    CHECK(vis.session() && vis.csrf == "public", "visitor csrf");
    r = vis.get("status");
    CHECK(r.status == 200, "a visitor reads status");
    r = vis.get("panel");
    CHECK(r.status == 200 && has(r.body, "\"data\":"), "a visitor reads the FM panel (a web-safe mode): %.120s", r.body.c_str());
    r = vis.post("frequency", "{\"hz\":100e6}");
    CHECK(r.status == 403, "read-only: %d", r.status);
    r = vis.get("settings");
    CHECK(r.status == 403, "no settings for a visitor");
    r = vis.post("settings", "{\"app_public\":false}");
    CHECK(r.status == 403, "a visitor cannot change settings");
    r = vis.post("record/start", "{}");
    CHECK(r.status == 403, "a visitor cannot record");
    r = vis.post("tx/start", "{}");
    CHECK(r.status == 403, "a visitor cannot transmit");
    r = lan.post("settings", "{\"app_public_tune\":true}");
    r = vis.post("frequency", "{\"hz\":100100000}");
    CHECK(r.status == 200, "public_tune: a visitor tunes: %d %s", r.status, r.body.c_str());
    r = vis.post("mode", "{\"family\":11,\"sub\":\"\"}");
    CHECK(r.status == 200, "a visitor picks ADS-B (web-safe): %d %s", r.status, r.body.c_str());
    r = vis.post("mode", "{\"family\":9,\"sub\":\"\"}");
    CHECK(r.status == 403 && has(r.body, "private"), "a visitor cannot switch to DMR (voice, not on the allow-list): %d %s", r.status, r.body.c_str());
    r = vis.post("tx/start", "{}");
    CHECK(r.status == 403, "still no transmit");
    // the owner switches to DMR: the visitor's panel goes private
    r = lan.post("mode", "{\"family\":9,\"sub\":\"\"}");
    CHECK(r.status == 200, "owner -> DMR");
    r = vis.get("panel");
    CHECK(has(r.body, "\"private\":true"), "the visitor sees no DMR data: %s", r.body.c_str());
    lan.post("mode", "{\"id\":\"fm\"}");
    // the visitor cap (3 after the settings change): the 4th WebSocket gets busy
    {
        Ws a, b, c, d;
        std::vector<std::string> t, bn;
        CHECK(a.open(port, "public", "", "198.51.100.21") && b.open(port, "public", "", "198.51.100.22") && c.open(port, "public", "", "198.51.100.23"), "three visitors connect");
        a.collect(300, t, bn); b.collect(300, t, bn); c.collect(300, t, bn);
        CHECK(countOf(t, "\"type\":\"hello\"") == 3, "three hellos (%d)", countOf(t, "\"type\":\"hello\""));
        CHECK(countOf(t, "\"owner\":false") == 3 && countOf(t, "\"control\":true") == 3, "visitors with public_tune may control");
        d.open(port, "public", "", "198.51.100.24");
        t.clear();
        d.collect(800, t, bn);
        CHECK(d.closeCode == 4029 || countOf(t, "\"code\":\"busy\"") == 1, "the 4th visitor is told busy (%d)", d.closeCode);
        r = lan.get("viewers");
        CHECK(has(r.body, "\"viewers\":3"), "3 viewers counted: %s", r.body.c_str());
        a.close(); b.close(); c.close(); d.close();
        for (int i = 0; i < 30; i++) { r = lan.get("viewers"); if (has(r.body, "\"viewers\":0")) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        CHECK(has(r.body, "\"viewers\":0"), "viewers counted away: %s", r.body.c_str());
    }
    r = lan.post("settings", "{\"app_public\":false,\"app_public_tune\":false}");
    r = vis.get("status");
    CHECK(r.status == 401, "public off: the visitor is out again");
    // recording on the server (owner): starts and stops, the file appears
    cfg.set("rec_dir", (dir / "rec").string());
    host.editConfig([&](ConfigFile& c) { c.set("rec_dir", (dir / "rec").string()); });
    r = lan.post("record/start", "{\"kind\":\"iq\"}");
    CHECK(r.status == 200, "record start: %d %s", r.status, r.body.c_str());
    r = lan.post("record/start", "{\"kind\":\"iq\"}");
    CHECK(r.status == 501 && has(r.body, "running"), "a second recording is refused");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    r = lan.post("record/stop", "{}");
    CHECK(r.status == 200, "record stop");
    {
        int files = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir / "rec", ec)) if (e.path().extension() == ".cf32" && e.file_size(ec) > 0) files++;
        CHECK(files == 1, "one IQ file was written (%d)", files);
    }
    r = lan.post("stop", "{}");
    CHECK(r.status == 200, "stop");
    supStop = true;
    if (sup.joinable()) sup.join();
    web.stop();
    host.setWebHub(nullptr);
    hub.stop();
    host.shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    printf("webapp: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
