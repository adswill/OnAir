// The remote-control API against a socket client: the HTTP endpoints, the request limits, the Host / Origin / token checks, the WebSocket
// (handshake, telemetry and spectrum pushes, ping, protocol errors) and, at the end, the real engine on the generated DVB-T2 signal behind it.
#include "../../core/src/net/tcp_client.h"
#include "dect2/engine/engine.h"
#include "dect2/net/remote_api.h"
#include "dect2/results/rx_json.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#ifdef DECT2_TLS
#include <openssl/ssl.h>
#endif
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif
using namespace dect2;

#include "../common/test_util.h"

// ---------------------------------------------------------------- a host that records what the API asked for
struct FakeHost : RemoteApiHost {
    std::mutex mu;
    std::string last;                 // "name arg"
    std::atomic<uint64_t> telSeq{1}, specSeq{1};
    bool failGain = false;
    bool withPage = false;                   // the optional parts of the host interface (headless server)
    std::atomic<int> audioOpened{0}, audioClosed{0}, audioReads{0};
    std::string indexHtml() override { return withPage ? "<!doctype html><title>fake</title>" : std::string(); }
    bool spectrumBinary(int bins, uint64_t& seq, std::string& f) override {
        if (!withPage) return false;
        seq = specSeq;
        f.assign(1, '\x01');
        f.append(20, '\0');
        f.append((size_t)bins, 'x');
        return true;
    }
    int audioOpen(int rate) override { if (!withPage) return -1; audioOpened++; return 7 + (rate == 8000 ? 1 : 0); }
    bool audioRead(int, std::string& pcm) override { audioReads++; pcm.assign(8, '\x05'); return true; }
    void audioClose(int) override { audioClosed++; }
    int handleExtra(const std::string& m, const std::string& path, const std::map<std::string, JsonVal>& body, std::string& json) override {
        if (!withPage) return 0;
        if (path == "/api/v1/x/ping" && m == "GET") { json = "{\"pong\":true}"; return 200; }
        if (path == "/api/v1/x/echo" && m == "POST") { auto it = body.find("v"); if (it == body.end()) { json = "give v"; return 400; } json = "{\"v\":" + jsonNum(it->second.num) + "}"; return 200; }
        return 0;
    }
    std::string statusJson() override { return "{\"running\":false,\"fake\":true}"; }
    std::string devicesJson() override { return "[{\"index\":0,\"name\":\"Synthetic\"}]"; }
    std::string modesJson() override { return "[{\"family\":0,\"name\":\"DVB\"}]"; }
    std::string telemetryJson(uint64_t& seq) override { seq = telSeq; return "{\"seq\":" + std::to_string(seq) + ",\"snrDb\":21.5}"; }
    std::string spectrumJson(int maxBins, uint64_t& seq) override { seq = specSeq; return "{\"bins\":" + std::to_string(maxBins) + ",\"dbfs\":[-60,-59]}"; }
    void rec(const std::string& s) { std::lock_guard<std::mutex> lk(mu); last = s; }
    std::string got() { std::lock_guard<std::mutex> lk(mu); return last; }
    bool start(int d, std::string&) override { rec("start " + std::to_string(d)); return true; }
    bool stop(std::string&) override { rec("stop"); return true; }
    bool setMode(int f, const std::string& s, std::string&) override { rec("mode " + std::to_string(f) + " " + s); return true; }
    bool setFrequencyHz(double hz, std::string&) override { rec("freq " + std::to_string((long long)hz)); return true; }
    bool setSampleRateHz(double hz, std::string&) override { rec("rate " + std::to_string((long long)hz)); return true; }
    bool setGainDb(double db, std::string& err) override { if (failGain) { err = "gain not supported by this radio"; return false; } rec("gain " + std::to_string((int)db)); return true; }
};

// ---------------------------------------------------------------- client helpers
static std::string readAll(sock_t s, int ms) {   // until the server closes (or ms pass)
    std::string r;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[4096];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        r.append(b, (size_t)k);
    }
    return r;
}
static sock_t dial(const std::string& host, int port) { std::string e; return tcpConnect(host, port, 2000, e); }
struct Resp { int status = 0; std::string head, body; bool has(const char* h) const { std::string l = head, n = h; for (char& c : l) c = (char)tolower((unsigned char)c); for (char& c : n) c = (char)tolower((unsigned char)c); return l.find(n) != std::string::npos; } };
static Resp parseResp(const std::string& r) {
    Resp o;
    if (r.compare(0, 9, "HTTP/1.1 ") == 0) o.status = atoi(r.c_str() + 9);
    const size_t e = r.find("\r\n\r\n");
    o.head = r.substr(0, e);
    if (e != std::string::npos) o.body = r.substr(e + 4);
    return o;
}
static Resp http(const std::string& host, int port, const std::string& raw, int waitMs = 6000) {
    sock_t s = dial(host, port);
    if (s == kBadSock) return Resp();
    sockSendAll(s, raw.data(), raw.size());
    const std::string r = readAll(s, waitMs);
    sockClose(s);
    return parseResp(r);
}
static std::string get(const std::string& path, const std::string& extra = "", const std::string& host = "127.0.0.1:%d") { return "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" + extra + "\r\n"; }
static std::string withPort(std::string s, int port) { const size_t i = s.find("%d"); if (i != std::string::npos) s.replace(i, 2, std::to_string(port)); return s; }
static std::string post(const std::string& path, const std::string& body, int port, const std::string& ctype = "application/json", const std::string& extra = "") {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n" + (ctype.empty() ? "" : "Content-Type: " + ctype + "\r\n") + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// a masked client frame
static std::string cframe(int op, const std::string& pl, bool mask = true, bool fin = true) {
    std::string f;
    f.push_back((char)((fin ? 0x80 : 0) | op));
    const uint8_t m = mask ? 0x80 : 0;
    if (pl.size() < 126) f.push_back((char)(m | pl.size()));
    else { f.push_back((char)(m | 126)); f.push_back((char)(pl.size() >> 8)); f.push_back((char)(pl.size() & 0xFF)); }
    const char key[4] = {0x12, 0x34, 0x56, 0x78};
    if (mask) f.append(key, 4);
    for (size_t i = 0; i < pl.size(); i++) f.push_back(mask ? (char)(pl[i] ^ key[i & 3]) : pl[i]);
    return f;
}
// one server frame within ms; false when none came / the connection closed
static bool sframe(sock_t s, std::string& buf, int& op, std::string& pl, int ms = 3000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        if (buf.size() >= 2) {
            size_t len = (uint8_t)buf[1] & 0x7F, hl = 2;
            if (len == 126 && buf.size() >= 4) { len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3]; hl = 4; }
            if (!(((uint8_t)buf[1] & 0x7F) == 126 && buf.size() < 4) && buf.size() >= hl + len) {
                op = (uint8_t)buf[0] & 0x0F;
                pl = buf.substr(hl, len);
                buf.erase(0, hl + len);
                return true;
            }
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[8192];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) return false;
        buf.append(b, (size_t)k);
    }
}
static const char* kKey = "dGhlIHNhbXBsZSBub25jZQ==";
static std::string wsHandshake(int port, const std::string& host, const std::string& extra = "") {
    return "GET /api/v1/ws HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + kKey + "\r\n" + extra + "\r\n";
}
// opens a WebSocket on loopback; the 101 response is consumed, anything after it stays in buf
static sock_t wsOpen(int port, std::string& buf, int* status = nullptr, const std::string& extra = "") {
    sock_t s = dial("127.0.0.1", port);
    if (s == kBadSock) return s;
    const std::string rq = wsHandshake(port, "127.0.0.1:" + std::to_string(port), extra);
    sockSendAll(s, rq.data(), rq.size());
    buf.clear();
    const auto t0 = std::chrono::steady_clock::now();
    size_t e;
    while ((e = buf.find("\r\n\r\n")) == std::string::npos && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[4096];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        buf.append(b, (size_t)k);
    }
    const Resp r = parseResp(buf);
    if (status) *status = r.status;
    if (e != std::string::npos) buf.erase(0, e + 4);
    if (r.status == 101) {
        CHECK(r.has("sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "handshake accept key: %s", r.head.c_str());
    }
    return s;
}

static std::string hex(const std::string& b) { std::string o; char t[4]; for (unsigned char c : b) { snprintf(t, sizeof t, "%02x", c); o += t; } return o; }

static void testHelpers() {
    CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(abc)");
    CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
    CHECK(hex(sha1("The quick brown fox jumps over the lazy dog")) == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12", "sha1 fox");
    CHECK(hex(sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba", "sha1 1000 x a (two blocks)");
    CHECK(base64("foobar") == "Zm9vYmFy" && base64("fooba") == "Zm9vYmE=" && base64("foob") == "Zm9vYg==" && base64("") == "", "base64");
    CHECK(wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 accept key");
    CHECK(jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001", "json escape");
    CHECK(jsonNum(NAN) == "null" && jsonNum(1.5) == "1.5", "json num");
    std::map<std::string, JsonVal> j;
    CHECK(parseFlatJson("{\"hz\": 474e6, \"s\":\"x\\u00e4\", \"b\":true, \"n\":null}", j) && j["hz"].num == 474e6 && j["s"].str == "x\xC3\xA4" && j["b"].b && j["n"].kind == JsonVal::Null, "flat json");
    CHECK(parseFlatJson("{}", j) && j.empty() && parseFlatJson("  { }  ", j), "empty object");
    for (const char* bad : {"", "[1]", "{\"a\":[1]}", "{\"a\":{\"b\":1}}", "{\"a\":1,}", "{\"a\":1} x", "{a:1}", "{\"a\":1e999}", "{\"a\":\"\x01\"}", "{\"a\":--1}", "{\"a\":"})
        CHECK(!parseFlatJson(bad, j), "should reject: %s", bad);
    CHECK(remoteApiNewToken().size() == 24 && remoteApiNewToken() != remoteApiNewToken(), "token");
    const std::string f = wsFrame(1, std::string(300, 'x'));
    CHECK((uint8_t)f[0] == 0x81 && (uint8_t)f[1] == 126 && f.size() == 4 + 300, "ws frame header");
}

static void testHttp(FakeHost& h, RemoteApiServer& srv, int port) {
    (void)srv;
    Resp r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port));
    CHECK(r.status == 200 && r.body == "{\"running\":false,\"fake\":true}", "status: %d %s", r.status, r.body.c_str());
    CHECK(r.has("content-type: application/json") && r.has("content-length:") && !r.has("access-control-"), "headers (no CORS): %s", r.head.c_str());
    r = http("127.0.0.1", port, withPort(get("/api/v1/devices"), port)); CHECK(r.status == 200 && r.body.find("Synthetic") != std::string::npos, "devices");
    r = http("127.0.0.1", port, withPort(get("/api/v1/modes"), port)); CHECK(r.status == 200 && r.body.find("DVB") != std::string::npos, "modes");
    r = http("127.0.0.1", port, withPort(get("/api/v1/telemetry"), port)); CHECK(r.status == 200 && r.body.find("21.5") != std::string::npos, "telemetry");
    r = http("127.0.0.1", port, withPort(get("/api/v1/spectrum?bins=300"), port)); CHECK(r.status == 200 && r.body.find("\"bins\":300") != std::string::npos, "spectrum bins: %s", r.body.c_str());
    r = http("127.0.0.1", port, withPort(get("/api/v1/spectrum?bins=999999"), port)); CHECK(r.body.find("\"bins\":4096") != std::string::npos, "bins clamp");
    r = http("127.0.0.1", port, withPort(get("/api/v1"), port)); CHECK(r.status == 200 && r.body.find("endpoints") != std::string::npos, "index");

    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 474}", port)); CHECK(r.status == 200 && h.got() == "freq 474000000", "frequency: %d %s", r.status, h.got().c_str());
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"hz\": 1090000000}", port)); CHECK(r.status == 200 && h.got() == "freq 1090000000", "frequency hz");
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 9}", port)); CHECK(r.status == 200 && h.got() == "freq 9000000", "9 MHz is passed on: %d %s", r.status, h.got().c_str());
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 6000}", port)); CHECK(r.status == 200 && h.got() == "freq 6000000000", "6 GHz is passed on: %d %s", r.status, h.got().c_str());
    r = http("127.0.0.1", port, post("/api/v1/samplerate", "{\"msps\": 2.4}", port)); CHECK(r.status == 200 && h.got() == "rate 2400000", "samplerate: %s", h.got().c_str());
    r = http("127.0.0.1", port, post("/api/v1/gain", "{\"db\": 33}", port)); CHECK(r.status == 200 && h.got() == "gain 33", "gain");
    r = http("127.0.0.1", port, post("/api/v1/mode", "{\"family\": 5, \"sub\": \"ft8\"}", port)); CHECK(r.status == 200 && h.got() == "mode 5 ft8", "mode: %s", h.got().c_str());
    r = http("127.0.0.1", port, post("/api/v1/start", "{\"device\": 2}", port)); CHECK(r.status == 200 && h.got() == "start 2", "start");
    r = http("127.0.0.1", port, post("/api/v1/start", "", port)); CHECK(r.status == 200 && h.got() == "start -1", "start with no body");
    r = http("127.0.0.1", port, post("/api/v1/stop", "", port)); CHECK(r.status == 200 && h.got() == "stop", "stop");
    h.failGain = true;
    r = http("127.0.0.1", port, post("/api/v1/gain", "{\"db\": 10}", port)); CHECK(r.status == 400 && r.body.find("not supported") != std::string::npos, "host error is returned: %d %s", r.status, r.body.c_str());
    h.failGain = false;

    // bad input
    h.rec("untouched");
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": -5}", port)); CHECK(r.status == 400, "negative frequency %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": \"474\"}", port)); CHECK(r.status == 400, "string frequency %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 474", port)); CHECK(r.status == 400, "broken json %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/mode", "{\"family\": 1.5}", port)); CHECK(r.status == 400, "fractional family %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/gain", "{\"db\": 1e9}", port)); CHECK(r.status == 400, "absurd gain %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 474}", port, "text/plain")); CHECK(r.status == 415, "text/plain POST (what a web page can send without preflight) %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 474}", port, "")); CHECK(r.status == 415, "no content type %d", r.status);
    CHECK(h.got() == "untouched", "a refused request reached the host: %s", h.got().c_str());
    r = http("127.0.0.1", port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\n\r\n"); CHECK(r.status == 411, "no Content-Length %d", r.status);
    r = http("127.0.0.1", port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"); CHECK(r.status == 501, "chunked %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/gain", std::string(5000, ' '), port)); CHECK(r.status == 413, "5000 byte body %d", r.status);
    r = http("127.0.0.1", port, "POST /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nContent-Length: 99999999999\r\n\r\n"); CHECK(r.status == 400, "absurd length %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/api/v1/status", "X-Pad: " + std::string(9000, 'a') + "\r\n"), port)); CHECK(r.status == 431, "9 KB header %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/" + std::string(600, 'a')), port)); CHECK(r.status == 400, "long target %d", r.status);
    r = http("127.0.0.1", port, "GET /api/v1/status HTTP/1.1\r\n\r\n"); CHECK(r.status == 400, "no Host %d", r.status);
    r = http("127.0.0.1", port, "garbage\r\n\r\n"); CHECK(r.status == 400, "garbage request line %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/nothing"), port)); CHECK(r.status == 404, "404 %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/api/v1/gain"), port)); CHECK(r.status == 405 && r.has("allow: post"), "GET on a POST endpoint %d", r.status);
    r = http("127.0.0.1", port, "POST /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n"); CHECK(r.status == 405, "POST on a GET endpoint %d", r.status);
    r = http("127.0.0.1", port, "DELETE /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n"); CHECK(r.status == 405, "DELETE %d", r.status);
    r = http("127.0.0.1", port, "OPTIONS /api/v1/gain HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nOrigin: http://evil.example\r\nAccess-Control-Request-Method: POST\r\n\r\n");
    CHECK(r.status != 200 && !r.has("access-control-allow"), "a CORS preflight must be refused without allow headers (%d)", r.status);

    // browsers: another site's page, DNS rebinding
    r = http("127.0.0.1", port, withPort(get("/api/v1/status", "Origin: http://evil.example\r\n"), port)); CHECK(r.status == 403, "foreign Origin %d", r.status);
    r = http("127.0.0.1", port, get("/api/v1/status", "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n", "127.0.0.1:" + std::to_string(port))); CHECK(r.status == 200, "own Origin %d", r.status);
    r = http("127.0.0.1", port, get("/api/v1/status", "", "rebind.evil.example:" + std::to_string(port))); CHECK(r.status == 403, "foreign Host on loopback %d", r.status);
    r = http("127.0.0.1", port, get("/api/v1/status", "", "localhost:" + std::to_string(port))); CHECK(r.status == 200, "localhost Host %d", r.status);
    r = http("127.0.0.1", port, get("/api/v1/status", "", "[::1]:" + std::to_string(port))); CHECK(r.status == 200, "[::1] Host %d", r.status);

    // the connection cap: 16 silent connections hold every slot, the 17th is turned away; they time out by themselves
    std::vector<sock_t> idle;
    for (int i = 0; i < 16; i++) idle.push_back(dial("127.0.0.1", port));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port), 2000);
    CHECK(r.status == 503, "17th connection %d", r.status);
    // a silent client is cut off at 5 s with 408
    const std::string slow = readAll(idle[0], 7000);
    CHECK(slow.find("408") != std::string::npos, "silent connection got: %.40s", slow.c_str());
    for (sock_t s : idle) if (s != kBadSock) sockClose(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port)); CHECK(r.status == 200, "the server serves again after the flood %d", r.status);
}

static void testWs(FakeHost& h, int port) {
    std::string buf, pl;
    int op = 0, st = 0;
    sock_t s = wsOpen(port, buf, &st);
    CHECK(st == 101 && s != kBadSock, "handshake status %d", st);
    CHECK(sframe(s, buf, op, pl) && op == 1 && pl.find("\"type\":\"telemetry\"") != std::string::npos && pl.find("21.5") != std::string::npos, "first telemetry push: %s", pl.c_str());
    h.telSeq++;   // a new report: pushed
    CHECK(sframe(s, buf, op, pl) && pl.find("\"seq\":2") != std::string::npos, "push of the next report: %s", pl.c_str());
    CHECK(!sframe(s, buf, op, pl, 600), "no push without a new report");
    // ping -> pong with the same payload
    sockSendAll(s, cframe(9, "hi").data(), cframe(9, "hi").size());
    CHECK(sframe(s, buf, op, pl) && op == 10 && pl == "hi", "pong");
    // subscribe to the spectrum
    const std::string sub = cframe(1, "{\"spectrum\":true,\"bins\":64}");
    sockSendAll(s, sub.data(), sub.size());
    bool gotSub = false, gotSpec = false;
    for (int i = 0; i < 6 && !(gotSub && gotSpec); i++) {
        if (!sframe(s, buf, op, pl)) break;
        if (pl.find("\"type\":\"subscribed\"") != std::string::npos) gotSub = true;
        if (pl.find("\"type\":\"spectrum\"") != std::string::npos) { gotSpec = pl.find("\"bins\":64") != std::string::npos; }
    }
    CHECK(gotSub && gotSpec, "subscribe: confirmation %d, spectrum push %d", gotSub, gotSpec);
    // telemetry off
    const std::string off = cframe(1, "{\"telemetry\":false}");
    sockSendAll(s, off.data(), off.size());
    h.telSeq++;
    bool tel = false;
    for (int i = 0; i < 8; i++) { if (!sframe(s, buf, op, pl, 700)) break; if (pl.find("\"type\":\"telemetry\"") != std::string::npos) tel = true; }
    CHECK(!tel, "telemetry still pushed after switching it off");
    // not JSON: an error message, the connection stays
    const std::string bad = cframe(1, "hello");
    sockSendAll(s, bad.data(), bad.size());
    bool err = false;
    for (int i = 0; i < 8; i++) { if (!sframe(s, buf, op, pl, 700)) break; if (pl.find("\"type\":\"error\"") != std::string::npos) { err = true; break; } }
    CHECK(err, "a text that is not JSON gets an error message");
    // close handshake
    const std::string cl = cframe(8, std::string("\x03\xe8", 2));
    sockSendAll(s, cl.data(), cl.size());
    bool closed = false;
    for (int i = 0; i < 20; i++) { if (!sframe(s, buf, op, pl, 700)) break; if (op == 8) { closed = true; break; } }
    CHECK(closed, "close frame answered");
    sockClose(s);

    // protocol errors: unmasked client frame (1002), oversize (1009), binary (1003), fragment (1003)
    struct Case { const char* name; std::string frame; int code; } cases[] = {
        {"unmasked", cframe(1, "x", false), 1002}, {"oversize", cframe(1, std::string(5000, 'x')), 1009}, {"binary", cframe(2, "x"), 1003}, {"fragment", cframe(1, "x", true, false), 1003}};
    for (auto& c : cases) {
        std::string b2;
        sock_t t = wsOpen(port, b2);
        sframe(t, b2, op, pl);   // the first telemetry
        sockSendAll(t, c.frame.data(), c.frame.size());
        int code = 0;
        for (int i = 0; i < 10; i++) { if (!sframe(t, b2, op, pl, 800)) break; if (op == 8 && pl.size() >= 2) { code = ((uint8_t)pl[0] << 8) | (uint8_t)pl[1]; break; } }
        CHECK(code == c.code, "%s frame: close code %d, expected %d", c.name, code, c.code);
        sockClose(t);
    }
    // handshake errors
    Resp r = http("127.0.0.1", port, "GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"); CHECK(r.status == 400, "no key %d", r.status);
    r = http("127.0.0.1", port, wsHandshake(port, "127.0.0.1:" + std::to_string(port), "Origin: http://evil.example\r\n")); CHECK(r.status == 403, "cross-site WebSocket %d", r.status);
    r = http("127.0.0.1", port, wsHandshake(port, "evil.example")); CHECK(r.status == 403, "WebSocket with a rebinding Host %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/api/v1/ws"), port)); CHECK(r.status == 426, "plain GET on the ws path %d", r.status);
    // at most 8 WebSockets
    std::vector<sock_t> socks;
    for (int i = 0; i < 8; i++) { std::string b3; socks.push_back(wsOpen(port, b3)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r = http("127.0.0.1", port, wsHandshake(port, "127.0.0.1:" + std::to_string(port))); CHECK(r.status == 503, "9th WebSocket %d", r.status);
    for (sock_t k : socks) sockClose(k);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string b4; int st2 = 0; sock_t again = wsOpen(port, b4, &st2); CHECK(st2 == 101, "WebSocket slots are freed (%d)", st2); sockClose(again);
}

#ifndef _WIN32
static std::string lanAddress() {   // an IPv4 address of this computer that is not loopback ("" when none)
    ifaddrs* ifs = nullptr;
    std::string r;
    if (getifaddrs(&ifs) != 0) return r;
    for (ifaddrs* a = ifs; a && r.empty(); a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char b[64];
        inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, b, sizeof b);
        if (strncmp(b, "169.254.", 8) != 0) r = b;
    }
    freeifaddrs(ifs);
    return r;
}
#else
static std::string lanAddress() { return ""; }
#endif


// the parts the headless server adds: its page, the binary spectrum, audio messages, the extra endpoints
static void testServerParts(FakeHost& h, int port) {
    h.withPage = true;
    Resp r = http("127.0.0.1", port, withPort(get("/"), port));
    CHECK(r.status == 200 && r.body.find("<title>fake</title>") != std::string::npos, "GET / serves the host's page: %d", r.status);
    CHECK(r.has("content-security-policy: default-src 'none'") && r.has("content-type: text/html"), "the page carries a CSP and its type");
    r = http("127.0.0.1", port, post("/", "{}", port));
    CHECK(r.status == 404, "POST / is not the page: %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/api/v1/x/ping"), port)); CHECK(r.status == 200 && r.body.find("pong") != std::string::npos, "extra GET: %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/x/echo", "{\"v\":4.5}", port)); CHECK(r.status == 200 && r.body.find("4.5") != std::string::npos, "extra POST: %d %s", r.status, r.body.c_str());
    r = http("127.0.0.1", port, post("/api/v1/x/echo", "{}", port)); CHECK(r.status == 400 && r.body.find("give v") != std::string::npos, "extra POST error: %d", r.status);
    r = http("127.0.0.1", port, post("/api/v1/x/echo", "{\"v\":1}", port, "text/plain")); CHECK(r.status == 415, "extra POST needs JSON: %d", r.status);
    r = http("127.0.0.1", port, withPort(get("/api/v1/x/nothing"), port)); CHECK(r.status == 404, "unknown extra: %d", r.status);

    std::string buf, pl;
    int op = 0, st = 0;
    sock_t s = wsOpen(port, buf, &st);
    CHECK(st == 101 && s != kBadSock, "handshake %d", st);
    const std::string sub = cframe(1, "{\"telemetry\":false,\"spectrum\":true,\"bins\":100,\"fps\":20,\"fmt\":\"u8\",\"audio\":true,\"arate\":8000}");
    sockSendAll(s, sub.data(), sub.size());
    bool gotSub = false, gotBin = false, gotAudio = false;
    for (int i = 0; i < 12 && !(gotSub && gotBin && gotAudio); i++) {
        if (!sframe(s, buf, op, pl)) break;
        if (op == 1 && pl.find("\"type\":\"subscribed\"") != std::string::npos) gotSub = pl.find("\"fps\":20") != std::string::npos && pl.find("\"audio\":true") != std::string::npos;
        if (op == 2 && !pl.empty() && pl[0] == 1) gotBin = pl.size() == 1 + 20 + 100;
        if (op == 2 && !pl.empty() && pl[0] == 2) gotAudio = pl.size() == 9;
    }
    CHECK(gotSub, "the subscription is confirmed with fps and audio");
    CHECK(gotBin, "a binary spectrum message of 1 + 20 + bins bytes");
    CHECK(gotAudio, "an audio message: type byte and the samples");
    CHECK(h.audioOpened == 1, "one audio listener opened (%d)", h.audioOpened.load());
    const std::string off = cframe(1, "{\"audio\":false}");
    sockSendAll(s, off.data(), off.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(h.audioClosed == 1, "switching audio off closes the listener (%d)", h.audioClosed.load());
    const int reads = h.audioReads;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(h.audioReads == reads, "no more audio reads once it is off");
    sockSendAll(s, cframe(1, "{\"audio\":true}").data(), cframe(1, "{\"audio\":true}").size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sockClose(s);   // a closed connection must close its listener too
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(h.audioClosed == h.audioOpened, "a dropped connection closes its audio listener (%d of %d)", h.audioClosed.load(), h.audioOpened.load());
    h.withPage = false;
    r = http("127.0.0.1", port, withPort(get("/"), port)); CHECK(r.status == 404, "no page without a host page: %d", r.status);
}

static void testWsSlowClient(FakeHost& h, int port) {
    // a client that never reads: the server must not stall other connections or grow without bound
    h.withPage = true;
    std::string buf;
    int st = 0;
    sock_t s = wsOpen(port, buf, &st);
    const std::string sub = cframe(1, "{\"telemetry\":false,\"spectrum\":true,\"bins\":2048,\"fps\":30,\"fmt\":\"u8\"}");
    sockSendAll(s, sub.data(), sub.size());
    for (int i = 0; i < 40; i++) { h.specSeq++; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    Resp r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port));
    CHECK(r.status == 200, "the API still answers while a WebSocket client does not read: %d", r.status);
    sockClose(s);
    h.withPage = false;
}


// TLS: a certificate made with the openssl program, an OpenSSL client (certificate not checked), the page and an API call over it
static void testTls() {
    FakeHost h;
    h.withPage = true;
    RemoteApiServer bad(h);
    RemoteApiConfig bc; bc.on = true; bc.port = 0; bc.tlsCert = "/nonexistent/c.pem"; bc.tlsKey = "/nonexistent/k.pem";
    CHECK(!bad.configure(bc) && !bad.status().lastError.empty(), "a certificate that is not there is refused: %s", bad.status().lastError.c_str());
#ifndef DECT2_TLS
    CHECK(bad.status().lastError.find("no TLS support") != std::string::npos, "a build without OpenSSL says so: %s", bad.status().lastError.c_str());
    printf("TLS: not built (DECT2_WITH_TLS=OFF), only the refusal was tested\n");
#else
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("onair_tls_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const std::string crt = (dir / "c.pem").string(), key = (dir / "k.pem").string();
    const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost -keyout '" + key + "' -out '" + crt + "' >/dev/null 2>&1";
    if (system(cmd.c_str()) != 0 || !fs::exists(crt)) { printf("TLS: the openssl program is not available, skipped\n"); fs::remove_all(dir); return; }
    RemoteApiServer srv(h);
    RemoteApiConfig c; c.on = true; c.port = 0; c.tlsCert = crt; c.tlsKey = key;
    CHECK(srv.configure(c) && srv.status().listening && srv.status().tls, "TLS server starts: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    auto tlsGet = [&](const std::string& req, bool ws, std::string& out) {
        out.clear();
        sock_t s = dial("127.0.0.1", port);
        if (s == kBadSock) return false;
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, (int)s);
        bool ok = SSL_connect(ssl) == 1;
        if (ok) {
            SSL_write(ssl, req.data(), (int)req.size());
            if (ws) { char b[2048]; const int k = SSL_read(ssl, b, sizeof b); if (k > 0) out.assign(b, (size_t)k); }
            else for (;;) { char b[4096]; const int k = SSL_read(ssl, b, sizeof b); if (k <= 0) break; out.append(b, (size_t)k); }
        }
        SSL_shutdown(ssl); SSL_free(ssl); sockClose(s);
        return ok;
    };
    std::string out;
    CHECK(tlsGet(withPort(get("/"), port), false, out) && out.find("200") != std::string::npos && out.find("<title>fake</title>") != std::string::npos, "the page over TLS");
    CHECK(tlsGet(withPort(get("/api/v1/status"), port), false, out) && out.find("\"fake\":true") != std::string::npos, "an API call over TLS");
    CHECK(tlsGet(wsHandshake(port, "127.0.0.1:" + std::to_string(port)), true, out) && out.find("101 Switching") != std::string::npos, "a WebSocket over TLS (wss)");
    // plain HTTP to the TLS port gets no answer that looks like HTTP
    Resp r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port), 1500);
    CHECK(r.status == 0, "plain HTTP to a TLS port is not answered (%d)", r.status);
    // the server still serves after the broken handshake
    CHECK(tlsGet(withPort(get("/api/v1/status"), port), false, out) && out.find("200") != std::string::npos, "TLS still works after a plain request");
    SSL_CTX_free(ctx);
    srv.configure(RemoteApiConfig());
    fs::remove_all(dir);
#endif
}

static void testAuth() {
    FakeHost h;
    RemoteApiServer srv(h);
    RemoteApiConfig c;
    c.on = true; c.port = 0; c.lan = true; c.token = "short";
    CHECK(!srv.configure(c) && !srv.status().listening && srv.status().lastError.find("token") != std::string::npos, "LAN without a real token must not start (%s)", srv.status().lastError.c_str());
    c.token = "0123456789abcdef0123";
    CHECK(srv.configure(c) && srv.status().listening, "LAN with a token starts: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;
    // loopback needs no token
    Resp r = http("127.0.0.1", port, withPort(get("/api/v1/status"), port)); CHECK(r.status == 200, "loopback without token %d", r.status);
    const std::string lan = lanAddress();
    if (lan.empty()) { printf("SKIP: no non-loopback address on this computer, the token check for remote peers was not exercised\n"); return; }
    const std::string host = lan + ":" + std::to_string(port);
    r = http(lan, port, get("/api/v1/status", "", host)); CHECK(r.status == 401 && r.has("www-authenticate"), "remote peer without token %d", r.status);
    r = http(lan, port, get("/api/v1/status", "Authorization: Bearer wrong-token-0123456789\r\n", host)); CHECK(r.status == 401, "remote peer with a wrong token %d", r.status);
    r = http(lan, port, get("/api/v1/status", "Authorization: Bearer 0123456789abcdef0123\r\n", host)); CHECK(r.status == 200, "remote peer with the token %d", r.status);
    r = http(lan, port, get("/api/v1/status?token=0123456789abcdef0123", "", host)); CHECK(r.status == 200, "remote peer, token in the query %d", r.status);
    r = http(lan, port, post("/api/v1/gain", "{\"db\": 7}", port, "application/json", "")); CHECK(r.status == 401, "remote POST without token %d", r.status);
    r = http(lan, port, "POST /api/v1/gain HTTP/1.1\r\nHost: " + host + "\r\nAuthorization: Bearer 0123456789abcdef0123\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"db\": 7  }"); CHECK(r.status == 200 && h.got() == "gain 7", "remote POST with token %d", r.status);
    // the token can change while the server runs: the old one stops working
    c.token = "fedcba9876543210fedc";
    c.port = port;   // port 0 would mean "pick another one": that restarts the listener elsewhere
    srv.configure(c);
    r = http(lan, port, get("/api/v1/status", "Authorization: Bearer 0123456789abcdef0123\r\n", host)); CHECK(r.status == 401, "old token after a change %d", r.status);
    r = http(lan, port, get("/api/v1/status", "Authorization: Bearer fedcba9876543210fedc\r\n", host)); CHECK(r.status == 200, "new token %d", r.status);
    // a WebSocket from a remote peer needs it too
    sock_t s = dial(lan, port);
    const std::string hs = wsHandshake(port, host);
    sockSendAll(s, hs.data(), hs.size());
    r = parseResp(readAll(s, 3000)); sockClose(s);
    CHECK(r.status == 401, "remote WebSocket without token %d", r.status);
    printf("remote-peer token checks ran against %s\n", lan.c_str());
}

// ---------------------------------------------------------------- the real engine on the generated signal behind the API
struct EngineHost : RemoteApiHost {
    Engine e;
    std::mutex mu;
    RxTelemetry rx; uint64_t rxSeq = 0;
    SpectrumFrame sf; uint64_t sfSeq = 0;
    double hz = 522e6;
    TuneSettings tune;
    bool running = false;
    void poll() { RxTelemetry t; if (e.latestRx(t, rxSeq)) { rxSeq = t.seq; rx = t; } SpectrumFrame f; if (e.latestSpectrum(f, sfSeq)) { sfSeq = f.seq; sf = f; } }
    std::string statusJson() override { return std::string("{\"running\":") + (running ? "true" : "false") + "}"; }
    std::string devicesJson() override { return "[{\"index\":0,\"name\":\"Synthetic\",\"kind\":\"synthetic\"}]"; }
    std::string modesJson() override { return "[{\"family\":0,\"name\":\"DVB\"}]"; }
    std::string telemetryJson(uint64_t& seq) override { std::lock_guard<std::mutex> lk(mu); poll(); seq = rx.seq; return rxTelemetryJson(rx); }
    std::string spectrumJson(int maxBins, uint64_t& seq) override {
        std::lock_guard<std::mutex> lk(mu); poll(); seq = sf.seq;
        std::string o = "{\"seq\":" + std::to_string(sf.seq) + ",\"rmsDbfs\":" + jsonNum(sf.stats.rmsDbfs) + ",\"dbfs\":[";
        const size_t n = sf.dbfs.size(), step = std::max<size_t>(1, n / (size_t)maxBins);
        bool first = true;
        for (size_t i = 0; i < n; i += step) { if (!first) o += ','; first = false; o += jsonNum(sf.dbfs[i]); }
        return o + "]}";
    }
    bool start(int, std::string& err) override { FileOptions fo; if (!e.start(DeviceInfo(), tune, fo)) { err = "cannot start"; return false; } running = true; return true; }
    bool stop(std::string&) override { e.stop(); running = false; return true; }
    bool setMode(int, const std::string&, std::string&) override { return true; }
    bool setFrequencyHz(double h, std::string&) override { hz = h; tune.centerHz = h; return e.retune(tune); }
    bool setSampleRateHz(double, std::string&) override { return true; }
    bool setGainDb(double, std::string&) override { return true; }
};

static void testEngine() {
    EngineHost h;
    h.tune.bandwidthMhz = 8;
    h.tune.synth.snrDb = 30;
    h.tune.synth.tx.s2field1 = 1;
    h.tune.synth.tx.giIdx = 3;
    h.e.setStandard(1);
    RemoteApiServer srv(h);
    RemoteApiConfig c; c.on = true; c.port = 0;
    CHECK(srv.configure(c), "server for the engine test");
    const int port = srv.config().port;
    Resp r = http("127.0.0.1", port, post("/api/v1/start", "{}", port), 40000);   // engine start runs inside the request: slow on a loaded CI machine
    CHECK(r.status == 200 && h.running, "start the engine through the API: %d %s", r.status, r.body.c_str());
    std::string buf, pl;
    int op = 0;
    sock_t s = wsOpen(port, buf);
    bool locked = false;
    double snr = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (!locked && std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() < 60) {
        if (!sframe(s, buf, op, pl, 1000)) continue;
        if (pl.find("\"locked\":true") != std::string::npos) {
            locked = true;
            const size_t i = pl.find("\"snrDb\":");
            if (i != std::string::npos) snr = atof(pl.c_str() + i + 8);
        }
    }
    CHECK(locked, "the generated T2 signal never reported locked over the WebSocket");
    printf("engine behind the API: locked, reported SNR %.1f dB over the WebSocket: %.200s\n", snr, pl.c_str());
    CHECK(snr > 15 && snr < 45, "implausible SNR %.1f dB for a 30 dB generated signal", snr);
    CHECK(pl.find("\"standardName\":\"DVB-T") != std::string::npos && pl.find("\"details\"") != std::string::npos, "telemetry JSON has the mode and its details");
    r = http("127.0.0.1", port, withPort(get("/api/v1/telemetry"), port));
    CHECK(r.status == 200 && r.body.find("\"locked\":true") != std::string::npos, "telemetry over HTTP: %.120s", r.body.c_str());
    r = http("127.0.0.1", port, withPort(get("/api/v1/spectrum?bins=256"), port));
    CHECK(r.status == 200 && r.body.find("\"dbfs\":[") != std::string::npos, "spectrum over HTTP: %.80s", r.body.c_str());
    r = http("127.0.0.1", port, post("/api/v1/frequency", "{\"mhz\": 530}", port)); CHECK(r.status == 200 && std::fabs(h.hz - 530e6) < 1, "retune through the API (%.0f)", h.hz);
    sockClose(s);
    r = http("127.0.0.1", port, post("/api/v1/stop", "{}", port)); CHECK(r.status == 200 && !h.running, "stop through the API");
}

// the FM block of the telemetry: the RDS PI also as four hexadecimal digits (piCode), null while unknown
static void testFmPiCode() {
    RxTelemetry rx;
    rx.standard = 6;
    rx.fm.piCode = 0xE2a1;
    std::string j = rxTelemetryJson(rx);
    CHECK(j.find("\"piCode\":\"E2A1\"") != std::string::npos && j.find("\"pi\":58017") != std::string::npos, "PI as hex and as a number: %s", j.c_str());
    rx.fm.piCode = 0x0A01;
    CHECK(rxTelemetryJson(rx).find("\"piCode\":\"0A01\"") != std::string::npos, "PI keeps four digits");
    rx.fm.piCode = 0;
    CHECK(rxTelemetryJson(rx).find("\"piCode\":null") != std::string::npos, "PI unknown: null");
}

int main(int argc, char** argv) {
    const bool onlyEngine = argc > 1 && !strcmp(argv[1], "engine");
    testHelpers();
    testFmPiCode();
    if (!onlyEngine) {
        FakeHost h;
        RemoteApiServer srv(h);
        RemoteApiConfig c; c.on = true; c.port = 0;
        CHECK(srv.configure(c) && srv.status().listening, "server starts on a free port: %s", srv.status().lastError.c_str());
        const int port = srv.config().port;
        CHECK(port > 0, "port");
        testHttp(h, srv, port);
        testWs(h, port);
        testServerParts(h, port);
        testWsSlowClient(h, port);
        const RemoteApiStatus st = srv.status();
        printf("server counters: %llu requests, %llu refused\n", (unsigned long long)st.requests, (unsigned long long)st.rejected);
        CHECK(srv.configure(RemoteApiConfig()) && !srv.status().listening, "switching it off");
        CHECK(http("127.0.0.1", port, withPort(get("/api/v1/status"), port), 500).status == 0, "nothing answers once it is off");
        testAuth();
        testTls();
    }
    testEngine();
    printf(fails ? "remote_api tests FAILED (%d)\n" : "remote_api tests passed\n", fails);
    return fails ? 1 : 0;
}
