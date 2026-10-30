// Remote-control API server: see dect2/net/remote_api.h. One thread accepts; every connection has a thread of its own (at most 16), so a slow or
// silent client never holds up another one. Blocking sockets with select() for the waits, on top of the helpers of tcp_client.h.
#include "dect2/common/thread_name.h"
#include "dect2/net/remote_api.h"
#include "dect2/net/api_policy.h"
#include "dect2/common/str.h"
#include "dect2/net/net_proto.h"
#include "dect2/source/rotator.h"
#include "dect2/engine/mode_tuning.h"
#include "netcompat.h"
#include "tcp_client.h"
#include "tls_conn.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#ifndef _WIN32
#include <sys/select.h>
#include <sys/time.h>
#include "dect2/common/str.h"
#endif

namespace dect2 {

// ------------------------------------------------------------------ SHA-1, base64, WebSocket frames: net_proto.h (C++ or the Rust crate onair_net)
std::string sha1(const std::string& data) { return netp::sha1(data); }
std::string base64(const std::string& d) { return netp::base64(d); }
std::string wsAcceptKey(const std::string& key) { return netp::wsAcceptKey(key); }
std::string wsFrame(int opcode, const std::string& p) { return netp::wsFrame(opcode, p); }

bool parseFlatJson(const std::string& t, std::map<std::string, JsonVal>& out) { return netp::parseFlatJson(t, out); }

std::string remoteApiNewToken() {
    std::random_device rd;
    std::seed_seq sq{rd(), rd(), rd(), rd(), (unsigned)std::chrono::steady_clock::now().time_since_epoch().count()};
    std::mt19937_64 g(sq);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 24; i++) s.push_back(hex[g() & 15]);
    return s;
}

// ------------------------------------------------------------------ the server
namespace {
constexpr size_t kMaxConns = 16, kMaxWs = 8, kMaxWsFrame = 4096;
constexpr int kRequestMs = 5000;

std::string lower(std::string s) { return dect2::str::lower(std::move(s)); }
// The connection of this thread (every connection has a thread of its own): TLS when the server was given a certificate, else plain sockets
thread_local tls::Conn* gTls = nullptr;
bool ioSendAll(sock_t s, const void* d, size_t n) {
    if (!gTls) return sockSendAll(s, d, n);
    const char* p = (const char*)d;
    while (n > 0) { const long k = tls::write(gTls, p, n); if (k <= 0) return false; p += k; n -= (size_t)k; }
    return true;
}
long long ioRecv(sock_t s, void* d, size_t n) { return gTls ? (long long)tls::read(gTls, d, n) : sockRecv(s, d, n); }
bool waitReadable(sock_t s, int ms) {
    if (gTls && tls::pending(gTls)) return true;
    fd_set rd;
    FD_ZERO(&rd); FD_SET(s, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0;
}
void sockSendTimeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}
std::string hostOnly(const std::string& h) { return netp::hostOnly(h); }
bool loopbackName(const std::string& n) { return netp::loopbackName(n); }
std::string urlDecode(const std::string& s) { return netp::urlDecode(s); }
std::string queryParam(const std::string& q, const std::string& key) { return netp::queryParam(q, key); }
std::string statusText(int c) { return netp::statusText(c); }
struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> h;   // lower-case names
    bool loopback = false;
    std::string hdr(const char* k) const { auto it = h.find(k); return it == h.end() ? std::string() : it->second; }
};
} // namespace

struct RemoteApiServer::Impl {
    struct Conn { sock_t s = kBadSock; std::string peer; std::thread th; std::atomic<bool> done{false}; std::atomic<bool> ws{false}; bool loopback = false; };
    explicit Impl(RemoteApiHost& h) : host(h) {}
    RemoteApiHost& host;
    mutable std::mutex mu;
    RemoteApiConfig cfg, live;
    RemoteApiStatus st;
    std::vector<std::unique_ptr<Conn>> conns;
    std::mutex hostMu;   // one call into the host at a time
    std::thread th;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> requests{0}, rejected{0};
    std::mutex limMu;
    apipol::RateLimiter limiter;

    void stopAll() {
        quit = true;
        if (th.joinable()) th.join();   // the accept thread closes and joins its connections on the way out
        quit = false;
        std::lock_guard<std::mutex> lk(mu);
        st.listening = false; st.tls = false; st.httpClients = st.wsClients = 0;
    }
    void reap() {   // under no lock: joins the connections that finished
        for (size_t i = 0; i < conns.size();) {
            if (conns[i]->done) { if (conns[i]->th.joinable()) conns[i]->th.join(); sockClose(conns[i]->s); conns.erase(conns.begin() + (long)i); }
            else i++;
        }
        int http = 0, ws = 0;
        for (auto& c : conns) (c->ws ? ws : http)++;
        std::lock_guard<std::mutex> lk(mu);
        st.httpClients = http; st.wsClients = ws;
    }
    std::atomic<int> wsActive{0};
    tls::Ctx* tctx = nullptr;   // set while listening with TLS

    // ---- responses
    bool send(sock_t s, int code, const std::string& body, const char* type = "application/json", const std::string& extra = "") {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" + extra + "\r\n" + body;
        return ioSendAll(s, r.data(), r.size());
    }
    bool sendErr(sock_t s, int code, const std::string& msg, const std::string& extra = "") {
        rejected++;
        return send(s, code, "{\"error\":\"" + jsonEscape(msg) + "\"}", "application/json", extra);
    }

    // ---- reading a request; false: the connection is answered (error) or dead
    bool readRequest(sock_t s, Request& rq) {
        std::string buf;
        const auto t0 = std::chrono::steady_clock::now();
        auto late = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > kRequestMs; };
        size_t hdrEnd = 0;
        bool have = false;
        while (!have) {
            if (quit) return false;
            if (late()) { sendErr(s, 408, "request timeout"); return false; }
            if (!waitReadable(s, 100)) continue;
            char b[2048];
            const long long k = ioRecv(s, b, sizeof b);
            if (k <= 0) return false;
            buf.append(b, (size_t)k);
            const int sc = netp::httpScan(buf, hdrEnd);   // 0 not yet, 1 found, 2 over 8192 bytes
            if (sc == 2) { sendErr(s, 431, "headers too large"); return false; }
            have = sc == 1;
        }
        const std::string head = buf.substr(0, hdrEnd);
        std::string rest = buf.substr(hdrEnd + 4);
        netp::HttpHead hh;
        const netp::HttpError he = netp::httpHead(head, hh);   // request line, header fields, Content-Length
        if (he.status) { sendErr(s, he.status, he.msg); return false; }
        rq.method = std::move(hh.method); rq.path = std::move(hh.path); rq.query = std::move(hh.query); rq.h = std::move(hh.h);
        const size_t want = hh.contentLength;
        while (rest.size() < want) {
            if (quit) return false;
            if (late()) { sendErr(s, 408, "request timeout"); return false; }
            if (!waitReadable(s, 100)) continue;
            char b[2048];
            const long long k = ioRecv(s, b, sizeof b);
            if (k <= 0) return false;
            rest.append(b, (size_t)k);
        }
        rq.body = rest.substr(0, want);
        return true;
    }

    // Host / Origin / token. False: answered.
    bool authorise(sock_t s, const Request& rq, const RemoteApiConfig& c) {
        const std::string host = rq.hdr("host");
        if (host.empty()) { sendErr(s, 400, "Host header required"); return false; }
        const std::string org = rq.hdr("origin");
        if (!org.empty()) {   // a web page: only this server's own pages (there are none), never another site's script
            const size_t sch = org.find("://");
            if (sch == std::string::npos || lower(org.substr(sch + 3)) != lower(host)) { sendErr(s, 403, "cross-origin requests are refused"); return false; }
        }
        if (rq.loopback && !loopbackName(lower(hostOnly(host)))) { sendErr(s, 403, "Host must be a loopback name"); return false; }   // DNS rebinding
        const apipol::Auth au = apipol::authorise(rq.loopback, c.requireTokenLocal, c.token, apipol::extractToken(rq.hdr("authorization"), rq.query));
        if (au == apipol::Auth::Allow) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // guessing is slow
        sendErr(s, 401, "token required", "WWW-Authenticate: Bearer\r\n");
        return false;
    }

    static bool needJson(const Request& rq) { return lower(rq.hdr("content-type")).compare(0, 16, "application/json") == 0; }

    // ---- the API
    void route(sock_t s, const Request& rq) {
        const std::string& p = rq.path;
        const bool isGet = rq.method == "GET", isPost = rq.method == "POST";
        if (!isGet && !isPost) { sendErr(s, 405, "method not allowed", "Allow: GET, POST\r\n"); return; }
        const apipol::Route pr = apipol::parseRoute(rq.method, p);   // malformed paths, the refusal list, unknown API versions, wrong verbs
        if (pr.status == 405) { sendErr(s, 405, pr.msg, "Allow: " + pr.allow + "\r\n"); return; }
        if (pr.status != 200 && pr.status != 404) { sendErr(s, pr.status, pr.msg); return; }
        if (pr.status == 404 && pr.msg != "no such endpoint") { sendErr(s, 404, pr.msg); return; }
        std::string err;
        uint64_t seq = 0;
        auto okJson = [&] { send(s, 200, "{\"ok\":true}"); };
        auto fail = [&](const std::string& e) { sendErr(s, 400, e.empty() ? "failed" : e); };
        auto getOnly = [&](const std::function<std::string()>& f) {
            if (!isGet) { sendErr(s, 405, "use GET", "Allow: GET\r\n"); return; }
            std::string j;
            { std::lock_guard<std::mutex> lk(hostMu); j = f(); }
            send(s, 200, j);
        };
        if (p == "/" || p == "/index.html") {
            std::string page;
            if (isGet) { std::lock_guard<std::mutex> lk(hostMu); page = host.indexHtml(); }
            if (page.empty()) { sendErr(s, 404, "no web page in this program"); return; }
            send(s, 200, page, "text/html; charset=utf-8", "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self' ws: wss:; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'\r\nReferrer-Policy: no-referrer\r\n");
            return;
        }
        if (p == "/api/v1" || p == "/api/v1/") {
            getOnly([] {
                std::string o = "{\"name\":\"OnAir remote API\",\"version\":1,\"endpoints\":[";
                bool first = true;
                for (const std::string& e : apipol::endpointList()) { if (!first) o += ','; first = false; o += "\"" + e + "\""; }
                return o + "]}";
            });
            return;
        }
        if (p == "/api/v1/status") { getOnly([&] { return host.statusJson(); }); return; }
        if (p == "/api/v1/devices") { getOnly([&] { return host.devicesJson(); }); return; }
        if (p == "/api/v1/modes") { getOnly([&] { return host.modesJson(); }); return; }
        if (p == "/api/v1/telemetry") { getOnly([&] { return host.telemetryJson(seq); }); return; }
        if (p == "/api/v1/spectrum") {
            int bins = 1024;
            const std::string b = queryParam(rq.query, "bins");
            if (!b.empty()) bins = std::max(16, std::min(4096, atoi(b.c_str())));
            getOnly([&] { return host.spectrumJson(bins, seq); });
            return;
        }
        if (p == "/api/v1/rotator") {
            getOnly([] { return Rotator::get().apiStatusJson(); });
            return;
        }
        if (pr.ep == apipol::Ep::TxStatus) {
            std::string j;
            { std::lock_guard<std::mutex> lk(hostMu); j = host.txStatusJson(); }
            if (j.empty()) sendErr(s, 501, "no transmitter in this program"); else send(s, 200, j);
            return;
        }
        if (pr.ep == apipol::Ep::Sessions) {   // paged: ?mode=&kind=&offset=&limit=(<=200)&q=&active=1&since=
            std::string j;
            { std::lock_guard<std::mutex> lk(hostMu); j = host.sessionsJson(rq.query); }
            if (j.empty()) sendErr(s, 501, "not available in this program"); else send(s, j.compare(0, 8, "{\"error\"") == 0 ? 400 : 200, j);
            return;
        }
        if (pr.ep == apipol::Ep::Memories || pr.ep == apipol::Ep::Scheduler || pr.ep == apipol::Ep::Audio || pr.ep == apipol::Ep::Recording) {
            std::string j;
            { std::lock_guard<std::mutex> lk(hostMu); j = pr.ep == apipol::Ep::Memories ? host.memoriesJson() : pr.ep == apipol::Ep::Scheduler ? host.schedulerJson() : pr.ep == apipol::Ep::Audio ? host.audioJson() : host.recordingJson(); }
            if (j.empty()) sendErr(s, 501, "not available in this program"); else send(s, 200, j);
            return;
        }
        if (pr.ep == apipol::Ep::Events) {   // polling: ?since=N (omitted: from now) &max=N
            const std::string sn = queryParam(rq.query, "since"), mx = queryParam(rq.query, "max");
            uint64_t cur = UINT64_MAX;
            if (!sn.empty()) { if (sn.size() > 18 || sn.find_first_not_of("0123456789") != std::string::npos) { sendErr(s, 400, "since must be a number"); return; } cur = strtoull(sn.c_str(), nullptr, 10); }
            const size_t maxN = mx.empty() ? 100 : (size_t)std::max(1, std::min(500, atoi(mx.c_str())));
            std::vector<std::string> lines;
            bool gap = false, have;
            { std::lock_guard<std::mutex> lk(hostMu); have = host.eventsSince(cur, maxN, lines, gap); }
            if (!have) { sendErr(s, 501, "no message stream in this program"); return; }
            std::string o = std::string("{\"gap\":") + (gap ? "true" : "false") + ",\"next\":" + std::to_string(cur) + ",\"events\":[";
            for (size_t i = 0; i < lines.size(); i++) { if (i) o += ','; o += lines[i]; }
            send(s, 200, o + "]}");
            return;
        }
        if (p == "/api/v1/ws") { sendErr(s, 426, "WebSocket upgrade required", "Upgrade: websocket\r\n"); return; }   // a proper upgrade is handled before route()
        if (p.compare(0, 10, "/api/v1/x/") == 0) {   // the host program's own endpoints
            std::map<std::string, JsonVal> jb;
            if (isPost) {
                if (!needJson(rq)) { sendErr(s, 415, "Content-Type: application/json required"); return; }
                if (!rq.body.empty() && !parseFlatJson(rq.body, jb)) { sendErr(s, 400, "body must be a flat JSON object"); return; }
            }
            std::string out;
            int code;
            { std::lock_guard<std::mutex> lk(hostMu); code = host.handleExtra(rq.method, p, jb, out); }
            if (code == 0) { sendErr(s, 404, "no such endpoint"); return; }
            if (code >= 400) sendErr(s, code, out); else send(s, code, out);
            return;
        }
        const bool known = p == "/api/v1/start" || p == "/api/v1/stop" || p == "/api/v1/mode" || p == "/api/v1/frequency" || p == "/api/v1/samplerate" || p == "/api/v1/gain"
                         || p == "/api/v1/autogain" || p == "/api/v1/bandwidth" || p == "/api/v1/squelch" || p == "/api/v1/volume" || p == "/api/v1/mute"
                         || p == "/api/v1/recording/start" || p == "/api/v1/recording/stop" || p == "/api/v1/tx/start" || p == "/api/v1/tx/stop"
                         || p == "/api/v1/rotator/goto" || p == "/api/v1/rotator/stop" || p == "/api/v1/rotator/park";
        if (!known) { sendErr(s, 404, "no such endpoint"); return; }
        if (!isPost) { sendErr(s, 405, "use POST", "Allow: POST\r\n"); return; }
        if (!needJson(rq)) { sendErr(s, 415, "Content-Type: application/json required"); return; }
        std::map<std::string, JsonVal> j;
        if (!rq.body.empty() && !parseFlatJson(rq.body, j)) { sendErr(s, 400, "body must be a flat JSON object"); return; }
        auto num = [&](const char* k, double& v) { auto it = j.find(k); if (it == j.end() || it->second.kind != JsonVal::Num) return false; v = it->second.num; return true; };
        std::lock_guard<std::mutex> lk(hostMu);
        double v = 0;
        if (p == "/api/v1/start") {
            int dev = -1;
            if (j.count("device")) { if (!num("device", v) || v < 0 || v > 1000 || v != std::floor(v)) { fail("device must be a whole number >= 0"); return; } dev = (int)v; }
            if (host.start(dev, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/stop") {
            if (host.stop(err)) okJson(); else fail(err);
        } else if (p == "/api/v1/mode") {
            if (!num("family", v) || v < 0 || v > 1000 || v != std::floor(v)) { fail("family must be a whole number (see /api/v1/modes)"); return; }
            std::string sub;
            if (j.count("sub")) { if (j["sub"].kind != JsonVal::Str || j["sub"].str.size() > 40) { fail("sub must be a short string"); return; } sub = j["sub"].str; }
            if (host.setMode((int)v, sub, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/frequency") {
            double hz = 0;
            if (num("hz", v)) hz = v; else if (num("mhz", v)) hz = v * 1e6; else { fail("give hz or mhz"); return; }
            if (!(hz > 0) || hz > kMaxTuneMhz * 1e6) { fail("frequency out of range"); return; }
            if (host.setFrequencyHz(hz, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/samplerate") {
            double hz = 0;
            if (num("hz", v)) hz = v; else if (num("msps", v)) hz = v * 1e6; else { fail("give hz or msps (0 = automatic)"); return; }
            if (hz < 0 || hz > 1e9) { fail("sample rate out of range"); return; }
            if (host.setSampleRateHz(hz, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/rotator/stop") {
            Rotator::get().stop();
            okJson();
        } else if (p == "/api/v1/rotator/park") {
            Rotator::get().park();
            okJson();
        } else if (p == "/api/v1/rotator/goto") {
            double az = 0, el = 0;
            if (!num("az", az) && !num("azimuth", az)) { fail("give az or azimuth (corrected degrees)"); return; }
            if (!num("el", el) && !num("elevation", el)) el = 0;
            Rotator::get().gotoAzEl(az, el);
            okJson();
        } else if (p == "/api/v1/volume") {
            if (!num("level", v) || !(v >= 0 && v <= 1)) { fail("level must be a number from 0 to 1"); return; }
            if (host.setVolume(v, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/mute" || p == "/api/v1/autogain") {
            auto it = j.find("on");
            if (it == j.end() || it->second.kind != JsonVal::Bool) { fail("give \"on\": true or false"); return; }
            const bool on = it->second.b;
            if (p == "/api/v1/mute" ? host.setMute(on, err) : host.setAutoGain(on, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/bandwidth") {
            double hz = 0;
            if (num("hz", v)) hz = v; else if (num("khz", v)) hz = v * 1e3; else { fail("give hz or khz"); return; }
            if (!(hz > 0) || hz > 1e9) { fail("bandwidth out of range"); return; }
            if (host.setBandwidthHz(hz, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/squelch") {
            auto on = j.find("on"), au = j.find("auto");
            double db = -100;
            if (on == j.end() || on->second.kind != JsonVal::Bool) { fail("give \"on\": true or false"); return; }
            if (au != j.end() && au->second.kind != JsonVal::Bool) { fail("auto must be true or false"); return; }
            if (j.count("db") && (!num("db", db) || db < -140 || db > 20)) { fail("db must be from -140 to 20"); return; }
            if (host.setSquelch(on->second.b, au != j.end() && au->second.b, db, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/recording/start") {
            std::string kind = "iq";
            if (j.count("kind")) { if (j["kind"].kind != JsonVal::Str || (j["kind"].str != "iq" && j["kind"].str != "audio")) { fail("kind must be \"iq\" or \"audio\""); return; } kind = j["kind"].str; }
            if (host.recordStart(kind, err)) okJson(); else fail(err);
        } else if (p == "/api/v1/recording/stop") {
            if (host.recordStop(err)) okJson(); else fail(err);
        } else if (p == "/api/v1/tx/start" || p == "/api/v1/tx/stop") {
            std::string out;
            const bool ok = p == "/api/v1/tx/start" ? host.txStart(j, out, err) : host.txStop(out, err);
            if (ok) send(s, 200, out.empty() ? std::string("{\"ok\":true}") : out); else fail(err);
        } else {   // gain
            if (!num("db", v) || v < -100 || v > 200) { fail("db must be a number"); return; }
            if (host.setGainDb(v, err)) okJson(); else fail(err);
        }
    }

    // ---- WebSocket: telemetry (and the spectrum when asked for) pushed as text frames; the client may send {"telemetry":bool,"spectrum":bool,"bins":N}
    void serveWs(sock_t s, const Request& rq, Conn& cn) {
        const std::string key = rq.hdr("sec-websocket-key");
        if (lower(rq.hdr("upgrade")) != "websocket" || lower(rq.hdr("connection")).find("upgrade") == std::string::npos || rq.hdr("sec-websocket-version") != "13" || key.size() != 24) {
            sendErr(s, 400, "not a WebSocket handshake (needs Upgrade, Connection, Sec-WebSocket-Version: 13 and a key)");
            return;
        }
        if (wsActive.fetch_add(1) >= (int)kMaxWs) { wsActive--; rejected++; send(s, 503, "{\"error\":\"too many WebSocket clients\"}"); return; }
        struct Dec { std::atomic<int>& n; ~Dec() { n--; } } dec{wsActive};
        cn.ws = true;
        const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
        if (!ioSendAll(s, r.data(), r.size())) return;
        bool wantTel = true, wantSpec = false, binSpec = false;
        int bins = 512, fps = 5, slow = 1, audioId = -1;   // slow: the spectrum interval is stretched by this while the client cannot keep up
        struct AudioGuard { RemoteApiHost& h; std::mutex& m; int& id; ~AudioGuard() { if (id >= 0) { std::lock_guard<std::mutex> lk(m); h.audioClose(id); } } } audioGuard{host, hostMu, audioId};
        uint64_t telSeq = ~0ull, specSeq = ~0ull, evCursor = UINT64_MAX;
        bool wantEv = false;
        auto lastTel = std::chrono::steady_clock::now() - std::chrono::seconds(1), lastSpec = lastTel;
        std::string in;
        auto closeWith = [&](int code) {
            std::string pl;
            pl.push_back((char)(code >> 8)); pl.push_back((char)(code & 0xFF));
            const std::string f = wsFrame(8, pl);
            ioSendAll(s, f.data(), f.size());
        };
        auto sendText = [&](const std::string& t) { const std::string f = wsFrame(1, t); return ioSendAll(s, f.data(), f.size()); };
        while (!quit) {
            if (waitReadable(s, audioId >= 0 ? 15 : 50)) {
                char b[1024];
                const long long k = ioRecv(s, b, sizeof b);
                if (k <= 0) return;
                in.append(b, (size_t)k);
                if (in.size() > 2 * (kMaxWsFrame + 16)) { closeWith(1009); return; }
            }
            while (in.size() >= 2) {   // whole frames
                const netp::WsClient fr = netp::wsParseClient(in);   // 0 need more, 1 close with a code, 2 a frame (text, ping, pong or close)
                if (fr.kind == 0) break;
                if (fr.kind == 1) { closeWith(fr.code); return; }
                in.erase(0, fr.used);
                const int op = fr.op;
                const std::string& pl = fr.payload;
                if (op == 8) { closeWith(1000); return; }
                if (op == 9) { const std::string f = wsFrame(10, pl.substr(0, 125)); if (!ioSendAll(s, f.data(), f.size())) return; continue; }
                if (op == 10) continue;
                std::map<std::string, JsonVal> j;
                if (!parseFlatJson(pl, j)) { if (!sendText("{\"type\":\"error\",\"error\":\"send a flat JSON object like {\\\"spectrum\\\":true}\"}")) return; continue; }
                if (j.count("telemetry") && j["telemetry"].kind == JsonVal::Bool) wantTel = j["telemetry"].b;
                if (j.count("spectrum") && j["spectrum"].kind == JsonVal::Bool) wantSpec = j["spectrum"].b;
                if (j.count("bins") && j["bins"].kind == JsonVal::Num) bins = std::max(16, std::min(2048, (int)j["bins"].num));
                if (j.count("fps") && j["fps"].kind == JsonVal::Num) fps = std::max(1, std::min(30, (int)j["fps"].num));
                if (j.count("fmt") && j["fmt"].kind == JsonVal::Str) binSpec = j["fmt"].str == "u8";   // bytes instead of JSON: about a tenth of the bandwidth
                if (j.count("events") && j["events"].kind == JsonVal::Bool) {
                    wantEv = j["events"].b;
                    evCursor = UINT64_MAX;
                    std::vector<std::string> none;
                    bool gp = false, have;
                    { std::lock_guard<std::mutex> lk(hostMu); have = host.eventsSince(evCursor, 0, none, gp); }
                    if (wantEv && !have) { wantEv = false; if (!sendText("{\"type\":\"error\",\"error\":\"no message stream in this program\"}")) return; }
                }
                if (j.count("audio") && j["audio"].kind == JsonVal::Bool) {
                    std::lock_guard<std::mutex> lk(hostMu);
                    if (audioId >= 0) { host.audioClose(audioId); audioId = -1; }
                    if (j["audio"].b) {
                        const int ar = j.count("arate") && j["arate"].kind == JsonVal::Num ? std::max(8000, std::min(48000, (int)j["arate"].num)) : 16000;
                        audioId = host.audioOpen(ar);
                        if (audioId < 0 && !sendText("{\"type\":\"error\",\"error\":\"this program has no audio to send\"}")) return;
                    }
                }
                slow = 1;
                telSeq = specSeq = ~0ull;   // the new choice is answered with the current values
                lastTel = lastSpec = std::chrono::steady_clock::now() - std::chrono::seconds(1);
                if (!sendText(std::string("{\"type\":\"subscribed\",\"telemetry\":") + (wantTel ? "true" : "false") + ",\"spectrum\":" + (wantSpec ? "true" : "false") + ",\"bins\":" + std::to_string(bins) + ",\"fps\":" + std::to_string(fps) + ",\"audio\":" + (audioId >= 0 ? "true" : "false") + "}")) return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (wantTel && now - lastTel >= std::chrono::milliseconds(100)) {
                uint64_t sq = telSeq;
                std::string j;
                { std::lock_guard<std::mutex> lk(hostMu); j = host.telemetryJson(sq); }
                if (sq != telSeq) {
                    telSeq = sq; lastTel = now;
                    if (!sendText("{\"type\":\"telemetry\",\"data\":" + j + "}")) return;
                }
            }
            if (wantSpec && now - lastSpec >= std::chrono::milliseconds(std::min(2000, 1000 / fps * slow))) {
                uint64_t sq = specSeq;
                std::string j;
                bool bin = false;
                { std::lock_guard<std::mutex> lk(hostMu); bin = binSpec && host.spectrumBinary(bins, sq, j); if (!bin) j = host.spectrumJson(bins, sq); }
                if (sq != specSeq) {
                    specSeq = sq; lastSpec = now;
                    const std::string f = bin ? wsFrame(2, j) : wsFrame(1, "{\"type\":\"spectrum\",\"data\":" + j + "}");
                    const auto t1 = std::chrono::steady_clock::now();
                    if (!ioSendAll(s, f.data(), f.size())) return;
                    // bandwidth-aware: a send that took a big part of the interval means the link is slow, so frames come less often until it keeps up again
                    const long us = (long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t1).count();
                    const long interval = 1000000L / fps * slow;
                    if (us > interval / 2 && slow < 16) slow *= 2;
                    else if (us < interval / 16 && slow > 1) slow--;
                }
            }
            if (wantEv) {
                std::vector<std::string> lines;
                bool gp = false;
                { std::lock_guard<std::mutex> lk(hostMu); host.eventsSince(evCursor, 20, lines, gp); }
                if (gp && !sendText("{\"type\":\"gap\"}")) return;
                for (const std::string& l : lines) if (!sendText("{\"type\":\"event\",\"data\":" + l + "}")) return;
            }
            if (audioId >= 0) {
                std::string pcm;
                bool alive;
                { std::lock_guard<std::mutex> lk(hostMu); alive = host.audioRead(audioId, pcm); }
                if (!alive) { { std::lock_guard<std::mutex> lk(hostMu); host.audioClose(audioId); } audioId = -1; }
                else if (!pcm.empty()) {
                    const std::string f = wsFrame(2, std::string(1, '\x02') + pcm);   // [0x02] then 16 bit little endian mono samples
                    if (!ioSendAll(s, f.data(), f.size())) return;
                }
            }
        }
    }

    void serve(Conn* cn) {
        sock_t s = cn->s;
        sockSendTimeout(s, 3000);
        bool tlsOk = true;
        if (tctx) {
            sockRecvTimeout(s, kRequestMs);
            std::string terr;
            gTls = tls::accept(tctx, s, terr);
            tlsOk = gTls != nullptr;
            if (!tlsOk) rejected++;
        }
        Request rq;
        rq.loopback = cn->loopback;
        RemoteApiConfig c;
        { std::lock_guard<std::mutex> lk(mu); c = live; }
        bool rateOk = true;
        if (tlsOk && readRequest(s, rq)) {
            if (c.ratePerSec > 0) {
                std::lock_guard<std::mutex> lk(limMu);
                rateOk = limiter.allow(cn->peer, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
            }
            if (!rateOk) sendErr(s, 429, "too many requests", "Retry-After: 1\r\n");
        } else rateOk = false;
        if (rateOk && authorise(s, rq, c)) {
            requests++;
            if (rq.method == "GET" && rq.path == "/api/v1/ws" && !rq.hdr("upgrade").empty()) serveWs(s, rq, *cn);
            else route(s, rq);
        }
        if (gTls) { tls::close(gTls); gTls = nullptr; }
        sockShutdown(s);
        cn->done = true;
    }

    void run(RemoteApiConfig c) {
        netInit();
        { std::lock_guard<std::mutex> lk(limMu); limiter = apipol::RateLimiter(c.ratePerSec, std::max(1.0, c.rateBurst)); }
        if (!c.tlsCert.empty() || !c.tlsKey.empty()) {
            std::string terr;
            tctx = c.tlsCert.empty() || c.tlsKey.empty() ? nullptr : tls::makeCtx(c.tlsCert, c.tlsKey, terr);
            if (!tctx) { std::lock_guard<std::mutex> lk(mu); st.lastError = terr.empty() ? "TLS needs both a certificate file and a key file" : terr; return; }
        }
        struct CtxFree { tls::Ctx*& c; ~CtxFree() { tls::freeCtx(c); c = nullptr; } } ctxFree{tctx};
        sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) { std::lock_guard<std::mutex> lk(mu); st.lastError = "cannot open a socket"; return; }
#ifndef _WIN32
        { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(c.lan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)c.port);
        if (c.port < 0 || c.port > 65535 || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 16) != 0) {
            std::lock_guard<std::mutex> lk(mu);
            st.lastError = "cannot listen on TCP port " + std::to_string(c.port) + " (in use by another program?)";
            sockClose(ls);
            return;
        }
        {
            sockaddr_in b{}; socklen_t bl = sizeof b;
            std::lock_guard<std::mutex> lk(mu);
            if (getsockname(ls, (sockaddr*)&b, &bl) == 0) live.port = ntohs(b.sin_port);
        }
        sockSetNonBlocking(ls, true);
        { std::lock_guard<std::mutex> lk(mu); st.listening = true; st.tls = tctx != nullptr; st.lastError.clear(); }
        while (!quit) {
            if (waitReadable(ls, conns.empty() ? 250 : 50)) {   // idle: 4 wakeups a second (a new connection wakes select at once)
                sockaddr_in pa{}; socklen_t pl = sizeof pa;
                const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
                if (k != kBadSock) {
                    sockSetNonBlocking(k, false);
#ifdef SO_NOSIGPIPE
                    { int one = 1; setsockopt(k, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
                    reap();
                    if (conns.size() >= kMaxConns) {
                        rejected++;
                        const std::string r = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        ioSendAll(k, r.data(), r.size());
                        // Closing with the client's request still unread resets the connection on Windows, and the reset throws the answer away:
                        // end our side, read what the client sent (briefly), then close
#ifdef _WIN32
                        ::shutdown(k, SD_SEND);
#else
                        ::shutdown(k, SHUT_WR);
#endif
                        sockRecvTimeout(k, 200);
                        { char drain[2048]; for (int n = 0; n < 8 && sockRecv(k, drain, sizeof drain) > 0; n++) {} }
                        sockClose(k);
                    } else {
                        auto cn = std::make_unique<Conn>();
                        cn->s = k;
                        cn->loopback = (ntohl(pa.sin_addr.s_addr) >> 24) == 127;
                        { char ab[INET_ADDRSTRLEN] = {0}; inet_ntop(AF_INET, &pa.sin_addr, ab, sizeof ab); cn->peer = ab; }
                        Conn* raw = cn.get();
                        cn->th = dect2::namedThread("onair-api-c", [this, raw] { serve(raw); });
                        conns.push_back(std::move(cn));
                    }
                }
            } else reap();
        }
        sockClose(ls);
        for (auto& c2 : conns) sockShutdown(c2->s);
        for (auto& c2 : conns) { if (c2->th.joinable()) c2->th.join(); sockClose(c2->s); }
        conns.clear();
    }
};

RemoteApiServer::RemoteApiServer(RemoteApiHost& host) : p_(new Impl(host)) {}
RemoteApiServer::~RemoteApiServer() { p_->stopAll(); }

bool RemoteApiServer::configure(const RemoteApiConfig& cIn) {
    RemoteApiConfig c = cIn;
    Impl& p = *p_;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        const int cur = p.cfg.on && p.cfg.port == 0 ? p.live.port : p.cfg.port;
        const bool same = c.on == p.cfg.on && c.port == cur && c.lan == p.cfg.lan && c.tlsCert == p.cfg.tlsCert && c.tlsKey == p.cfg.tlsKey && p.st.listening == c.on;
        if (same) {   // only the token changed (or nothing): it takes effect at once
            if (c.lan && c.token.size() < 16) { p.st.lastError = "LAN access needs a token of at least 16 characters"; }
            else { p.cfg.token = c.token; p.live.token = c.token; p.st.lastError.clear(); }
            return !c.on || p.st.listening;
        }
        p.cfg = c;
    }
    p.stopAll();
    if (!c.on) { std::lock_guard<std::mutex> lk(p.mu); p.st.lastError.clear(); return true; }
    if (c.lan && c.token.size() < 16) {
        std::lock_guard<std::mutex> lk(p.mu);
        p.st.lastError = "LAN access needs a token of at least 16 characters";
        return false;
    }
    { std::lock_guard<std::mutex> lk(p.mu); p.live = c; p.st.lastError.clear(); p.st.listening = false; }
    p.th = dect2::namedThread("onair-api", [&p, c] { p.run(c); });
    for (int i = 0; i < 400; i++) {   // until it listens (or failed), so that port 0 can be read back
        { std::lock_guard<std::mutex> lk(p.mu); if (p.st.listening || !p.st.lastError.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lk(p.mu);
    return p.st.listening;
}

RemoteApiConfig RemoteApiServer::config() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    RemoteApiConfig c = p_->cfg;
    if (c.on && c.port == 0) c.port = p_->live.port;
    return c;
}

RemoteApiStatus RemoteApiServer::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    RemoteApiStatus s = p_->st;
    s.requests = p_->requests; s.rejected = p_->rejected;
    return s;
}

} // namespace dect2
