#include "web_relay.h"
#include "../../core/src/net/netcompat.h"
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/tls_conn.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/net/remote_api.h"
#include "server_config.h"
#include "web_directory_proto.h"
#include "web_json.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace onair_web {

namespace mc = dect2::meshcrypto;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- pure parts

bool relayParseUrl(const std::string& in, RelayUrl& o, std::string& err) {
    std::string u = in;
    while (!u.empty() && u.back() == ' ') u.pop_back();
    while (!u.empty() && u.front() == ' ') u.erase(u.begin());
    bool tls;
    size_t at;
    if (u.compare(0, 6, "wss://") == 0) { tls = true; at = 6; }
    else if (u.compare(0, 8, "https://") == 0) { tls = true; at = 8; }
    else if (u.compare(0, 5, "ws://") == 0) { tls = false; at = 5; }
    else if (u.compare(0, 7, "http://") == 0) { tls = false; at = 7; }
    else { err = "the relay address must start with wss://"; return false; }
    for (unsigned char c : u) if (c <= 32 || c == 127) { err = "the relay address has a space or a control character"; return false; }
    size_t e = u.find('/', at);
    std::string hp = u.substr(at, e == std::string::npos ? std::string::npos : e - at);
    std::string path = e == std::string::npos ? "/relay/v1/host" : u.substr(e);
    if (path == "/") path = "/relay/v1/host";
    if (path.find('?') != std::string::npos || path.find('#') != std::string::npos || path.size() > 200) { err = "the relay address must not have a query"; return false; }
    if (hp.find('@') != std::string::npos) { err = "the relay address must not have a user name"; return false; }
    std::string host = hp;
    int port = tls ? 443 : 80;
    if (!hp.empty() && hp[0] == '[') {
        const size_t r = hp.find(']');
        if (r == std::string::npos) { err = "bad relay address"; return false; }
        host = hp.substr(1, r - 1);
        if (r + 1 < hp.size()) { if (hp[r + 1] != ':') { err = "bad relay address"; return false; } port = atoi(hp.c_str() + r + 2); }
    } else {
        const size_t c = hp.rfind(':');
        if (c != std::string::npos) { host = hp.substr(0, c); port = atoi(hp.c_str() + c + 1); }
    }
    if (host.empty() || port < 1 || port > 65535) { err = "bad relay address"; return false; }
    if (!tls && !(host == "127.0.0.1" || host == "localhost" || host == "::1")) { err = "a plain ws:// relay is only allowed on this computer; use wss://"; return false; }
    o.tls = tls; o.host = host; o.port = port; o.path = path;
    return true;
}

std::string relayCanonical(const std::string& id, const std::string& pub, long long ts, const std::string& nonce) {
    return "OnAir-Relay-v1\nid=" + id + "\npub=" + pub + "\nts=" + std::to_string(ts) + "\nnonce=" + nonce + "\n";
}

std::string relayHelloJson(const RelayConfig& c, const std::string& id, const std::string& pub, long long ts, const std::string& nonce, const std::string& sigHex) {
    return "{\"type\":\"hello\",\"v\":1,\"id\":\"" + id + "\",\"pub\":\"" + pub + "\",\"ts\":" + std::to_string(ts) + ",\"nonce\":\"" + jsonEsc(nonce) + "\",\"name\":\"" +
           jsonEsc(dirClean(c.name, 60)) + "\",\"max_conns\":" + std::to_string(c.maxConns) + ",\"client\":\"onair-server " + jsonEsc(dirClean(c.version, 20)) + "\"" + (c.listed ? ",\"public\":true" : "") + ",\"sig\":\"" + sigHex + "\"}";
}

std::string relayFrame(uint8_t type, uint32_t id, const void* payload, size_t n) {
    std::string f;
    f.reserve(5 + n);
    f.push_back((char)type);
    for (int i = 3; i >= 0; i--) f.push_back((char)(id >> (8 * i)));
    if (n) f.append((const char*)payload, n);
    return f;
}

bool relayParseFrame(const unsigned char* p, size_t n, uint8_t& type, uint32_t& id, const unsigned char*& payload, size_t& plen) {
    if (n < 5 || p[0] < 1 || p[0] > 3) return false;
    type = p[0];
    id = (uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4];
    payload = p + 5;
    plen = n - 5;
    return true;
}

namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
}

std::string relayRewriteHead(const std::string& head, const std::string& ip) {
    std::string cleanIp;
    bool okIp = true;
    for (char c : ip) { if (isxdigit((unsigned char)c) || c == '.' || c == ':') cleanIp += c; else okIp = false; }
    if (!okIp) cleanIp.clear();
    if (cleanIp.empty() || cleanIp.size() > 45) cleanIp = "0.0.0.0";
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < head.size()) {
        size_t e = head.find("\r\n", at);
        if (e == std::string::npos) e = head.size();
        lines.push_back(head.substr(at, e - at));
        at = e + 2;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    bool upgrade = false;
    for (size_t i = 1; i < lines.size(); i++) {
        const std::string l = lower(lines[i]);
        if (l.compare(0, 8, "upgrade:") == 0 && l.find("websocket") != std::string::npos) upgrade = true;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) {
            const std::string l = lower(lines[i]);
            if (l.compare(0, 16, "x-forwarded-for:") == 0 || l.compare(0, 10, "x-real-ip:") == 0 || l.compare(0, 10, "forwarded:") == 0 ||
                l.compare(0, 18, "x-forwarded-host:") == 0 || l.compare(0, 19, "x-forwarded-proto:") == 0) continue;
            if (!upgrade && l.compare(0, 11, "connection:") == 0) continue;
            if (lines[i].empty()) continue;
        }
        out += lines[i] + "\r\n";
    }
    out += "X-Forwarded-For: " + cleanIp + "\r\n";
    if (!upgrade) out += "Connection: close\r\n";
    out += "\r\n";
    return out;
}

std::string relayAdvice(const std::string& code) {
    if (code == "dns") return "The relay's name cannot be looked up. Check that this computer is online, then it retries by itself.";
    if (code == "unreachable") return "The relay service does not answer. Check the internet connection and that no firewall blocks outgoing connections on port 443; it retries by itself.";
    if (code == "tls") return "The secure connection to the relay failed. A wrong date or time on this computer is the usual cause; so is a network that inspects traffic.";
    if (code == "no_tls") return "This build of OnAir has no TLS library, so it cannot reach the relay. Use the Cloudflare tunnel option instead, or a build with TLS.";
    if (code == "clock") return "The relay says this computer's clock is off by too much. Set the date and time to automatic.";
    if (code == "rejected") return "The relay refused this server. See the message; if it keeps happening the key file may need to be made again.";
    if (code == "busy") return "The relay is full right now. It retries by itself.";
    if (code == "banned") return "The relay has blocked this server's key. Contact the relay's operator.";
    if (code == "version") return "The relay needs a newer OnAir. Update the program.";
    if (code == "local") return "The web server on this computer does not answer. Check that OnAir Web is running and its port is not used by another program.";
    return "";
}

std::string relayStatusJson(const RelayStatus& s) {
    return "{\"state\":\"" + s.state + "\",\"url\":\"" + jsonEsc(s.url) + "\",\"code\":\"" + jsonEsc(s.code) + "\",\"message\":\"" + jsonEsc(s.message) + "\",\"advice\":\"" + jsonEsc(s.advice) +
           "\",\"conns\":" + std::to_string(s.conns) + ",\"conns_total\":" + std::to_string(s.connsTotal) + ",\"bytes_in\":" + std::to_string(s.bytesIn) + ",\"bytes_out\":" + std::to_string(s.bytesOut) +
           ",\"retry_in\":" + std::to_string(s.retryIn) + ",\"online_since\":" + std::to_string(s.onlineSince) + "}\n";
}

bool relayParseStatus(const std::string& json, RelayStatus& o) {
    Json j;
    std::string err;
    if (!jsonParse(json, j, err) || j.kind != Json::Obj) return false;
    auto S = [&](const char* k) { const Json* v = j.get(k); return v && v->kind == Json::Str ? v->str : std::string(); };
    auto N = [&](const char* k) { const Json* v = j.get(k); return v && v->kind == Json::Num ? (long long)v->num : 0LL; };
    o.state = S("state"); o.url = S("url"); o.code = S("code"); o.message = S("message"); o.advice = S("advice");
    o.conns = (int)N("conns"); o.connsTotal = N("conns_total"); o.bytesIn = N("bytes_in"); o.bytesOut = N("bytes_out"); o.retryIn = N("retry_in"); o.onlineSince = N("online_since");
    return !o.state.empty();
}

// ---------------------------------------------------------------- the client

namespace {

long long wall() { return (long long)time(nullptr); }

void randomBytes(uint8_t* p, size_t n) {
#ifndef _WIN32
    std::ifstream u("/dev/urandom", std::ios::binary);
    if (u.read((char*)p, (std::streamsize)n) && (size_t)u.gcount() == n) return;
#endif
    std::random_device rd;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rd() & 0xFF);
}

// the transport of the link: a socket, with TLS when wss
struct Io {
    dect2::sock_t s = dect2::kBadSock;
    dect2::tls::Ctx* ctx = nullptr;
    dect2::tls::Conn* t = nullptr;
    ~Io() { closeAll(); }
    void closeAll() {
        if (t) { dect2::tls::close(t); t = nullptr; }
        if (ctx) { dect2::tls::freeCtx(ctx); ctx = nullptr; }
        if (s != dect2::kBadSock) { dect2::sockClose(s); s = dect2::kBadSock; }
    }
    void abort() { if (s != dect2::kBadSock) dect2::sockShutdown(s); }   // wakes a blocked read in another thread
    bool readable(int ms) {
        if (t && dect2::tls::pending(t)) return true;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        return select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0;
    }
    long rd(void* d, size_t n) { return t ? dect2::tls::read(t, d, n) : (long)dect2::sockRecv(s, d, n); }
    bool wr(const void* d, size_t n) {
        if (!t) return dect2::sockSendAll(s, d, n);
        const char* p = (const char*)d;
        while (n > 0) { const long k = dect2::tls::write(t, p, n); if (k <= 0) return false; p += k; n -= (size_t)k; }
        return true;
    }
};

struct Link {
    Io io;
    std::mutex sendMu;
    std::atomic<bool> dead{false};

    bool sendFrame(int op, const void* d, size_t n) {
        std::string f;
        f.push_back((char)(0x80 | op));
        if (n < 126) f.push_back((char)(0x80 | n));
        else if (n < 65536) { f.push_back((char)(0x80 | 126)); f.push_back((char)(n >> 8)); f.push_back((char)n); }
        else { f.push_back((char)(0x80 | 127)); for (int i = 7; i >= 0; i--) f.push_back((char)((uint64_t)n >> (8 * i))); }
        uint8_t m[4];
        randomBytes(m, 4);
        f.append((const char*)m, 4);
        const unsigned char* p = (const unsigned char*)d;
        const size_t at = f.size();
        f.resize(at + n);
        for (size_t i = 0; i < n; i++) f[at + i] = (char)(p[i] ^ m[i & 3]);
        std::lock_guard<std::mutex> lk(sendMu);
        if (dead) return false;
        if (!io.wr(f.data(), f.size())) { dead = true; return false; }
        return true;
    }
    bool sendText(const std::string& s) { return sendFrame(0x1, s.data(), s.size()); }
    bool sendBin(const std::string& s) { return sendFrame(0x2, s.data(), s.size()); }

    // all n bytes within `ms` of silence, else false
    bool readExact(void* d, size_t n, int ms, const std::atomic<bool>& stop) {
        char* p = (char*)d;
        size_t got = 0;
        int waited = 0;
        while (got < n) {
            if (stop) return false;
            if (!io.readable(250)) { waited += 250; if (waited >= ms) return false; continue; }
            const long k = io.rd(p + got, n - got);
            if (k <= 0) return false;
            got += (size_t)k;
            waited = 0;
        }
        return true;
    }

    enum class Got { Text, Binary, Idle, Closed };
    // One message. Idle: nothing started within idleMs. Pings are answered, pongs dropped.
    Got readMsg(std::vector<unsigned char>& out, int idleMs, const std::atomic<bool>& stop, size_t maxMsg = 1u << 20) {
        out.clear();
        int kind = -1;
        for (;;) {
            unsigned char h[2];
            if (kind < 0) {
                int waited = 0;
                for (;;) {
                    if (stop) return Got::Closed;
                    if (io.readable(250)) break;
                    waited += 250;
                    if (waited >= idleMs) return Got::Idle;
                }
            }
            if (!readExact(h, 2, 15000, stop)) return Got::Closed;
            const bool fin = h[0] & 0x80;
            const int op = h[0] & 0x0F;
            if ((h[0] & 0x70) || (h[1] & 0x80)) return Got::Closed;
            uint64_t len = h[1] & 0x7F;
            if (len == 126) { unsigned char x[2]; if (!readExact(x, 2, 15000, stop)) return Got::Closed; len = (uint64_t)x[0] << 8 | x[1]; }
            else if (len == 127) { unsigned char x[8]; if (!readExact(x, 8, 15000, stop)) return Got::Closed; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | x[i]; }
            if (op >= 8) {
                if (len > 125 || !fin) return Got::Closed;
                std::vector<unsigned char> p((size_t)len);
                if (len && !readExact(p.data(), p.size(), 15000, stop)) return Got::Closed;
                if (op == 8) { sendFrame(8, p.data(), p.size() >= 2 ? 2 : 0); return Got::Closed; }
                if (op == 9) sendFrame(10, p.data(), p.size());
                continue;
            }
            if (op == 0 ? kind < 0 : kind >= 0) return Got::Closed;
            if (op != 0) { if (op != 1 && op != 2) return Got::Closed; kind = op; }
            if (len > maxMsg || out.size() + len > maxMsg) return Got::Closed;
            const size_t at = out.size();
            out.resize(at + (size_t)len);
            if (len && !readExact(out.data() + at, (size_t)len, 15000, stop)) return Got::Closed;
            if (fin) return kind == 1 ? Got::Text : Got::Binary;
        }
    }
};

struct VConn {
    uint32_t id = 0;
    dect2::sock_t local = dect2::kBadSock;
    std::string ip, head;
    bool headDone = false;
    std::thread rd;
    std::atomic<bool> closing{false};
    std::mutex wmu;
};

class Impl : public RelayClient {
public:
    ~Impl() override { stop(); }

    bool start(const RelayConfig& c, std::string& err) override {
        if (th_.joinable()) { err = "already running"; return false; }
        c_ = c;
        if (!relayParseUrl(c_.url, url_, err)) return false;
        if (url_.tls && !dect2::tls::available()) {
            set([&](RelayStatus& s) { s = RelayStatus(); s.state = "failed"; s.code = "no_tls"; s.message = "this build has no TLS library"; s.advice = relayAdvice("no_tls"); });
            err = "this build has no TLS library, so it cannot connect to a wss:// relay";
            return false;
        }
        if (!loadKey(err)) return false;
        if (c_.maxConns < 1) c_.maxConns = 1;
        stop_ = false;
        set([&](RelayStatus& s) { s = RelayStatus(); s.state = "connecting"; });
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        stop_ = true;
        {
            std::lock_guard<std::mutex> lk(linkMu_);
            if (link_) link_->io.abort();
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        set([&](RelayStatus& s) { s.state = "off"; s.url.clear(); s.conns = 0; });
    }

    RelayStatus status() const override { std::lock_guard<std::mutex> lk(m_); return st_; }

private:
    RelayConfig c_;
    RelayUrl url_;
    uint8_t seed_[32]{};
    std::string pub_, id_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    RelayStatus st_;
    std::mutex cvm_;
    std::condition_variable cv_;
    std::mutex linkMu_;
    Link* link_ = nullptr;
    std::map<uint32_t, std::shared_ptr<VConn>> conns_;
    std::mutex cm_;
    std::atomic<long long> bIn_{0}, bOut_{0}, total_{0};
    long long lastWrite_ = 0;

    void set(const std::function<void(RelayStatus&)>& f) {
        RelayStatus copy;
        {
            std::lock_guard<std::mutex> lk(m_);
            f(st_);
            copy = st_;
        }
        writeStatus(copy);
    }
    void writeStatus(const RelayStatus& s) {
        if (c_.statusFile.empty()) return;
        std::string werr;
        onair_server::writeHostFile(c_.statusFile, relayStatusJson(s), werr, true);
    }
    long long now() const { return c_.nowOverride ? c_.nowOverride : wall(); }

    bool loadKey(std::string& err) {
        if (c_.keyFile.empty()) { err = "no place for the key file"; return false; }
        onair_server::ConfigFile kf;
        kf.load(c_.keyFile);
        std::vector<uint8_t> s;
        if (!mc::hexDecode(kf.get("seed"), s) || s.size() != 32) {
            randomBytes(seed_, 32);
            kf.set("seed", mc::hexEncode(seed_, 32));
            kf.set("public_key", dirPubHex(seed_));
            std::error_code ec;
            fs::create_directories(fs::path(c_.keyFile).parent_path(), ec);
            if (!kf.save(c_.keyFile, err)) { err = "cannot write the key file: " + err; return false; }
            fprintf(stderr, "onair-server: made a new key in %s (it proves this server is the one at its relay address; keep it)\n", c_.keyFile.c_str());
        } else memcpy(seed_, s.data(), 32);
        pub_ = dirPubHex(seed_);
        id_ = dirIdFromPub(pub_);
        return true;
    }

    void sleepFor(int ms) {
        std::unique_lock<std::mutex> lk(cvm_);
        cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_.load(); });
    }

    void problem(const std::string& code, const std::string& msg, const std::string& state) {
        set([&](RelayStatus& s) { s.state = state; s.url.clear(); s.code = code; s.message = msg; s.advice = relayAdvice(code); s.conns = 0; });
    }

    void run() {
        int back = 2;
        std::mt19937 rng((unsigned)wall());
        while (!stop_) {
            const long long t0 = wall();
            std::string code, msg;
            bool fatal = false;
            session(code, msg, fatal);
            if (stop_) break;
            const long long up = wall() - t0;
            if (up > 30) back = 2;
            const bool slow = fatal;
            int wait = slow ? 300 : back;
            problem(code.empty() ? "unreachable" : code, msg.empty() ? "the link to the relay ended" : msg, slow ? "failed" : "retrying");
            wait = std::max(1, wait + (int)(rng() % (unsigned)std::max(1, wait / 4 + 1)));
            for (int i = wait; i > 0 && !stop_; i--) {
                set([&](RelayStatus& s) { s.retryIn = i; });
                sleepFor(1000);
            }
            back = std::min(back * 2, 60);
            set([&](RelayStatus& s) { s.state = "connecting"; s.retryIn = 0; });
        }
    }

    // One connection to the relay, from connect to the end. code / msg: why it ended.
    void session(std::string& code, std::string& msg, bool& fatal) {
        std::string err;
        Link link;
        link.io.s = dect2::tcpConnect(url_.host, url_.port, 10000, err);
        if (link.io.s == dect2::kBadSock) {
            code = err.compare(0, 11, "cannot find") == 0 ? "dns" : "unreachable";
            msg = err;
            return;
        }
        dect2::sockRecvTimeout(link.io.s, 15000);
        if (url_.tls) {
            link.io.ctx = dect2::tls::makeClientCtx(err);
            if (!link.io.ctx) { code = "tls"; msg = err; return; }
            link.io.t = dect2::tls::connect(link.io.ctx, link.io.s, url_.host, err);
            if (!link.io.t) { code = "tls"; msg = err; return; }
        }
        {
            std::lock_guard<std::mutex> lk(linkMu_);
            link_ = &link;
        }
        struct Reset { Impl* p; ~Reset() { std::lock_guard<std::mutex> lk(p->linkMu_); p->link_ = nullptr; } } reset{this};
        // WebSocket upgrade
        uint8_t kr[16];
        randomBytes(kr, 16);
        const std::string key = mc::base64Encode(kr, 16);
        const std::string hp = (url_.host.find(':') != std::string::npos ? "[" + url_.host + "]" : url_.host) + ":" + std::to_string(url_.port);
        const std::string req = "GET " + url_.path + " HTTP/1.1\r\nHost: " + hp + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: onair-server/" + c_.version + "\r\n\r\n";
        if (!link.io.wr(req.data(), req.size())) { code = "unreachable"; msg = "the connection dropped during the handshake"; return; }
        std::string head;
        char ch;
        while (head.size() < 8192 && !stop_) {
            if (!link.io.readable(15000) || link.io.rd(&ch, 1) <= 0) { code = "unreachable"; msg = "no answer to the WebSocket handshake"; return; }
            head += ch;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (stop_) return;
        if (head.compare(0, 5, "HTTP/") != 0 || head.find(" 101") == std::string::npos || head.find(" 101") > 12) {
            code = "rejected";
            msg = "the relay did not accept the WebSocket: " + head.substr(0, std::min<size_t>(head.find("\r\n"), 60));
            return;
        }
        if (lower(head).find(lower(dect2::wsAcceptKey(key))) == std::string::npos) { code = "rejected"; msg = "the relay's handshake answer does not match"; return; }
        // challenge, hello, ready
        std::vector<unsigned char> m;
        if (link.readMsg(m, 15000, stop_) != Link::Got::Text) { code = "unreachable"; msg = "the relay sent no challenge"; return; }
        Json j;
        if (!jsonParse(std::string(m.begin(), m.end()), j, err) || j.kind != Json::Obj) { code = "rejected"; msg = "the relay's first message is not valid"; return; }
        const Json* ty = j.get("type");
        if (ty && ty->kind == Json::Str && ty->str == "error") return relayError(j, code, msg, fatal);
        const Json* nn = j.get("nonce");
        if (!ty || ty->str != "challenge" || !nn || nn->kind != Json::Str || nn->str.empty() || nn->str.size() > 128) { code = "rejected"; msg = "the relay's first message is not a challenge"; return; }
        const long long ts = now();
        const std::string text = relayCanonical(id_, pub_, ts, nn->str);
        uint8_t sig[64];
        mc::ed25519Sign(seed_, (const uint8_t*)text.data(), text.size(), sig);
        if (!link.sendText(relayHelloJson(c_, id_, pub_, ts, nn->str, mc::hexEncode(sig, 64)))) { code = "unreachable"; msg = "the connection dropped"; return; }
        if (link.readMsg(m, 15000, stop_) != Link::Got::Text) { code = "unreachable"; msg = "the relay did not answer the hello"; return; }
        Json r;
        if (!jsonParse(std::string(m.begin(), m.end()), r, err) || r.kind != Json::Obj) { code = "rejected"; msg = "the relay's answer is not valid"; return; }
        ty = r.get("type");
        if (ty && ty->kind == Json::Str && ty->str == "error") return relayError(r, code, msg, fatal);
        const Json* u = r.get("url");
        if (!ty || ty->str != "ready" || !u || u->kind != Json::Str) { code = "rejected"; msg = "the relay's answer is not ready"; return; }
        std::string pubUrl, uerr;
        if (!dirNormalizeUrl(u->str, pubUrl, uerr)) { code = "rejected"; msg = "the relay gave an address that is not valid: " + uerr; return; }
        const long long since = wall();
        set([&](RelayStatus& s) { s.state = "online"; s.url = pubUrl; s.code.clear(); s.message.clear(); s.advice.clear(); s.onlineSince = since; s.retryIn = 0; });
        serve(link, code, msg);
        closeAll();
    }

    void relayError(const Json& j, std::string& code, std::string& msg, bool& fatal) {
        const Json* c = j.get("code");
        const Json* mm = j.get("message");
        const std::string rc = c && c->kind == Json::Str ? c->str : "rejected";
        code = (rc == "clock" || rc == "busy" || rc == "banned" || rc == "version") ? rc : "rejected";
        msg = mm && mm->kind == Json::Str ? dirClean(mm->str, 200) : std::string("the relay refused this server");
        fatal = code == "banned" || code == "version";
    }

    void closeConn(const std::shared_ptr<VConn>& v, Link* link, bool tell) {
        bool was = false;
        if (!v->closing.compare_exchange_strong(was, true)) return;
        if (v->local != dect2::kBadSock) dect2::sockShutdown(v->local);
        if (tell && link) { const std::string f = relayFrame(kRelayClose, v->id, nullptr, 0); link->sendBin(f); }
    }

    void closeAll() {
        std::vector<std::shared_ptr<VConn>> all;
        {
            std::lock_guard<std::mutex> lk(cm_);
            for (auto& kv : conns_) all.push_back(kv.second);
            conns_.clear();
        }
        for (auto& v : all) closeConn(v, nullptr, false);
        for (auto& v : all) { if (v->rd.joinable()) v->rd.join(); if (v->local != dect2::kBadSock) { dect2::sockClose(v->local); v->local = dect2::kBadSock; } }
    }

    void reap() {
        std::vector<std::shared_ptr<VConn>> done;
        {
            std::lock_guard<std::mutex> lk(cm_);
            for (auto it = conns_.begin(); it != conns_.end();) {
                if (it->second->closing) { done.push_back(it->second); it = conns_.erase(it); } else ++it;
            }
        }
        for (auto& v : done) { if (v->rd.joinable()) v->rd.join(); if (v->local != dect2::kBadSock) { dect2::sockClose(v->local); v->local = dect2::kBadSock; } }
    }

    void localReader(Link* link, std::shared_ptr<VConn> v) {
        std::vector<char> buf(16384);
        while (!v->closing && !stop_) {
            const long long k = dect2::sockRecv(v->local, buf.data(), buf.size());
            if (k <= 0) break;
            bOut_ += k;
            const std::string f = relayFrame(kRelayData, v->id, buf.data(), (size_t)k);
            if (!link->sendBin(f)) break;
        }
        if (!v->closing) closeConn(v, link, true);
        else if (link && !link->dead) { /* the relay closed it: nothing to tell */ }
    }

    void openConn(Link& link, uint32_t id, const std::string& ip) {
        {
            std::lock_guard<std::mutex> lk(cm_);
            if ((int)conns_.size() >= c_.maxConns || conns_.count(id)) { const std::string f = relayFrame(kRelayClose, id, nullptr, 0); link.sendBin(f); return; }
        }
        std::string err;
        dect2::sock_t s = dect2::tcpConnect("127.0.0.1", c_.localPort, 2000, err);
        if (s == dect2::kBadSock) {
            const std::string f = relayFrame(kRelayClose, id, nullptr, 0);
            link.sendBin(f);
            set([&](RelayStatus& st) { st.message = "the web server on this computer does not answer on port " + std::to_string(c_.localPort); st.code = "local"; st.advice = relayAdvice("local"); });
            return;
        }
#ifdef _WIN32
        { DWORD t = 5000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t); }
#else
        { timeval tv{5, 0}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
#endif
        auto v = std::make_shared<VConn>();
        v->id = id; v->local = s; v->ip = ip;
        {
            std::lock_guard<std::mutex> lk(cm_);
            conns_[id] = v;
        }
        total_++;
        v->rd = std::thread([this, &link, v] { localReader(&link, v); });
    }

    void data(Link& link, uint32_t id, const unsigned char* p, size_t n) {
        std::shared_ptr<VConn> v;
        {
            std::lock_guard<std::mutex> lk(cm_);
            auto it = conns_.find(id);
            if (it != conns_.end()) v = it->second;
        }
        if (!v || v->closing) return;
        bIn_ += (long long)n;
        std::string out;
        if (!v->headDone) {
            v->head.append((const char*)p, n);
            const size_t e = v->head.find("\r\n\r\n");
            if (e == std::string::npos) { if (v->head.size() > 16384) closeConn(v, &link, true); return; }
            const std::string h = v->head.substr(0, e + 4), rest = v->head.substr(e + 4);
            v->head.clear();
            v->headDone = true;
            out = relayRewriteHead(h, v->ip) + rest;
        } else out.assign((const char*)p, n);
        if (!dect2::sockSendAll(v->local, out.data(), out.size())) closeConn(v, &link, true);
    }

    void serve(Link& link, std::string& code, std::string& msg) {
        std::vector<unsigned char> m;
        long long lastPing = wall(), lastRx = wall(), lastSt = 0;
        while (!stop_) {
            const Link::Got g = link.readMsg(m, 1000, stop_, 1u << 20);
            const long long t = wall();
            if (g == Link::Got::Closed || link.dead) { code = "unreachable"; msg = "the relay closed the link"; if (stop_) code.clear(); break; }
            if (g != Link::Got::Idle) lastRx = t;
            if (g == Link::Got::Binary) {
                uint8_t type; uint32_t id; const unsigned char* pl; size_t pn;
                if (!relayParseFrame(m.data(), m.size(), type, id, pl, pn)) continue;
                if (type == kRelayOpen) openConn(link, id, std::string((const char*)pl, pn > 64 ? 64 : pn));
                else if (type == kRelayData) data(link, id, pl, pn);
                else {
                    std::shared_ptr<VConn> v;
                    { std::lock_guard<std::mutex> lk(cm_); auto it = conns_.find(id); if (it != conns_.end()) v = it->second; }
                    if (v) closeConn(v, nullptr, false);
                }
            } else if (g == Link::Got::Text) {
                Json j; std::string e;
                if (jsonParse(std::string(m.begin(), m.end()), j, e) && j.kind == Json::Obj) {
                    const Json* ty = j.get("type");
                    if (ty && ty->kind == Json::Str && ty->str == "error") { bool f = false; relayError(j, code, msg, f); break; }
                }
            }
            if (t - lastPing >= 20) { link.sendFrame(9, "", 0); lastPing = t; }
            if (t - lastRx > 70) { code = "unreachable"; msg = "the relay stopped answering"; break; }
            reap();
            if (t != lastSt) {
                lastSt = t;
                int n;
                { std::lock_guard<std::mutex> lk(cm_); n = (int)conns_.size(); }
                if (t - lastWrite_ >= 2) {
                    lastWrite_ = t;
                    set([&](RelayStatus& s) { s.conns = n; s.connsTotal = total_; s.bytesIn = bIn_; s.bytesOut = bOut_; });
                }
            }
        }
        link.sendFrame(8, "\x03\xe8", 2);
        link.dead = true;
    }
};

} // namespace

RelayClient* makeRelayClient() { return new Impl; }

} // namespace onair_web
