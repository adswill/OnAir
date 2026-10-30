// Internet TV: a small HTTP/1.1 client (GET only). Plain http goes over our own sockets everywhere; https goes through OpenSSL when the
// build has it (Linux), else through the system's own TLS (macOS: NSURLSession, Windows: WinHTTP/Schannel), see iptv_http_cocoa.mm / iptv_http_win.cpp.
#include "dect2/tv/iptv.h"
#include "../net/tcp_client.h"
#include "../net/tls_conn.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace dect2::iptv {

#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<HttpBody> httpOpenPlatform(const std::string&, const HttpOptions&, HttpResult&, bool& handled) { handled = false; return nullptr; }
#endif

bool httpsAvailable() {
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return tls::available();
#endif
}
const char* httpsMissingText() { return "This build of OnAir has no TLS support, so https:// streams cannot be opened. http:// and udp:// streams work. See docs/IPTV.md."; }

static std::string lc(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

namespace {

struct Conn {
    sock_t s = kBadSock;
    tls::Ctx* ctx = nullptr;
    tls::Conn* tc = nullptr;
    ~Conn() { if (tc) tls::close(tc); if (ctx) tls::freeCtx(ctx); if (s != kBadSock) sockClose(s); }
    long rd(void* b, size_t n) { return tc ? tls::read(tc, b, n) : (long)sockRecv(s, b, n); }
    bool wr(const void* b, size_t n) {
        if (!tc) return sockSendAll(s, b, n);
        const char* p = (const char*)b;
        while (n) { long k = tls::write(tc, p, n); if (k <= 0) return false; p += k; n -= (size_t)k; }
        return true;
    }
};

bool timeoutErrno() {
#ifdef _WIN32
    const int e = WSAGetLastError(); return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// reads with short socket timeouts so that cancel() is noticed; returns -1 on timeout (readTimeoutMs without a byte) or cancel
long readWait(Conn& c, void* b, size_t n, int totalMs, std::atomic<bool>* cancel) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (cancel && cancel->load()) return -2;
        if (c.tc && tls::pending(c.tc)) return c.rd(b, n);
        const long k = c.rd(b, n);
        if (k > 0) return k;
        if (k == 0 && !c.tc) return 0;
        if (c.tc && k <= 0) {   // OpenSSL: timeouts and closure are indistinguishable here; treat a quiet socket as timeout, a closed one as end
            if (!timeoutErrno()) return 0;
        } else if (k < 0 && !timeoutErrno()) return -1;
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (el >= totalMs) return -1;
    }
}

class SocketBody : public HttpBody {
public:
    std::unique_ptr<Conn> c;
    std::string pre;            // body bytes already read with the headers
    int64_t remaining = -1;     // content-length, -1 unknown
    bool chunked = false, done = false;
    int64_t chunkLeft = 0;
    int readMs = 15000;
    std::atomic<bool>* cancel = nullptr;
    std::string chunkBuf;       // raw bytes of a chunked body not yet decoded
    bool fillRaw() {
        char tmp[8192];
        const long k = readWait(*c, tmp, sizeof tmp, readMs, cancel);
        if (k <= 0) return false;
        chunkBuf.append(tmp, (size_t)k);
        return true;
    }
    long rawRead(void* b, size_t n) {
        if (!pre.empty()) { const size_t k = std::min(n, pre.size()); memcpy(b, pre.data(), k); pre.erase(0, k); return (long)k; }
        return readWait(*c, b, n, readMs, cancel);
    }
    long read(void* b, size_t n) override {
        if (done) return 0;
        if (!chunked) {
            if (remaining == 0) { done = true; return 0; }
            size_t want = n;
            if (remaining > 0 && (int64_t)want > remaining) want = (size_t)remaining;
            const long k = rawRead(b, want);
            if (k > 0 && remaining > 0) remaining -= k;
            if (k == 0) done = true;
            return k;
        }
        // chunked: decode from chunkBuf (pre first)
        if (!pre.empty()) { chunkBuf += pre; pre.clear(); }
        for (;;) {
            if (chunkLeft > 0) {
                if (chunkBuf.empty() && !fillRaw()) return -1;
                const size_t k = std::min({(size_t)chunkLeft, chunkBuf.size(), n});
                memcpy(b, chunkBuf.data(), k);
                chunkBuf.erase(0, k);
                chunkLeft -= (int64_t)k;
                return (long)k;
            }
            // need a chunk-size line (after the CRLF that ends the previous chunk)
            size_t eol;
            while (true) {
                while (!chunkBuf.empty() && (chunkBuf[0] == '\r' || chunkBuf[0] == '\n')) chunkBuf.erase(0, 1);   // the CRLF that ends the previous chunk
                eol = chunkBuf.find("\r\n");
                if (eol != std::string::npos) break;
                if (!fillRaw()) return -1;
            }
            std::string line = chunkBuf.substr(0, eol);
            chunkBuf.erase(0, eol + 2);
            const long sz = strtol(line.c_str(), nullptr, 16);
            if (sz <= 0) { done = true; return 0; }
            chunkLeft = sz;
        }
    }
};

std::string statusText(int s) {
    switch (s) {
    case 400: return "400 Bad Request: the server did not accept the request";
    case 401: return "401 Unauthorized: this stream needs a login or token";
    case 403: return "403 Forbidden: the server refuses this request (try another User-Agent or Referer under Advanced, the link may have expired, or the stream is not available in your country)";
    case 404: return "404 Not Found: the stream or segment no longer exists at this address";
    case 410: return "410 Gone: this link has expired";
    case 429: return "429 Too Many Requests: the server is limiting requests, OnAir will retry more slowly";
    case 451: return "451 Unavailable For Legal Reasons: blocked in your region";
    case 500: return "500 Server error at the stream provider";
    case 502: return "502 Bad Gateway at the stream provider";
    case 503: return "503 Service Unavailable: the stream provider is busy or down";
    case 504: return "504 Gateway Timeout at the stream provider";
    default: return "HTTP " + std::to_string(s);
    }
}

} // namespace

std::unique_ptr<HttpBody> httpOpen(const std::string& urlIn, const HttpOptions& o, HttpResult& r) {
    std::string url = urlIn;
    for (int hop = 0; hop <= o.maxRedirects; hop++) {
        r = HttpResult();
        r.finalUrl = url;
        const Url u = parseUrl(url);
        if (!u.ok || (u.scheme != "http" && u.scheme != "https")) { r.error = "not a valid http(s) address: " + redactUrl(url); return nullptr; }
        if (u.scheme == "https") {
#if !defined(__APPLE__) && !defined(_WIN32)
            if (!tls::available()) { r.error = httpsMissingText(); return nullptr; }
#else
            bool handled = false;
            auto b = httpOpenPlatform(url, o, r, handled);
            if (handled) return b;
#endif
        }
        auto c = std::make_unique<Conn>();
        std::string err;
        c->s = tcpConnect(u.host, u.port, o.connectTimeoutMs, err);
        if (c->s == kBadSock) { r.error = err.empty() ? "could not connect to " + u.host : err; return nullptr; }
        sockRecvTimeout(c->s, 500);
        if (u.scheme == "https") {
            c->ctx = tls::makeClientCtx(err, o.insecureTls);
            if (!c->ctx) { r.error = err; return nullptr; }
            c->tc = tls::connect(c->ctx, c->s, u.host, err);
            if (!c->tc) { r.error = err + (o.insecureTls ? "" : " (Advanced > Ignore certificate errors can be ticked for a stream you trust)"); return nullptr; }
        }
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + (u.ipv6 ? "[" + u.host + "]" : u.host);
        if ((u.scheme == "http" && u.port != 80) || (u.scheme == "https" && u.port != 443)) req += ":" + std::to_string(u.port);
        req += "\r\nUser-Agent: " + (o.userAgent.empty() ? std::string("OnAir") : o.userAgent) + "\r\nAccept: */*\r\nConnection: close\r\n";
        if (!o.referer.empty()) req += "Referer: " + o.referer + "\r\n";
        for (auto& h : o.headers) if (h.first.find_first_of("\r\n") == std::string::npos && h.second.find_first_of("\r\n") == std::string::npos) req += h.first + ": " + h.second + "\r\n";
        req += "\r\n";
        if (!c->wr(req.data(), req.size())) { r.error = "the connection was closed while sending the request"; return nullptr; }
        // headers
        std::string head;
        size_t hend = std::string::npos;
        char tmp[4096];
        while (hend == std::string::npos) {
            const long k = readWait(*c, tmp, sizeof tmp, o.readTimeoutMs, o.cancel);
            if (k == -2) { r.error = "stopped"; return nullptr; }
            if (k <= 0) { r.error = k == 0 ? "the server closed the connection without an answer" : "the server did not answer in time"; return nullptr; }
            head.append(tmp, (size_t)k);
            hend = head.find("\r\n\r\n");
            if (head.size() > 256 * 1024) { r.error = "the server's answer is not HTTP"; return nullptr; }
        }
        std::string body = head.substr(hend + 4);
        head.resize(hend);
        // status line (also ICY 200 OK from radio servers)
        if (head.compare(0, 5, "HTTP/") != 0 && head.compare(0, 3, "ICY") != 0) { r.error = "the server's answer is not HTTP"; return nullptr; }
        const size_t sp = head.find(' ');
        r.status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
        size_t p = head.find("\r\n");
        while (p != std::string::npos && p + 2 < head.size()) {
            size_t e = head.find("\r\n", p + 2);
            std::string line = head.substr(p + 2, e == std::string::npos ? std::string::npos : e - p - 2);
            const size_t col = line.find(':');
            if (col != std::string::npos) r.headers[lc(line.substr(0, col))] = trim(line.substr(col + 1));
            p = e;
        }
        r.contentType = r.headers.count("content-type") ? r.headers["content-type"] : "";
        if (r.headers.count("content-length")) r.contentLength = atoll(r.headers["content-length"].c_str());
        if (r.status >= 300 && r.status < 400 && r.headers.count("location")) {
            url = resolveUrl(url, r.headers["location"]);
            continue;
        }
        if (r.status < 200 || r.status >= 300) { r.error = statusText(r.status); return nullptr; }
        auto b = std::make_unique<SocketBody>();
        b->c = std::move(c);
        b->pre = std::move(body);
        b->readMs = o.readTimeoutMs;
        b->cancel = o.cancel;
        b->chunked = r.headers.count("transfer-encoding") && lc(r.headers["transfer-encoding"]).find("chunked") != std::string::npos;
        b->remaining = b->chunked ? -1 : r.contentLength;
        return b;
    }
    r.error = "too many redirects";
    return nullptr;
}

bool httpGetAll(const std::string& url, const HttpOptions& o, std::string& body, HttpResult& r, size_t maxBytes) {
    body.clear();
    auto b = httpOpen(url, o, r);
    if (!b) return false;
    char buf[16384];
    for (;;) {
        const long k = b->read(buf, sizeof buf);
        if (k == 0) return true;
        if (k < 0) { if (!body.empty() && r.contentLength < 0) return true; r.error = "the download stopped (timeout or connection lost)"; return false; }
        body.append(buf, (size_t)k);
        if (body.size() > maxBytes) { r.error = "the file is larger than " + std::to_string(maxBytes >> 20) + " MB"; return false; }
    }
}

} // namespace dect2::iptv
