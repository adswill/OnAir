#include "web_server.h"
#include "../../core/src/net/netcompat.h"
#include "../../core/src/net/tls_conn.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace fs = std::filesystem;
using dect2::sock_t;
using dect2::kBadSock;

namespace onair_web {

// ---------------------------------------------------------------- helpers
std::string urlDecodeStrict(const std::string& s, bool& ok) {
    ok = true;
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') { o += s[i]; continue; }
        if (i + 2 >= s.size()) { ok = false; return std::string(); }
        auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int a = hv(s[i + 1]), b = hv(s[i + 2]);
        if (a < 0 || b < 0) { ok = false; return std::string(); }
        const char c = (char)(a * 16 + b);
        if (c == 0) { ok = false; return std::string(); }
        o += c;
        i += 2;
    }
    return o;
}

std::string staticPath(const std::string& root, const std::string& urlPath) {
    if (root.empty() || urlPath.empty() || urlPath[0] != '/' || urlPath.size() > 256) return std::string();
    bool ok;
    std::string p = urlDecodeStrict(urlPath, ok);
    if (!ok) return std::string();
    if (p.back() == '/') p += "index.html";
    // only plain names: letters, digits, '.', '-', '_', '/'; no empty, '.' or '..' segments and no dot files
    for (char c : p) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/')) return std::string();
    std::string rel;
    size_t i = 1;
    while (i <= p.size()) {
        const size_t j = std::min(p.find('/', i), p.size());
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg[0] == '.') return std::string();
        rel += (rel.empty() ? "" : "/") + seg;
        i = j + 1;
    }
    std::error_code ec;
    const fs::path base = fs::canonical(fs::path(root), ec);
    if (ec) return std::string();
    const fs::path full = fs::weakly_canonical(base / rel, ec);
    if (ec) return std::string();
    // symlinks resolved: the file must still be below the root
    const std::string b = base.generic_string(), f = full.generic_string();
    if (f.size() <= b.size() || f.compare(0, b.size(), b) != 0 || f[b.size()] != '/') return std::string();
    if (!fs::is_regular_file(full, ec)) return std::string();
    return full.string();
}

const char* contentType(const std::string& p) {
    const auto dot = p.find_last_of('.');
    const std::string e = dot == std::string::npos ? "" : p.substr(dot + 1);
    if (e == "html" || e == "htm") return "text/html; charset=utf-8";
    if (e == "js" || e == "mjs") return "text/javascript; charset=utf-8";
    if (e == "css") return "text/css; charset=utf-8";
    if (e == "json" || e == "map") return "application/json";
    if (e == "svg") return "image/svg+xml";
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "webp") return "image/webp";
    if (e == "ico") return "image/x-icon";
    if (e == "wasm") return "application/wasm";
    if (e == "woff2") return "font/woff2";
    if (e == "txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool privateAddress(const std::string& ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

static std::string lowerStr(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}
static const char* reason(int c) {
    switch (c) {
    case 101: return "Switching Protocols"; case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request";
    case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 408: return "Request Timeout"; case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type"; case 426: return "Upgrade Required";
    case 429: return "Too Many Requests"; case 431: return "Request Header Fields Too Large"; case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static const char* kPlaceholder =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>OnAir Web</title><style>body{font:16px system-ui,sans-serif;max-width:40em;margin:3em auto;padding:0 1em;color:#222;background:#fafafa}"
    "@media (prefers-color-scheme:dark){body{color:#ddd;background:#161616}}code{font-size:90%}</style></head><body>"
    "<h1>OnAir Web</h1><p>This OnAir server accepts web listeners, but its browser interface is not installed (set <code>web_root</code> "
    "to the folder with the OnAir Web files).</p><p>Server information: <a href=\"/web/v1/info\">/web/v1/info</a>. "
    "Programs connect to <code>/web/v1/ws</code> (protocol onair-web.v1).</p></body></html>";

// ---------------------------------------------------------------- the server
struct WebServer::Impl {
    WebHub& hub;
    WebServerConfig cfg;
    mutable std::mutex mu;
    WebServerStatus st;
    std::vector<InviteToken> invites;   // under mu (cfg.auth.invites is the start value)
    std::function<void(const WebPolicy&, const std::vector<InviteToken>&)> onChange;
    FailLimiter fails;   // under mu
    std::map<std::string, int> perIp;   // open connections per address, under mu
    int open = 0;
    dect2::tls::Ctx* tctx = nullptr;
    sock_t ls = kBadSock;
    std::thread acc;
    std::atomic<bool> quit{false};
    std::atomic<int> threads{0};
    explicit Impl(WebHub& h) : hub(h) {}

    // ---- one connection's I/O (plain or TLS)
    struct Io {
        sock_t s = kBadSock;
        dect2::tls::Conn* t = nullptr;
        bool sendAll(const std::string& d) const {
            size_t off = 0;
            while (off < d.size()) {
                const long long k = t ? (long long)dect2::tls::write(t, d.data() + off, d.size() - off) : dect2::sockSend(s, d.data() + off, d.size() - off);
                if (k <= 0) return false;
                off += (size_t)k;
            }
            return true;
        }
        long long recv(void* b, size_t n) const { return t ? (long long)dect2::tls::read(t, b, n) : dect2::sockRecv(s, b, n); }
        bool readable(int ms) const {
            if (t && dect2::tls::pending(t)) return true;
            fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
            timeval tv{ms / 1000, (ms % 1000) * 1000};
            return select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0;
        }
    };

    struct Req {
        std::string method, path, query, version, body;
        std::map<std::string, std::string> h;
        std::string peer;   // the listener's address (X-Forwarded-For when trusted)
        bool loopbackPeer = false;
        bool viaProxy = false;   // the connection came from a reverse proxy on this computer that web_trust_proxy believes (also when it sent X-Forwarded-For)
    };

    static std::string secHeaders(bool tls) {
        std::string h = "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\nCross-Origin-Opener-Policy: same-origin\r\n"
                        "Cross-Origin-Resource-Policy: same-origin\r\nPermissions-Policy: camera=(), microphone=(), geolocation=()\r\n";
        if (tls) h += "Strict-Transport-Security: max-age=31536000\r\n";
        return h;
    }
    void reply(const Io& io, int code, const std::string& body, const char* type, const std::string& extra = "", bool head = false) const {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nCache-Control: no-store\r\nConnection: close\r\n" + secHeaders(io.t != nullptr) + extra + "\r\n";
        if (!head) r += body;
        io.sendAll(r);
    }
    void replyErr(const Io& io, int code, const std::string& msg) const { reply(io, code, "{\"error\":\"" + jsonEsc(msg) + "\"}", "application/json"); }

    // Reads the request head (and a small body). False: answered with an error already, or the peer went away.
    bool readRequest(const Io& io, Req& r) {
        std::string buf;
        const auto t0 = std::chrono::steady_clock::now();
        size_t headEnd = std::string::npos;
        while (true) {
            headEnd = buf.find("\r\n\r\n");
            if (headEnd != std::string::npos) break;
            if (buf.size() > kMaxHttpHead) { replyErr(io, 431, "request head too large"); return false; }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 5000) { replyErr(io, 408, "request timeout"); return false; }
            if (!io.readable(100)) continue;
            char b[2048];
            const long long k = io.recv(b, sizeof b);
            if (k <= 0) return false;
            buf.append(b, (size_t)k);
        }
        if (headEnd > kMaxHttpHead) { replyErr(io, 431, "request head too large"); return false; }
        std::istringstream in(buf.substr(0, headEnd));
        std::string line;
        if (!std::getline(in, line)) { replyErr(io, 400, "bad request"); return false; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        {
            const size_t a = line.find(' '), b = line.rfind(' ');
            if (a == std::string::npos || b == a) { replyErr(io, 400, "bad request line"); return false; }
            r.method = line.substr(0, a);
            std::string target = line.substr(a + 1, b - a - 1);
            r.version = line.substr(b + 1);
            if (r.version != "HTTP/1.1" && r.version != "HTTP/1.0") { replyErr(io, 400, "bad version"); return false; }
            if (target.empty() || target[0] != '/') { replyErr(io, 400, "bad target"); return false; }
            const size_t q = target.find('?');
            r.path = target.substr(0, q);
            if (q != std::string::npos) r.query = target.substr(q + 1);
        }
        int n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (++n > 48) { replyErr(io, 431, "too many header fields"); return false; }
            const size_t c = line.find(':');
            if (c == std::string::npos || c == 0) { replyErr(io, 400, "bad header"); return false; }
            const std::string k = lowerStr(line.substr(0, c));
            for (char ch : k) if (!(std::isalnum((unsigned char)ch) || ch == '-' || ch == '_')) { replyErr(io, 400, "bad header name"); return false; }
            r.h[k] = trimStr(line.substr(c + 1));
        }
        if (r.h.count("transfer-encoding")) { replyErr(io, 400, "chunked bodies are not accepted"); return false; }
        size_t len = 0;
        if (r.h.count("content-length")) {
            const std::string& v = r.h["content-length"];
            if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos) { replyErr(io, 400, "bad length"); return false; }
            len = (size_t)std::stoul(v);
            if (len > kMaxHttpBody) { replyErr(io, 413, "body too large"); return false; }
        }
        r.body = buf.substr(headEnd + 4);
        while (r.body.size() < len) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 5000) { replyErr(io, 408, "request timeout"); return false; }
            if (!io.readable(100)) continue;
            char b[2048];
            const long long k = io.recv(b, sizeof b);
            if (k <= 0) return false;
            r.body.append(b, (size_t)k);
        }
        if (r.body.size() > len) r.body.resize(len);
        return true;
    }

    // the origin a browser on this server's own page sends
    bool originOk(const Req& r, bool tls) const {
        auto it = r.h.find("origin");
        if (it == r.h.end()) return true;   // not a browser page (a program); it still has to authenticate
        const std::string o = lowerStr(it->second);
        auto ht = r.h.find("host");
        if (ht != r.h.end()) {
            const std::string host = lowerStr(ht->second);
            if (o == std::string(tls ? "https://" : "http://") + host) return true;
            if (cfg.trustProxy && r.viaProxy && o == "https://" + host) return true;   // TLS ends at the proxy
        }
        for (const auto& a : cfg.origins) if (o == lowerStr(a)) return true;
        return false;
    }

    // ---- the WebSocket of one listener
    void webSocket(const Io& io, const Req& r) {
        auto up = r.h.find("upgrade");
        auto key = r.h.find("sec-websocket-key");
        auto ver = r.h.find("sec-websocket-version");
        if (r.method != "GET" || up == r.h.end() || lowerStr(up->second) != "websocket" || key == r.h.end() || key->second.size() != 24) { replyErr(io, 426, "a WebSocket upgrade is needed"); return; }
        if (ver == r.h.end() || ver->second != "13") { reply(io, 426, "{\"error\":\"WebSocket version 13 only\"}", "application/json", "Sec-WebSocket-Version: 13\r\n"); return; }
        if (!originOk(r, io.t != nullptr)) { replyErr(io, 403, "this page's origin is not allowed"); return; }
        std::string proto;
        auto sp = r.h.find("sec-websocket-protocol");
        if (sp != r.h.end()) {
            std::stringstream ss(sp->second);
            std::string item;
            while (std::getline(ss, item, ',')) if (trimStr(item) == kSubprotocol) proto = kSubprotocol;
            if (proto.empty()) { replyErr(io, 400, "unsupported subprotocol"); return; }
        }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAccept(key->second) + "\r\n";
        if (!proto.empty()) resp += "Sec-WebSocket-Protocol: " + proto + "\r\n";
        resp += "\r\n";
        if (!io.sendAll(resp)) return;

        auto sendText = [&](const std::string& t) { return io.sendAll(wsServerFrame(1, t)); };
        auto closeWith = [&](int code, const std::string& why) { io.sendAll(wsCloseFrame(code, why)); };
        auto errMsg = [](const std::string& code, const std::string& why) { return "{\"type\":\"error\",\"code\":\"" + jsonEsc(code) + "\",\"message\":\"" + jsonEsc(why) + "\"}"; };

        std::string in;
        int id = 0;
        double tokens = cfg.msgBurst, lastRefill = wallSeconds(), lastRx = wallSeconds(), lastPing = wallSeconds();
        std::vector<double> violations;
        const double helloBy = wallSeconds() + cfg.helloTimeoutMs / 1000.0;
        bool closing = false;
        while (!quit && !closing) {
            const double now = wallSeconds();
            if (!id && now > helloBy) { sendText(errMsg(err::Auth, "no hello")); closeWith(4001, "no hello"); break; }
            if (now - lastRx > cfg.keepaliveSec) { closeWith(1001, "keepalive timeout"); break; }
            if (now - lastPing > cfg.pingSec) { lastPing = now; if (!io.sendAll(wsServerFrame(9, "k"))) break; }
            if (id) {
                std::vector<OutMsg> out;
                std::string kick;
                const bool alive = hub.drain(id, out, kick);
                bool ok = true;
                for (auto& m : out) if (!(ok = io.sendAll(wsServerFrame(m.binary ? 2 : 1, m.data)))) break;
                if (!ok) break;
                if (!alive) {
                    sendText("{\"type\":\"kicked\",\"reason\":\"" + jsonEsc(kick) + "\"}");
                    closeWith(kick == "idle" || kick == "time_limit" ? 4041 : kick == "bye" ? 1000 : 4040, kick);
                    break;
                }
            }
            if (!io.readable(id ? 15 : 50)) continue;
            char b[4096];
            const long long k = io.recv(b, sizeof b);
            if (k <= 0) break;
            in.append(b, (size_t)k);
            if (in.size() > 2 * kMaxClientMsg + 64) { closeWith(1009, "too much data"); break; }
            while (!closing) {
                WsIn f = wsParse(in, kMaxClientMsg);
                if (f.kind == 0) break;
                if (f.kind == 1) { closeWith(f.closeCode, "protocol error"); closing = true; break; }
                in.erase(0, f.used);
                lastRx = wallSeconds();
                if (f.op == 8) { io.sendAll(wsCloseFrame(1000, "")); closing = true; break; }
                if (f.op == 9) { io.sendAll(wsServerFrame(10, f.payload)); continue; }
                if (f.op == 10) continue;
                if (f.op == 2) { closeWith(1003, "binary messages are not accepted"); closing = true; break; }
                // rate limit: a token bucket per connection
                const double t = wallSeconds();
                tokens = std::min<double>(cfg.msgBurst, tokens + (t - lastRefill) * cfg.msgPerSec);
                lastRefill = t;
                if (tokens < 1) {
                    violations.push_back(t);
                    violations.erase(std::remove_if(violations.begin(), violations.end(), [&](double v) { return t - v > 10; }), violations.end());
                    sendText(errMsg(err::RateLimited, "too many messages"));
                    if (violations.size() >= 3) { closeWith(4008, "rate limit"); closing = true; }
                    continue;
                }
                tokens -= 1;
                ClientMsg m;
                std::string code, why;
                if (!parseClientMsg(f.payload, m, code, why, hub.allowTx())) {
                    sendText(errMsg(code, why));
                    if (!id && code != err::Forbidden) { closeWith(4000, "bad message"); closing = true; }
                    continue;
                }
                if (!id) {
                    if (m.type != ClientMsg::Hello) { sendText(errMsg(err::Auth, "send hello first")); closeWith(4001, "hello first"); closing = true; break; }
                    AuthResult a;
                    bool blocked;
                    {
                        std::lock_guard<std::mutex> g(mu);
                        blocked = fails.blocked(r.peer, t);
                        if (!blocked) { AuthConfig ac = cfg.auth; ac.invites = invites; a = authenticate(ac, m.password, m.invite); }
                    }
                    if (blocked) { sendText(errMsg(err::Auth, "too many failed attempts, wait a minute")); closeWith(4029, "blocked"); closing = true; break; }
                    if (!a.ok) {
                        { std::lock_guard<std::mutex> g(mu); fails.fail(r.peer, t); }
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        sendText(errMsg(err::Auth, a.why));
                        closeWith(4001, "auth");
                        closing = true;
                        break;
                    }
                    AdmitRequest ar;
                    ar.ip = r.peer; ar.cls = a.cls; ar.name = m.name; ar.label = a.label; ar.maxMinutes = a.maxMinutes;
                    std::string c2, w2;
                    id = hub.admit(ar, c2, w2);
                    if (id <= 0) { id = 0; sendText(errMsg(c2, w2)); closeWith(4029, "busy"); closing = true; break; }
                    sendText(hub.welcomeJson(id));
                    continue;
                }
                hub.handle(id, m);
            }
        }
        if (id) hub.leave(id);
    }

    // ---- admin API
    void admin(const Io& io, const Req& r) {
        if (!r.loopbackPeer && !cfg.adminLan) { replyErr(io, 403, "the admin API answers on this computer only (admin_lan=1 to change)"); return; }
        if (cfg.auth.adminToken.size() < 24) { replyErr(io, 404, "no admin token configured"); return; }
        auto o = r.h.find("origin");
        if (o != r.h.end() && !originOk(r, io.t != nullptr)) { replyErr(io, 403, "origin not allowed"); return; }
        auto au = r.h.find("authorization");
        const std::string tok = au != r.h.end() && au->second.compare(0, 7, "Bearer ") == 0 ? au->second.substr(7) : std::string();
        bool blocked;
        { std::lock_guard<std::mutex> g(mu); blocked = fails.blocked(r.peer, wallSeconds()); }
        if (blocked) { replyErr(io, 429, "too many failed attempts"); return; }
        if (!ctEq(tok, cfg.auth.adminToken)) {
            { std::lock_guard<std::mutex> g(mu); fails.fail(r.peer, wallSeconds()); }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            replyErr(io, 401, "admin token required");
            return;
        }
        const std::string sub = r.path.substr(std::string("/web/v1/admin/").size());
        Json body;
        if (r.method == "POST") {
            auto ct = r.h.find("content-type");
            if (ct == r.h.end() || lowerStr(ct->second).compare(0, 16, "application/json") != 0) { replyErr(io, 415, "Content-Type: application/json"); return; }
            std::string e;
            if (!jsonParse(r.body, body, e) || body.kind != Json::Obj) { replyErr(io, 400, "bad JSON: " + e); return; }
        } else if (r.method != "GET") { replyErr(io, 405, "GET or POST"); return; }
        auto str = [&](const char* k) { const Json* j = body.get(k); return j && j->kind == Json::Str ? j->str : std::string(); };
        auto num = [&](const char* k, double d) { const Json* j = body.get(k); return j && j->kind == Json::Num ? j->num : d; };
        auto flag = [&](const char* k, bool& out) { const Json* j = body.get(k); if (!j || j->kind != Json::Bool) return false; out = j->b; return true; };
        if (r.method == "GET" && sub == "users") { reply(io, 200, hub.usersJson(), "application/json"); return; }
        if (r.method == "GET" && sub == "stats") { reply(io, 200, hub.statsJson(), "application/json"); return; }
        if (r.method == "GET" && sub == "policy") { std::lock_guard<std::mutex> g(hub.policyMutex()); reply(io, 200, hub.policy().json(), "application/json"); return; }
        if (r.method == "GET" && sub == "ranges") { std::lock_guard<std::mutex> g(hub.policyMutex()); reply(io, 200, hub.ranges().json(), "application/json"); return; }
        if (r.method == "POST" && sub == "ranges") {
            // {"allow":["144-146MHz,nfm"],"block":["..."],"presets":["ham_hf@open"],"action":"refuse"|"clamp"} (a missing key stays as it is),
            // or {"clear":true}. Nothing changes when any line is bad.
            bool clr = false;
            flag("clear", clr);
            auto list = [&](const char* k, std::vector<std::string>& out) {
                const Json* j = body.get(k);
                if (!j) return true;
                if (j->kind != Json::Arr) return false;
                for (const auto& x : j->arr) { if (x.kind != Json::Str) return false; out.push_back(x.str); }
                return true;
            };
            std::vector<std::string> a, b, p;
            std::string e, action = str("action");
            bool ok = true;
            {
                std::lock_guard<std::mutex> g(hub.policyMutex());
                if (clr) hub.ranges().clear();
                else {
                    if (!body.get("allow")) a = hub.ranges().allowSpecs(); else ok = ok && list("allow", a);
                    if (!body.get("block")) b = hub.ranges().blockSpecs(); else ok = ok && list("block", b);
                    if (!body.get("presets")) p = hub.ranges().presetSpecs(); else ok = ok && list("presets", p);
                    if (!ok) e = "allow, block and presets are lists of strings";
                    else ok = hub.ranges().replace(a, b, p, action, e);
                }
            }
            if (!ok) { replyErr(io, 400, e); return; }
            hub.rangesChanged();   // the listeners are told, tunings are re-checked (policy mutex not held)
            changed();
            std::lock_guard<std::mutex> g(hub.policyMutex());
            reply(io, 200, hub.ranges().json(), "application/json");
            return;
        }
        if (r.method == "GET" && sub == "legal") {
            reply(io, 200, std::string("{\"version\":\"") + kLegalNoticeVersion + "\",\"text\":\"" + jsonEsc(legalNoticeText()) + "\"}", "application/json");
            return;
        }
        if (r.method == "GET" && sub == "invites") {
            std::lock_guard<std::mutex> g(mu);
            std::string o2 = "{\"invites\":[";
            for (size_t i = 0; i < invites.size(); i++)   // the tokens themselves are not listed again: only a prefix to tell them apart
                o2 += (i ? "," : "") + std::string("{\"label\":\"") + jsonEsc(invites[i].label) + "\",\"prefix\":\"" + invites[i].token.substr(0, 6) + "\",\"max_minutes\":" + std::to_string(invites[i].maxMinutes) + "}";
            reply(io, 200, o2 + "]}", "application/json");
            return;
        }
        if (r.method == "POST" && sub == "kick") {
            const double id = num("id", -1);
            if (id < 1 || id > 2e9) { replyErr(io, 400, "id"); return; }
            std::string why = str("reason");
            if (why.size() > 64) why.resize(64);
            if (!hub.kick((int)id, why.empty() ? "kicked" : "kicked: " + why)) { replyErr(io, 404, "no such session"); return; }
            reply(io, 200, "{\"ok\":true}", "application/json");
            return;
        }
        if (r.method == "POST" && sub == "ban") {
            const double id = num("id", -1);
            if (id < 1 || id > 2e9) { replyErr(io, 400, "id"); return; }
            std::string why = str("reason");
            if (why.size() > 64) why.resize(64);
            if (!hub.ban((int)id, (int)num("minutes", 1440), why)) { replyErr(io, 404, "no such session"); return; }
            reply(io, 200, "{\"ok\":true}", "application/json");
            return;
        }
        if (r.method == "GET" && sub == "bans") { reply(io, 200, hub.bansJson(), "application/json"); return; }
        if (r.method == "POST" && sub == "unban") {
            const std::string tag = str("address");
            if (tag.empty() || !hub.unban(tag)) { replyErr(io, 404, "no such ban"); return; }
            reply(io, 200, "{\"ok\":true}", "application/json");
            return;
        }
        if (r.method == "POST" && sub == "policy") {
            std::string e;
            bool on = false;
            if (!flag("on", on)) { replyErr(io, 400, "on: true or false"); return; }
            bool ok;
            {
                std::lock_guard<std::mutex> g(hub.policyMutex());
                if (!str("decoder").empty()) ok = hub.policy().setDecoder(str("decoder"), on, str("accept_legal_notice"), e);
                else if (!str("demod").empty()) { Demod d; ok = demodFromName(str("demod"), d) && hub.policy().setDemod(d, on); if (!ok) e = "unknown demod"; }
                else { ok = false; e = "decoder or demod"; }
            }
            if (!ok) { replyErr(io, 400, e); return; }
            changed();
            std::lock_guard<std::mutex> g(hub.policyMutex());
            reply(io, 200, hub.policy().json(), "application/json");
            return;
        }
        if (r.method == "POST" && sub == "invite") {
            InviteToken t;
            t.token = newToken(16);
            t.label = str("label").substr(0, 48);
            t.maxMinutes = (int)std::clamp(num("max_minutes", 0), 0.0, 100000.0);
            { std::lock_guard<std::mutex> g(mu); if (invites.size() >= 256) { replyErr(io, 400, "too many invites"); return; } invites.push_back(t); }
            changed();
            reply(io, 200, "{\"token\":\"" + t.token + "\",\"label\":\"" + jsonEsc(t.label) + "\"}", "application/json");
            return;
        }
        if (r.method == "POST" && sub == "invite/revoke") {
            const std::string pre = str("prefix"), tokn = str("token");
            size_t n = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                const size_t before = invites.size();
                invites.erase(std::remove_if(invites.begin(), invites.end(), [&](const InviteToken& i) {
                    return (!tokn.empty() && ctEq(tokn, i.token)) || (pre.size() >= 6 && i.token.compare(0, pre.size(), pre) == 0);
                }), invites.end());
                n = before - invites.size();
            }
            if (!n) { replyErr(io, 404, "no such invite"); return; }
            changed();
            reply(io, 200, "{\"revoked\":" + std::to_string(n) + "}", "application/json");
            return;
        }
        replyErr(io, 404, "unknown admin call");
    }

    void changed() {
        std::function<void(const WebPolicy&, const std::vector<InviteToken>&)> f;
        std::vector<InviteToken> inv;
        { std::lock_guard<std::mutex> g(mu); f = onChange; inv = invites; }
        if (!f) return;
        std::lock_guard<std::mutex> g(hub.policyMutex());
        f(hub.policy(), inv);
    }

    void serveStatic(const Io& io, const Req& r) {
        if (r.method != "GET" && r.method != "HEAD") { replyErr(io, 405, "GET only"); return; }
        const bool head = r.method == "HEAD";
        const std::string csp = "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; "
                                "style-src 'self' 'unsafe-inline'; script-src 'self'; worker-src 'self' blob:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
        const std::string p = staticPath(cfg.webRoot, r.path);
        if (p.empty()) {
            if (r.path == "/" || r.path == "/index.html") { reply(io, 200, kPlaceholder, "text/html; charset=utf-8", csp, head); return; }
            replyErr(io, 404, "not found");
            return;
        }
        std::error_code ec;
        const auto sz = fs::file_size(p, ec);
        if (ec || sz > 16 * 1024 * 1024) { replyErr(io, 404, "not found"); return; }
        std::ifstream f(p, std::ios::binary);
        std::string data((size_t)sz, '\0');
        if (!f.read(&data[0], (std::streamsize)sz)) { replyErr(io, 404, "not found"); return; }
        reply(io, 200, data, contentType(p), csp, head);
    }

    void connection(sock_t s, std::string peer) {
        Io io;
        io.s = s;
        // a request must arrive within 5 s; sends that cannot go out within 5 s end the connection
#ifdef _WIN32
        DWORD tv = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
        timeval tv{5, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one); }
        if (tctx) {
            std::string e;
            io.t = dect2::tls::accept(tctx, s, e);
            if (!io.t) { finish(s, peer); return; }
        }
        Req r;
        r.loopbackPeer = peer.compare(0, 4, "127.") == 0;
        r.peer = peer;
        if (readRequest(io, r)) {
            if (cfg.trustProxy && r.loopbackPeer) {
                r.viaProxy = true;
                auto xf = r.h.find("x-forwarded-for");
                if (xf != r.h.end()) {   // the last address is the one the proxy saw
                    std::string v = xf->second;
                    const size_t c = v.rfind(',');
                    if (c != std::string::npos) v = v.substr(c + 1);
                    v = trimStr(v);
                    if (!v.empty() && v.size() < 64) { r.peer = v; r.loopbackPeer = false; }
                }
            }
            const bool behindTls = io.t || (cfg.trustProxy && peer.compare(0, 4, "127.") == 0);
            if (!behindTls && !cfg.plainInternet && !privateAddress(r.peer)) replyErr(io, 403, "this server needs HTTPS for listeners from the internet");
            else if (r.path == "/web/v1/ws") webSocket(io, r);
            else if (r.path == "/web/v1/info") {
                if (r.method != "GET") replyErr(io, 405, "GET only");
                else {
                    std::string info = hub.infoJson();
                    info.pop_back();
                    info += std::string(",\"auth\":\"") + authModeName(cfg.auth.mode) + "\",\"tls\":" + (io.t ? "true" : "false") + "}";
                    reply(io, 200, info, "application/json");
                }
            } else if (r.path.compare(0, 14, "/web/v1/admin/") == 0) admin(io, r);
            else if (r.path.compare(0, 5, "/web/") == 0) replyErr(io, 404, "not found");
            else serveStatic(io, r);
        }
        if (io.t) dect2::tls::close(io.t);
        finish(s, peer);
    }
    void finish(sock_t s, const std::string& peer) {
        dect2::sockShutdown(s);
        dect2::sockClose(s);
        std::lock_guard<std::mutex> g(mu);
        if (--perIp[peer] <= 0) perIp.erase(peer);
        open--;
        st.connections = open;
        threads--;
    }

    void acceptLoop() {
        while (!quit) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 200000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in pa{};
            socklen_t pl = sizeof pa;
            const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
            if (k == kBadSock) continue;
            char ip[64] = {0};
            inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
            const std::string peer = ip;
            {
                std::lock_guard<std::mutex> g(mu);
                if (open >= cfg.maxConnections || perIp[peer] >= cfg.maxConnPerIp) {
                    st.refused++;
                    if (perIp[peer] == 0) perIp.erase(peer);
                    dect2::sockClose(k);
                    continue;
                }
                perIp[peer]++;
                open++;
                st.connections = open;
                threads++;
            }
            std::thread([this, k, peer] { connection(k, peer); }).detach();
        }
    }
};

WebServer::WebServer(WebHub& hub) : p_(new Impl(hub)) {}
WebServer::~WebServer() { stop(); }

bool WebServer::start(const WebServerConfig& c) {
    stop();
    dect2::netInit();
    Impl& p = *p_;
    std::lock_guard<std::mutex> g(p.mu);
    p.cfg = c;
    p.invites = c.auth.invites;
    p.st = WebServerStatus();
    if (c.tlsCert.empty() != c.tlsKey.empty()) { p.st.lastError = "TLS needs both a certificate and a key"; return false; }
    if (c.auth.mode == AuthMode::Password && c.auth.password.size() < 8) { p.st.lastError = "web_password must have 8 characters or more"; return false; }
    if (!c.tlsCert.empty()) {
        std::string e;
        p.tctx = dect2::tls::makeCtx(c.tlsCert, c.tlsKey, e);
        if (!p.tctx) { p.st.lastError = e; return false; }
    }
    p.ls = socket(AF_INET, SOCK_STREAM, 0);
    if (p.ls == kBadSock) { p.st.lastError = "socket"; return false; }
    { int one = 1; setsockopt(p.ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)c.port);
    if (inet_pton(AF_INET, c.bind.c_str(), &a.sin_addr) != 1) { p.st.lastError = "bad bind address " + c.bind; dect2::sockClose(p.ls); p.ls = kBadSock; return false; }
    if (c.port < 0 || c.port > 65535 || bind(p.ls, (sockaddr*)&a, sizeof a) != 0 || listen(p.ls, 32) != 0) {
        p.st.lastError = "cannot listen on " + c.bind + ":" + std::to_string(c.port);
        dect2::sockClose(p.ls); p.ls = kBadSock;
        return false;
    }
    socklen_t al = sizeof a;
    getsockname(p.ls, (sockaddr*)&a, &al);
    p.st.port = ntohs(a.sin_port);
    p.st.listening = true;
    p.st.tls = p.tctx != nullptr;
    p.quit = false;
    p.acc = std::thread([&p] { p.acceptLoop(); });
    return true;
}

void WebServer::stop() {
    Impl& p = *p_;
    if (!p.acc.joinable()) return;
    p.quit = true;
    p.acc.join();
    for (int i = 0; i < 100 && p.threads > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // connections see quit within ~50 ms
    std::lock_guard<std::mutex> g(p.mu);
    if (p.ls != kBadSock) { dect2::sockClose(p.ls); p.ls = kBadSock; }
    if (p.tctx && p.threads == 0) { dect2::tls::freeCtx(p.tctx); p.tctx = nullptr; }
    p.st.listening = false;
}

WebServerStatus WebServer::status() const { std::lock_guard<std::mutex> g(p_->mu); return p_->st; }
void WebServer::setOnChange(std::function<void(const WebPolicy&, const std::vector<InviteToken>&)> f) { std::lock_guard<std::mutex> g(p_->mu); p_->onChange = std::move(f); }
std::vector<InviteToken> WebServer::invites() const { std::lock_guard<std::mutex> g(p_->mu); return p_->invites; }

} // namespace onair_web
