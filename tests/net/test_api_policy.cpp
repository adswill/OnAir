// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure rules of the remote API (token, rate limit, sizes, routes and the refusal list, message lines, rigctl) and the parts built on them:
// the HTTP server with token on loopback, rate limit and the new endpoints, the rigctl TCP server, the UDP output. Plus a guard: the sources of
// the API must not mention the key store (transmit is allowed since 3.2: the operator decides).
#include "../../core/src/net/tcp_client.h"
#include "dect2/net/api_policy.h"
#include "dect2/net/api_servers.h"
#include "dect2/net/remote_api.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
using namespace dect2;
using namespace dect2::apipol;

#include "../common/test_util.h"

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

// ---------------------------------------------------------------- pure functions
static void testToken() {
    CHECK(!tokenUsable("") && !tokenUsable("short") && !tokenUsable("0123456789abcde"), "short tokens are not usable");
    CHECK(tokenUsable("0123456789abcdef"), "16 characters are usable");
    CHECK(!tokenUsable("0123456789abcde f") && !tokenUsable(std::string("0123456789abcdef\n")), "spaces and control characters are not usable");
    CHECK(!tokenUsable(std::string(300, 'a')), "300 characters are too many");
    CHECK(tokenEqual("0123456789abcdef", "0123456789abcdef"), "equal");
    CHECK(!tokenEqual("0123456789abcdef", "0123456789abcdeg") && !tokenEqual("0123456789abcdef", "0123456789abcde") && !tokenEqual("", "0123456789abcdef"), "unequal");
    CHECK(tokenEqual("", ""), "two empty strings are equal (but never usable)");
    CHECK(!tokenEqual(std::string(400, 'a'), std::string(400, 'a')), "over-long values are never equal");
    CHECK(extractToken("Bearer abc", "") == "abc" && extractToken("bearer   abc  ", "") == "abc", "bearer header");
    CHECK(extractToken("Basic abc", "x=1&token=zz&y=2") == "zz", "query token when the header is not bearer");
    CHECK(extractToken("", "mytoken=1").empty() && extractToken("", "").empty() && extractToken("Bearer ", "").empty(), "no token");
    const std::string t = "0123456789abcdef";
    CHECK(authorise(true, false, "", "") == Auth::Allow, "loopback needs nothing by default");
    CHECK(authorise(true, true, t, t) == Auth::Allow && authorise(true, true, t, "x") == Auth::NeedToken, "loopback with the local switch");
    CHECK(authorise(true, true, "", "") == Auth::NoTokenConfigured, "local switch without a token is refused");
    CHECK(authorise(false, false, t, t) == Auth::Allow && authorise(false, false, t, "") == Auth::NeedToken, "remote needs it");
    CHECK(authorise(false, false, "", "") == Auth::NoTokenConfigured && authorise(false, false, "short", "short") == Auth::NoTokenConfigured, "remote without a usable token is refused");
}

static void testRate() {
    RateLimiter r(2, 3);
    int ok = 0;
    for (int i = 0; i < 10; i++) ok += r.allow("a", 100.0);
    CHECK(ok == 3, "burst of 3 then refused (%d)", ok);
    CHECK(!r.allow("a", 100.2), "0.2 s later still refused (0.4 token)");
    CHECK(r.allow("a", 100.6), "0.6 s later one more");
    CHECK(r.allow("b", 100.0), "another address has its own bucket");
    CHECK(r.retryAfter("a", 100.6) > 0.3 && r.retryAfter("a", 100.6) <= 0.5, "retry after %.2f", r.retryAfter("a", 100.6));
    CHECK(r.allow("a", 200.0) && r.allow("a", 200.0) && r.allow("a", 200.0) && !r.allow("a", 200.0), "refills to the burst, not beyond");
    RateLimiter small(1, 1, 4);
    for (int i = 0; i < 100; i++) small.allow("k" + std::to_string(i), 1.0);
    CHECK(small.keys() <= 4, "bounded memory (%zu keys)", small.keys());
    for (int i = 0; i < 3; i++) r.allow("t", 50.0);
    CHECK(!r.allow("t", 40.0), "a clock that goes backwards does not give tokens");
}

static void testSizes() {
    CHECK(checkSizes(100, "", 10) == Size::Ok && checkSizes(8192, "4096", 512) == Size::Ok, "limits are inclusive");
    CHECK(checkSizes(8193, "", 10) == Size::HeaderTooLarge, "header");
    CHECK(checkSizes(100, "4097", 10) == Size::BodyTooLarge && checkSizes(100, "99999999999999999999", 10) == Size::BodyTooLarge, "body");
    CHECK(checkSizes(100, "-1", 10) == Size::BadLength && checkSizes(100, "12a", 10) == Size::BadLength && checkSizes(100, "0x10", 10) == Size::BadLength, "malformed length");
    CHECK(checkSizes(100, "", 513) == Size::PathTooLong, "path");
}

static void testRoutes() {
    CHECK(parseRoute("GET", "/api/v1/status").ep == Ep::Status && parseRoute("GET", "/api/v1/status").status == 200, "status");
    CHECK(parseRoute("POST", "/api/v1/status").status == 405 && parseRoute("POST", "/api/v1/status").allow == "GET", "POST on a GET endpoint");
    CHECK(parseRoute("GET", "/api/v1/gain").status == 405 && parseRoute("GET", "/api/v1/gain").allow == "POST", "GET on a POST endpoint");
    CHECK(parseRoute("POST", "/api/v1/recording/start").ep == Ep::RecordStart, "recording start");
    CHECK(parseRoute("GET", "/").ep == Ep::Page && parseRoute("GET", "/api/v1").ep == Ep::Index && parseRoute("GET", "/api/v1/").ep == Ep::Index, "index pages");
    CHECK(parseRoute("GET", "/api/v2/status").status == 404 && has(parseRoute("GET", "/api/v2/status").msg, "version"), "other API versions");
    CHECK(parseRoute("GET", "/api/v10/status").status == 404 && parseRoute("GET", "/api/v0/status").status == 404, "v10 and v0 are not v1");
    CHECK(parseRoute("GET", "/api/v1/nothing").status == 404, "unknown");
    CHECK(parseRoute("GET", "/api/v1/x/anything").ep == Ep::Extra && parseRoute("DELETE", "/api/v1/x/anything").status == 405, "extras");
    const char* bad[] = {"", "api/v1/status", "/api/v1/../status", "/api//v1/status", "/api/v1/./status", "/api/v1/st%61tus", "/api/v1/status\\x", "/api/v1/sta tus", "/api/v1/status\r\nX: y", "/api/v1/stat\xc3\xa9"};
    for (const char* b : bad) CHECK(parseRoute("GET", b).status == 400, "malformed path refused: %.30s", b);
    CHECK(parseRoute("GET", std::string("/api/v1/") + std::string(600, 'a')).status == 400, "long path");
    CHECK(parseRoute("GET", std::string("/api/v1/status\0x", 16)).status == 400, "NUL byte");
    // the refusal list: nothing that touches keys / credentials / security (transmit is NOT on it any more)
    const char* refused[] = {"/api/v1/voice/keys", "/api/v1/key", "/api/v1/keys/import",
                             "/api/v1/credentials", "/api/v1/password", "/api/v1/secret", "/api/v1/token", "/api/v1/security", "/api/v1/settings", "/api/v1/config", "/api/v1/decrypt",
                             "/api/v1/x/voice-key", "/api/v1/rotator/encrypt", "/api/v1/login", "/api/v1/admin"};
    for (const char* r : refused) CHECK(parseRoute("POST", r).status == 403 && parseRoute("GET", r).status == 403, "refused: %s", r);
    for (const char* t : {"/api/v1/tx", "/api/v1/tx/start", "/api/v1/tx/stop", "/api/v1/transmit", "/api/v1/ptt", "/api/v1/x/tx"}) CHECK(parseRoute("POST", t).status != 403 && parseRoute("GET", t).status != 403, "transmit paths are not refused: %s", t);
    CHECK(parseRoute("GET", "/api/v1/tx").status == 200 && parseRoute("GET", "/api/v1/tx").ep == Ep::TxStatus, "GET /api/v1/tx is the transmit status");
    CHECK(parseRoute("POST", "/api/v1/tx/start").status == 200 && parseRoute("POST", "/api/v1/tx/start").ep == Ep::TxStart && parseRoute("POST", "/api/v1/tx/stop").ep == Ep::TxStop, "tx start and stop are routes");
    for (const std::string& line : endpointList()) CHECK(!pathRefused(line.substr(line.find(' ') + 1)), "an existing endpoint is not refused: %s", line.c_str());
    CHECK(!pathRefused("/api/v1/mute") && !pathRefused("/api/v1/memories") && !pathRefused("/api/v1/samplerate") && !pathRefused("/api/v1/x/stations"), "ordinary names pass");
    CHECK(endpointList().size() >= 28, "endpoint list");
}

static void testLines() {
    automation::Event e("adsb.aircraft");
    e.at(1760000000.5, "ADS-B", 1090e6);
    e.str("icao", "4B1814").num("alt", 36000).flag("onGround", false).str("note", "quote \" back\\slash\nnewline\x01");
    const std::string l = eventLine(e, 7);
    CHECK(l.find('\n') == std::string::npos && l[0] == '{' && l.back() == '}', "one line");
    CHECK(has(l, "\"v\":1") && has(l, "\"seq\":7") && has(l, "\"type\":\"adsb.aircraft\"") && has(l, "\"mode\":\"ADS-B\"") && has(l, "\"freqHz\":1090000000"), "head: %s", l.c_str());
    CHECK(has(l, "\"icao\":\"4B1814\"") && has(l, "\"alt\":36000") && has(l, "\"onGround\":false") && has(l, "\\\"") && has(l, "\\\\") && has(l, "\\n") && has(l, "\\u0001"), "fields: %s", l.c_str());
    automation::Event big("pager.message");
    big.str("text", std::string(1900, 'x'));
    const std::string t = eventLine(big, 1, kMaxUdpLine);
    CHECK(t.size() <= kMaxUdpLine && has(t, "\"truncated\":true") && !has(t, "xxxx"), "truncated to fit a datagram (%zu)", t.size());
    automation::Event nan("x");
    nan.num("v", NAN);
    CHECK(has(eventLine(nan), "\"v\":null"), "NaN becomes null");
    EventRing ring(4);
    for (int i = 0; i < 6; i++) ring.push("L" + std::to_string(i));
    uint64_t cur = 0;
    std::vector<std::string> out;
    bool gap = false;
    ring.since(cur, 100, out, &gap);
    CHECK(gap && out.size() == 4 && out[0] == "L2" && out[3] == "L5" && cur == 6, "ring keeps the last 4, tells of the gap");
    out.clear();
    ring.since(cur, 100, out, &gap);
    CHECK(!gap && out.empty(), "nothing new");
    cur = UINT64_MAX;
    ring.since(cur, 100, out, &gap);
    CHECK(cur == 6 && out.empty(), "from now");
    ring.push("L6"); ring.push("L7");
    ring.since(cur, 1, out, &gap);
    CHECK(out.size() == 1 && out[0] == "L6" && cur == 7, "max honoured");
    cur = 99;
    ring.since(cur, 10, out, &gap);
    CHECK(cur == 8, "a cursor from the future is clamped");
}

struct FakeRadio : RigctlRadio {
    double hz = 145500000;
    std::string mode = "FM";
    int pb = 15000;
    bool getFreq(double& f) override { f = hz; return true; }
    bool setFreq(double f) override { hz = f; return true; }
    bool getMode(std::string& m, int& p) override { m = mode; p = pb; return true; }
    bool setMode(const std::string& m, int p) override { mode = m; pb = p; return true; }
    bool getStrengthDb(double& db) override { db = -42; return true; }
};

struct TxRadio : FakeRadio {   // a radio that can transmit: T 1 / T 0 / t reach it
    bool on = false;
    bool getPtt(bool& o) override { o = on; return true; }
    bool setPtt(bool o) override { on = o; return true; }
};

static void testRigctlPtt() {
    TxRadio r;
    bool q = false;
    auto h = [&](const std::string& l) { return rigctlHandle(l, r, q); };
    CHECK(h("t") == "0\n", "t: not transmitting");
    CHECK(h("T 1") == "RPRT 0\n" && r.on && h("t") == "1\n", "T 1 keys the transmitter the host has set up");
    CHECK(h("T 0") == "RPRT 0\n" && !r.on && h("t") == "0\n", "T 0 stops it");
    CHECK(h("T ON") == "RPRT 0\n" && r.on && h("T OFF") == "RPRT 0\n" && !r.on, "T ON / OFF");
}

static void testRigctl() {
    FakeRadio r;
    bool q = false;
    auto h = [&](const std::string& l) { return rigctlHandle(l, r, q); };
    CHECK(h("f") == "145500000\n", "f");
    CHECK(h("F 433920000") == "RPRT 0\n" && r.hz == 433920000, "F");
    CHECK(h("F 433920000.5\r\n") == "RPRT 0\n", "F with fraction and CRLF");
    CHECK(h("m") == "FM\n15000\n", "m");
    CHECK(h("M USB 2400") == "RPRT 0\n" && r.mode == "USB" && r.pb == 2400, "M");
    CHECK(h("M usb 0") == "RPRT 0\n" && r.mode == "USB", "lower-case mode");
    CHECK(h("M BOGUS 1") == "RPRT -1\n" && h("M USB") == "RPRT -1\n" && h("M USB x") == "RPRT -1\n", "bad M");
    CHECK(h("t") == "0\n", "t is receive while the radio says it is not transmitting");
    CHECK(h("T 1") == "RPRT -11\n" && h("T ON") == "RPRT -11\n" && h("T 0") == "RPRT 0\n" && h("T") == "RPRT -1\n" && h("T x") == "RPRT -1\n", "T 1 on a radio that cannot key: RPRT -11 (the fake radio has no transmitter)");
    CHECK(h("s") == "0\nVFOA\n" && h("S 0 VFOA") == "RPRT 0\n" && h("S 1 VFOB") == "RPRT -11\n", "split is off only");
    CHECK(h("l STRENGTH") == "-42\n" && h("l SWR") == "RPRT -11\n", "l");
    CHECK(h("F") == "RPRT -1\n" && h("F abc") == "RPRT -1\n" && h("F -5") == "RPRT -1\n" && h("F 0") == "RPRT -1\n" && h("F nan") == "RPRT -1\n" && h("F inf") == "RPRT -1\n" && h("F 1e99") == "RPRT -1\n", "bad F");
    CHECK(h("f extra") == "RPRT -1\n", "extra argument");
    CHECK(h("") == "RPRT -1\n" && h("   ") == "RPRT -1\n", "empty line");
    CHECK(h("zzz") == "RPRT -4\n" && h("\\unknown") == "RPRT -4\n" && h("w AT") == "RPRT -4\n" && h("P") == "RPRT -4\n", "unknown commands: -4");
    CHECK(h(std::string(300, 'f')) == "RPRT -8\n" && h(std::string("f\0x", 3)) == "RPRT -8\n" && h("f\x01") == "RPRT -8\n", "oversized and binary lines");
    CHECK(!q && h("q").empty() && q, "q quits without a reply");
    q = false;
    CHECK(h("\\chk_vfo") == "0\n" && h("+f") == "433920000\n", "chk_vfo, extended prefix answered plainly");
    // an HTTP request line (a web page POSTing text/plain to the rigctl port) ends the connection and runs nothing
    q = false;
    const double before = r.hz;
    CHECK(h("POST / HTTP/1.1").empty() && q && r.hz == before, "HTTP request line: hang up");
    q = false;
    CHECK(h("GET /x HTTP/1.0\r\n").empty() && q, "HTTP/1.0 request line: hang up");
    q = false;
    // header lines (a request that arrives without its request line in the same read) hang up too
    for (const char* hdr : {"Host: 127.0.0.1:4532", "host:localhost", "Origin: http://evil.example", "Content-Type: text/plain", "Content-Length: 12"}) {
        q = false;
        CHECK(h(hdr).empty() && q && r.hz == before, "HTTP header line: hang up");
    }
    q = false;

    // with a token: reads are open, changes need "\auth <token>" on the connection
    RigctlSession sess1; sess1.required = true; sess1.token = "0123456789abcdef";
    auto hs = [&](const std::string& l) { return rigctlHandle(l, r, q, sess1); };
    const double hz0 = r.hz;
    const std::string mode0 = r.mode;
    CHECK(hs("f") != "RPRT -1\n" && hs("m") == mode0 + "\n" + std::to_string(r.pb) + "\n", "reads stay open");
    CHECK(hs("F 100000000") == "RPRT -1\n" && r.hz == hz0, "F refused before auth");
    CHECK(hs("M USB 2400") == "RPRT -1\n" && r.mode == mode0, "M refused before auth");
    CHECK(hs("S 1 VFOB") == "RPRT -1\n" && hs("T 1") == "RPRT -1\n" && hs("T 0") == "RPRT 0\n", "S 1 and T 1 refused before auth, the no-op T 0 is fine");
    CHECK(hs("\\auth wrong") == "RPRT -1\n" && hs("F 100000000") == "RPRT -1\n" && r.hz == hz0, "a wrong token does not open it");
    CHECK(hs("\\auth 0123456789abcdef") == "RPRT 0\n" && hs("F 100000000") == "RPRT 0\n" && r.hz == 100000000, "the token opens changes");
    // three wrong tokens hang up
    RigctlSession s2; s2.required = true; s2.token = "0123456789abcdef";
    q = false;
    CHECK(rigctlHandle("\\auth a", r, q, s2) == "RPRT -1\n" && !q && rigctlHandle("\\auth b", r, q, s2) == "RPRT -1\n" && !q, "two wrong tokens");
    CHECK(rigctlHandle("\\auth c", r, q, s2).empty() && q, "the third wrong token hangs up");
    // required with no token (LAN without a token): nothing can authenticate
    RigctlSession s3; s3.required = true;
    q = false;
    const double hz1 = r.hz;
    CHECK(rigctlHandle("\\auth ", r, q, s3) == "RPRT -1\n" && rigctlHandle("\\auth x", r, q, s3) == "RPRT -1\n" && rigctlHandle("F 1000000", r, q, s3) == "RPRT -1\n" && r.hz == hz1, "no token set: changes always refused");
}

// ---------------------------------------------------------------- sockets
static std::string httpReq(int port, const std::string& raw, int ms = 1500) {
    std::string err;
    sock_t s = tcpConnect("127.0.0.1", port, 1000, err);
    if (s == kBadSock) return std::string();
    sockRecvTimeout(s, ms);
    sockSendAll(s, raw.data(), raw.size());
    std::string out;
    char b[4096];
    for (;;) {
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        out.append(b, (size_t)k);
    }
    sockClose(s);
    return out;
}
static int statusOf(const std::string& r) { return r.compare(0, 5, "HTTP/") == 0 && r.size() > 12 ? atoi(r.c_str() + 9) : 0; }
static std::string bodyOf(const std::string& r) { const size_t i = r.find("\r\n\r\n"); return i == std::string::npos ? std::string() : r.substr(i + 4); }
static std::string getReq(int port, const std::string& path, const std::string& extra = "") { return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + extra + "\r\n"; }
static std::string postReq(int port, const std::string& path, const std::string& body, const std::string& extra = "") {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}

struct Host : RemoteApiHost {
    std::mutex mu;
    std::string last;
    double vol = 0.5;
    bool muted = false;
    EventRing ring;
    std::string statusJson() override { return "{\"running\":false}"; }
    std::string devicesJson() override { return "[]"; }
    std::string modesJson() override { return "[]"; }
    std::string telemetryJson(uint64_t& s) override { s = 1; return "{}"; }
    std::string spectrumJson(int, uint64_t& s) override { s = 1; return "{}"; }
    bool start(int, std::string&) override { return true; }
    bool stop(std::string&) override { return true; }
    bool setMode(int, const std::string&, std::string&) override { return true; }
    bool setFrequencyHz(double hz, std::string&) override { last = "freq " + std::to_string((long long)hz); return true; }
    bool setSampleRateHz(double, std::string&) override { return true; }
    bool setGainDb(double, std::string&) override { return true; }
    std::string memoriesJson() override { return "{\"memories\":[{\"name\":\"NOAA\",\"freqHz\":162550000}]}"; }
    std::string audioJson() override { return "{\"volume\":" + std::to_string(vol) + ",\"muted\":" + (muted ? "true" : "false") + "}"; }
    bool setVolume(double v, std::string&) override { vol = v; last = "volume"; return true; }
    bool setMute(bool on, std::string&) override { muted = on; last = "mute"; return true; }
    bool setSquelch(bool on, bool au, double db, std::string&) override { last = std::string("squelch ") + (on ? "1" : "0") + (au ? "a" : "m") + std::to_string((int)db); return true; }
    bool recordStart(const std::string& k, std::string& err) override { if (k == "audio") { err = "no audio source"; return false; } last = "rec " + k; return true; }
    bool eventsSince(uint64_t& c, size_t n, std::vector<std::string>& out, bool& gap) override { ring.since(c, n, out, &gap); return true; }
};

static void testServer() {
    Host h;
    RemoteApiServer srv(h);
    RemoteApiConfig c;
    c.on = true; c.port = 0; c.token = "0123456789abcdef"; c.requireTokenLocal = true;
    CHECK(srv.configure(c) && srv.status().listening, "starts: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;
    const std::string tok = "Authorization: Bearer 0123456789abcdef\r\n";
    // token on loopback
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status"))) == 401, "loopback without a token");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status", "Authorization: Bearer wrongwrongwrong12\r\n"))) == 401, "wrong token");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status", tok))) == 200, "right token");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status?token=0123456789abcdef"))) == 200, "token in the query");
    // no CORS headers, ever
    const std::string r = httpReq(port, getReq(port, "/api/v1/status", tok));
    CHECK(!has(r, "Access-Control"), "no CORS headers");
    CHECK(statusOf(httpReq(port, "OPTIONS /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.example\r\n" + tok + "\r\n")) == 403 || statusOf(httpReq(port, "OPTIONS /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n" + tok + "\r\n")) == 405, "preflight is not granted");
    // routes and refusal list through the server
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/tx", tok))) == 501, "tx: a host without a transmitter answers 501, not 403");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/voice/keys", "{}", tok))) == 403, "keys refused");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/../etc/passwd", tok))) == 400, "dot-dot");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v2/status", tok))) == 404, "v2");
    CHECK(has(bodyOf(httpReq(port, getReq(port, "/api/v1", tok))), "GET /api/v1/memories"), "the index lists the endpoints");
    // new endpoints
    CHECK(has(bodyOf(httpReq(port, getReq(port, "/api/v1/memories", tok))), "NOAA"), "memories");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/scheduler", tok))) == 501, "an endpoint the host does not offer: 501");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/volume", "{\"level\":0.25}", tok))) == 200 && h.vol == 0.25, "volume");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/volume", "{\"level\":1.5}", tok))) == 400 && statusOf(httpReq(port, postReq(port, "/api/v1/volume", "{\"level\":\"loud\"}", tok))) == 400, "bad volume");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/mute", "{\"on\":true}", tok))) == 200 && h.muted, "mute");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/mute", "{\"on\":1}", tok))) == 400, "mute needs a boolean");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/squelch", "{\"on\":true,\"auto\":false,\"db\":-60}", tok))) == 200 && h.last == "squelch 1m-60", "squelch: %s", h.last.c_str());
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/squelch", "{\"on\":true,\"db\":-500}", tok))) == 400, "squelch out of range");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/recording/start", "{\"kind\":\"iq\"}", tok))) == 200 && h.last == "rec iq", "recording");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/recording/start", "{\"kind\":\"audio\"}", tok))) == 400, "host refusal is a 400");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/recording/start", "{\"kind\":\"../x\"}", tok))) == 400, "bad kind");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/autogain", "{\"on\":true}", tok))) == 400, "unsupported setter: 400 with the host's message");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/bandwidth", "{}", tok))) == 400, "bandwidth needs a value");
    // events
    std::string e = bodyOf(httpReq(port, getReq(port, "/api/v1/events", tok)));
    CHECK(has(e, "\"next\":0") && has(e, "\"events\":[]"), "events from now: %s", e.c_str());
    automation::Event ev("pager.message");
    ev.at(1, "POCSAG", 153350000);
    ev.str("text", "hello");
    h.ring.push(eventLine(ev, 0));
    h.ring.push(eventLine(ev, 1));
    e = bodyOf(httpReq(port, getReq(port, "/api/v1/events?since=0&max=1", tok)));
    CHECK(has(e, "\"next\":1") && has(e, "\"text\":\"hello\"") && has(e, "\"gap\":false"), "events: %s", e.c_str());
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/events?since=abc", tok))) == 400, "bad cursor");
    // malformed and oversized requests
    CHECK(statusOf(httpReq(port, "GARBAGE\r\n\r\n")) >= 400, "garbage");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status", "X-Fill: " + std::string(9000, 'a') + "\r\n" + tok))) == 431, "oversized headers");
    CHECK(statusOf(httpReq(port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 5000\r\n" + tok + "\r\n" + std::string(5000, '{'))) >= 400, "oversized body");
    CHECK(statusOf(httpReq(port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: -1\r\n" + tok + "\r\n")) >= 400, "negative length");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/gain", "{\"db\":", tok))) == 400, "truncated JSON");
    CHECK(statusOf(httpReq(port, postReq(port, "/api/v1/gain", "{\"db\":{\"a\":1}}", tok))) == 400, "nested JSON");
    CHECK(statusOf(httpReq(port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n" + tok + "\r\n{}")) == 415, "POST needs the JSON content type");
    CHECK(statusOf(httpReq(port, getReq(port, "/api/v1/status", "Origin: http://evil.example\r\n" + tok))) == 403, "cross-origin");
    CHECK(statusOf(httpReq(port, "GET /api/v1/status HTTP/1.1\r\nHost: attacker.example\r\n" + tok + "\r\n")) == 403, "DNS rebinding");
    // rate limit on a server of its own: a flood gets 429 and Retry-After
    {
        Host h2;
        RemoteApiServer srv2(h2);
        RemoteApiConfig c2 = c;
        c2.ratePerSec = 0.1; c2.rateBurst = 5;   // a slow refill: on a loaded runner (a second per request) the 30 requests stay a flood
        CHECK(srv2.configure(c2), "second server");
        const int p2 = srv2.config().port;
        int n429 = 0, n200 = 0;
        std::string last429;
        for (int i = 0; i < 30; i++) {
            const std::string rr = httpReq(p2, getReq(p2, "/api/v1/status", tok), 800);
            if (statusOf(rr) == 429) { n429++; last429 = rr; }
            n200 += statusOf(rr) == 200;
        }
        CHECK(n429 > 10 && n200 >= 5 && n200 <= 12, "the rate limit: %d x 200, %d x 429", n200, n429);
        CHECK(has(last429, "Retry-After"), "Retry-After");
    }
    // token changes take effect at once
    c.token = "fedcba9876543210";
    srv.configure(c);
    const int port2 = srv.config().port;
    CHECK(statusOf(httpReq(port2, getReq(port2, "/api/v1/status", tok))) == 401 && statusOf(httpReq(port2, getReq(port2, "/api/v1/status", "Authorization: Bearer fedcba9876543210\r\n"))) == 200, "a new token replaces the old at once");
}

static void testRigctlServer() {
    FakeRadio r;
    RigctlServer srv(r);
    RigctlConfig c; c.on = true; c.port = 0;
    CHECK(srv.configure(c), "rigctl listens: %s", srv.error().c_str());
    std::string err;
    sock_t s = tcpConnect("127.0.0.1", srv.port(), 1000, err);
    CHECK(s != kBadSock, "connect");
    if (s == kBadSock) return;
    sockRecvTimeout(s, 1500);
    auto ask = [&](const std::string& l) {
        sockSendAll(s, l.data(), l.size());
        char b[256];
        const long long k = sockRecv(s, b, sizeof b);
        return k > 0 ? std::string(b, (size_t)k) : std::string();
    };
    CHECK(ask("f\n") == "145500000\n", "f over TCP");
    CHECK(ask("F 162550000\n") == "RPRT 0\n" && r.hz == 162550000, "F over TCP");
    CHECK(ask("T 1\n") == "RPRT -11\n", "T 1 refused over TCP");
    std::string two = ask("f\nm\n");
    if (two.find("FM\n") == std::string::npos) two += ask("");
    CHECK(two.find("162550000\n") == 0 && two.find("FM\n15000\n") != std::string::npos, "pipelined commands: %s", two.c_str());
    CHECK(ask(std::string(400, 'a')) == "RPRT -8\n", "an endless line is refused and the connection closed");
    sockClose(s);
    sock_t s2 = tcpConnect("127.0.0.1", srv.port(), 1000, err);
    sockRecvTimeout(s2, 1500);
    sockSendAll(s2, "q\n", 2);
    char b[8];
    CHECK(sockRecv(s2, b, sizeof b) <= 0, "q closes the connection");
    sockClose(s2);
    CHECK(srv.configure(RigctlConfig()) && srv.port() == 0, "off");

    // a token set: changes need \auth first; an HTTP request (a web page POSTing to the port) is dropped and runs nothing
    RigctlConfig ct; ct.on = true; ct.port = 0; ct.allowLan = true; ct.token = "0123456789abcdef";
    CHECK(srv.configure(ct), "rigctl with a token listens");
    r.hz = 145500000;
    sock_t s3 = tcpConnect("127.0.0.1", srv.port(), 1000, err);
    CHECK(s3 != kBadSock, "connect 3");
    if (s3 == kBadSock) return;
    sockRecvTimeout(s3, 1500);
    auto ask3 = [&](const std::string& l) {
        sockSendAll(s3, l.data(), l.size());
        char b3[256];
        const long long k = sockRecv(s3, b3, sizeof b3);
        return k > 0 ? std::string(b3, (size_t)k) : std::string();
    };
    CHECK(ask3("f\n") == "145500000\n", "reads open with a token");
    CHECK(ask3("F 100000000\n") == "RPRT -1\n" && r.hz == 145500000, "F refused without \\auth");
    CHECK(ask3("\\auth 0123456789abcdef\n") == "RPRT 0\n" && ask3("F 100000000\n") == "RPRT 0\n" && r.hz == 100000000, "F after \\auth");
    sockClose(s3);
    sock_t s4 = tcpConnect("127.0.0.1", srv.port(), 1000, err);
    CHECK(s4 != kBadSock, "connect 4");
    if (s4 == kBadSock) return;
    sockRecvTimeout(s4, 1500);
    const std::string post = "POST / HTTP/1.1\r\nHost: 127.0.0.1:4532\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nF 145500000\n";
    sockSendAll(s4, post.data(), post.size());
    char b4[64];
    CHECK(sockRecv(s4, b4, sizeof b4) <= 0, "an HTTP request: the connection is closed, nothing is sent back");
    CHECK(r.hz == 100000000, "and nothing was executed");
    sockClose(s4);
    CHECK(srv.configure(RigctlConfig()) && srv.port() == 0, "off again");
}

static void testUdp() {
    CHECK(validUdpHost("255.255.255.255") && validUdpHost("127.0.0.1") && !validUdpHost("256.1.1.1") && !validUdpHost("example.com") && !validUdpHost("1.2.3") && !validUdpHost("1.2.3.4.5") && !validUdpHost(""), "host check");
    netInit();
    sock_t rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(bind(rx, (sockaddr*)&a, sizeof a) == 0, "bind");
    socklen_t al = sizeof a;
    getsockname(rx, (sockaddr*)&a, &al);
    sockRecvTimeout(rx, 1500);
    UdpOut u;
    UdpOutConfig c; c.on = true; c.host = "127.0.0.1"; c.port = ntohs(a.sin_port);
    CHECK(u.configure(c), "udp configure: %s", u.error().c_str());
    automation::Event ev("ais.position");
    ev.at(5, "AIS", 161975000);
    ev.str("mmsi", "244660000");
    CHECK(u.send(eventLine(ev)), "send");
    char b[2048];
    const long long k = sockRecv(rx, b, sizeof b);
    CHECK(k > 0 && b[k - 1] == '\n' && has(std::string(b, (size_t)k), "\"mmsi\":\"244660000\""), "one JSON line per datagram");
    CHECK(!u.send(std::string(1500, 'x')), "over one datagram: refused");
    c.host = "not an address";
    CHECK(!u.configure(c) && !u.error().empty() && !u.send("x"), "bad address: nothing sent");
    c.host = "127.0.0.1"; c.port = 0;
    CHECK(!u.configure(c), "port 0");
    sockClose(rx);
}

// ---------------------------------------------------------------- guard: what the API sources may not mention
static void testGuard(const char* argv0) {
    (void)argv0;
    std::string root = __FILE__;
    const size_t at = root.rfind("tests/net/");
    root = at == 0 || at == std::string::npos ? std::string(".") : root.substr(0, at - 1);
    const char* files[] = {"/core/src/net/remote_api.cpp", "/core/include/dect2/net/remote_api.h", "/core/src/net/api_policy.cpp", "/core/include/dect2/net/api_policy.h", "/core/src/net/api_servers.cpp",
                           "/core/include/dect2/net/api_servers.h", "/app/platform/remote_api_app.cpp"};
    const char* words[] = {"keystore", "key_store", "key store", "KeyStore", "voice_key", "VoiceKey", "voicekey", "importKey", "userKeys"};
    for (const char* f : files) {
        std::ifstream in(root + f);
        if (!in) { printf("note: %s not readable here, guard skipped\n", f); continue; }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string t = ss.str();
        for (const char* w : words) CHECK(t.find(w) == std::string::npos, "%s mentions '%s'", f, w);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    testToken();
    testRate();
    testRigctlPtt();
    testSizes();
    testRoutes();
    testLines();
    testRigctl();
    testServer();
    testRigctlServer();
    testUdp();
    testGuard(argv[0]);
    return testSummary("api_policy");
}
