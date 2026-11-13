// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web relay client: the pure parts (address, signing text, frames, header rewrite, status JSON) and the client against a fake relay and a fake
// web server on 127.0.0.1 (no real relay is ever contacted): handshake, challenge and signed hello, the public address, a virtual connection carried
// to the web server with the listener's address added (a spoofed X-Forwarded-For dropped), the answer carried back, close, reconnect after the link
// is cut, a refusal ("banned") ending in the failed state, and stop().
#include "../../core/src/net/netcompat.h"
#include "../../core/src/net/tcp_client.h"
#include "../../server/web/web_json.h"
#include "../../server/web/web_relay.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/net/remote_api.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
using namespace onair_web;
using namespace dect2;
namespace mc = dect2::meshcrypto;
#include "../common/test_util.h"

namespace {

sock_t listenLocal(int& port) {
    netInit();
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0) return kBadSock;
    socklen_t l = sizeof a;
    getsockname(s, (sockaddr*)&a, &l);
    port = ntohs(a.sin_port);
    return s;
}

// fake web server: answers every connection with the request head it received, then closes
std::atomic<bool> gQuit{false};
void fakeWeb(sock_t ls) {
    while (!gQuit) {
        fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
        timeval tv{0, 100000};
        if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == kBadSock) continue;
        std::thread([c] {
            sockRecvTimeout(c, 3000);
            std::string head;
            char ch;
            while (head.size() < 8192 && sockRecv(c, &ch, 1) == 1) { head += ch; if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break; }
            const std::string r = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(head.size()) + "\r\n\r\n" + head;
            sockSendAll(c, r.data(), r.size());
            sockClose(c);
        }).detach();
    }
}

// the fake relay's end of the WebSocket
struct Peer {
    sock_t s = kBadSock;
    bool send(int op, const std::string& d) {
        std::string f;
        f.push_back((char)(0x80 | op));
        if (d.size() < 126) f.push_back((char)d.size());
        else { f.push_back(126); f.push_back((char)(d.size() >> 8)); f.push_back((char)d.size()); }
        f += d;
        return sockSendAll(s, f.data(), f.size());
    }
    // 1 text, 2 binary, 0 closed / timeout
    int recv(std::string& out) {
        out.clear();
        for (;;) {
            unsigned char h[2];
            if (!sockRecvAll(s, h, 2)) return 0;
            const int op = h[0] & 15;
            uint64_t len = h[1] & 0x7F;
            if (len == 126) { unsigned char x[2]; if (!sockRecvAll(s, x, 2)) return 0; len = (uint64_t)x[0] << 8 | x[1]; }
            else if (len == 127) return 0;
            unsigned char m[4] = {0, 0, 0, 0};
            if ((h[1] & 0x80) && !sockRecvAll(s, m, 4)) return 0;
            out.resize((size_t)len);
            if (len && !sockRecvAll(s, &out[0], (size_t)len)) return 0;
            for (size_t i = 0; i < out.size(); i++) out[i] = (char)(out[i] ^ m[i & 3]);
            if (op == 9) { send(10, out); continue; }
            if (op == 10) continue;
            if (op == 8) return 0;
            return op == 1 ? 1 : 2;
        }
    }
};

bool upgrade(Peer& p) {
    std::string head;
    char ch;
    while (head.size() < 8192 && sockRecv(p.s, &ch, 1) == 1) { head += ch; if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break; }
    const size_t k = head.find("Sec-WebSocket-Key: ");
    if (k == std::string::npos) return false;
    const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
    const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
    return sockSendAll(p.s, r.data(), r.size()) && head.find("GET /relay/v1/host ") == 0;
}

bool verifyHello(const std::string& text, const std::string& nonce, std::string& id) {
    Json j;
    std::string e;
    if (!jsonParse(text, j, e)) return false;
    auto S = [&](const char* k) { const Json* v = j.get(k); return v && v->kind == Json::Str ? v->str : std::string(); };
    if (S("type") != "hello" || S("nonce") != nonce) return false;
    const Json* ts = j.get("ts");
    if (!ts) return false;
    id = S("id");
    std::vector<uint8_t> pub, sig;
    if (!mc::hexDecode(S("pub"), pub) || !mc::hexDecode(S("sig"), sig) || pub.size() != 32 || sig.size() != 64) return false;
    const std::string t = relayCanonical(id, S("pub"), (long long)ts->num, nonce);
    return mc::ed25519Verify(sig.data(), (const uint8_t*)t.data(), t.size(), pub.data());
}

template <class F> bool waitFor(F f, int ms) {
    for (int i = 0; i < ms / 20; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}

} // namespace

int main() {
    // ---- pure parts
    {
        RelayUrl u; std::string e;
        CHECK(relayParseUrl("wss://relay.on-air.dev/relay/v1/host", u, e) && u.tls && u.port == 443 && u.host == "relay.on-air.dev" && u.path == "/relay/v1/host", "url wss");
        CHECK(relayParseUrl("https://relay.example.org:8443", u, e) && u.port == 8443 && u.path == "/relay/v1/host", "url https default path");
        CHECK(relayParseUrl("ws://127.0.0.1:9000/x", u, e) && !u.tls && u.port == 9000, "plain ws on this computer");
        CHECK(!relayParseUrl("ws://relay.example.org/", u, e), "plain ws to the internet refused");
        CHECK(!relayParseUrl("relay.example.org", u, e) && !relayParseUrl("wss://a@b/", u, e) && !relayParseUrl("wss://a b/", u, e) && !relayParseUrl("wss://h/p?x=1", u, e), "bad urls");
        const std::string f = relayFrame(kRelayData, 0x01020304, "hi", 2);
        uint8_t t; uint32_t id; const unsigned char* p; size_t n;
        CHECK(f.size() == 7 && relayParseFrame((const unsigned char*)f.data(), f.size(), t, id, p, n) && t == kRelayData && id == 0x01020304 && n == 2 && p[0] == 'h', "frame round trip");
        CHECK(!relayParseFrame((const unsigned char*)"\x09\0\0\0\0", 5, t, id, p, n) && !relayParseFrame((const unsigned char*)"\x01\0\0", 3, t, id, p, n), "bad frames");
        const std::string h = relayRewriteHead("GET /web/v1/ws HTTP/1.1\r\nHost: a\r\nx-forwarded-for: 1.2.3.4\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nX-Real-IP: 9.9.9.9\r\n\r\n", "203.0.113.9");
        CHECK(h.find("1.2.3.4") == std::string::npos && h.find("9.9.9.9") == std::string::npos && h.find("X-Forwarded-For: 203.0.113.9\r\n") != std::string::npos &&
              h.find("Connection: Upgrade") != std::string::npos && h.find("Connection: close") == std::string::npos && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0, "upgrade head rewritten: %s", h.c_str());
        const std::string h2 = relayRewriteHead("GET /web/v1/info HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nForwarded: for=1.1.1.1\r\n\r\n", "bad ip;");
        CHECK(h2.find("keep-alive") == std::string::npos && h2.find("Connection: close") != std::string::npos && h2.find("for=1.1.1.1") == std::string::npos && h2.find("X-Forwarded-For: 0.0.0.0") != std::string::npos, "plain head rewritten: %s", h2.c_str());
        RelayStatus s; s.state = "online"; s.url = "https://x.relay.on-air.dev/"; s.conns = 3; s.bytesIn = 12;
        RelayStatus b;
        CHECK(relayParseStatus(relayStatusJson(s), b) && b.state == "online" && b.url == s.url && b.conns == 3 && b.bytesIn == 12, "status round trip");
        CHECK(!relayAdvice("tls").empty() && !relayAdvice("dns").empty() && relayAdvice("zzz").empty(), "advice");
    }

    // ---- the client against a fake relay
    int relayPort = 0, webPort = 0;
    sock_t rl = listenLocal(relayPort), wl = listenLocal(webPort);
    CHECK(rl != kBadSock && wl != kBadSock, "listen");
    if (rl == kBadSock || wl == kBadSock) return 1;
    std::thread web(fakeWeb, wl);
    std::mutex m;
    std::string gotId, gotReply;
    std::atomic<int> stage{0};
    std::atomic<int> sessions{0};
    std::thread relay([&] {
        for (int session = 1; session <= 2 && !gQuit; session++) {
            fd_set rd; FD_ZERO(&rd); FD_SET(rl, &rd);
            timeval tv{10, 0};
            if (select((int)rl + 1, &rd, nullptr, nullptr, &tv) <= 0) return;
            Peer p;
            p.s = accept(rl, nullptr, nullptr);
            sockRecvTimeout(p.s, 5000);
            sessions = session;
            const std::string nonce = "0123456789abcdef0123456789abcdef";
            std::string msg, id;
            if (!upgrade(p)) { stage = -1; return; }
            p.send(1, "{\"type\":\"challenge\",\"v\":1,\"nonce\":\"" + nonce + "\"}");
            if (p.recv(msg) != 1 || !verifyHello(msg, nonce, id)) { stage = -2; return; }
            { std::lock_guard<std::mutex> lk(m); gotId = id; }
            if (session == 1) {
                p.send(1, "{\"type\":\"ready\",\"url\":\"https://abc123.relay.example.org/\",\"max_conns\":64}");
                // a listener opens a connection; its request has a forged X-Forwarded-For; sent in two pieces
                p.send(2, relayFrame(kRelayOpen, 7, "203.0.113.9", 11));
                const std::string part = "GET /web/v1/info HTTP/1.1\r\nHost: abc123.relay.example.org\r\nX-Forwarded-For: 6.6.6.6\r\n";
                p.send(2, relayFrame(kRelayData, 7, part.data(), part.size()));
                p.send(2, relayFrame(kRelayData, 7, "\r\n", 2));
                std::string reply;
                bool closed = false;
                while (!closed && p.recv(msg) == 2) {
                    uint8_t t; uint32_t cid; const unsigned char* pl; size_t n;
                    if (!relayParseFrame((const unsigned char*)msg.data(), msg.size(), t, cid, pl, n) || cid != 7) continue;
                    if (t == kRelayData) reply.append((const char*)pl, n);
                    else closed = true;
                }
                { std::lock_guard<std::mutex> lk(m); gotReply = reply; }
                stage = 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));   // the link stays up long enough to be seen online
                shutdown(p.s, 2);   // the link is cut: the client must come back
                sockClose(p.s);
            } else {
                p.send(1, "{\"type\":\"error\",\"code\":\"banned\",\"message\":\"this key is blocked\"}");
                stage = 2;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                sockClose(p.s);
            }
        }
    });

    std::string dir = (std::filesystem::temp_directory_path() / ("onair_relay_test_" + std::to_string(relayPort))).string();
    std::filesystem::create_directories(dir);
    RelayConfig c;
    c.on = true;
    c.url = "ws://127.0.0.1:" + std::to_string(relayPort) + "/relay/v1/host";
    c.localPort = webPort;
    c.keyFile = dir + "/relay.key";
    c.statusFile = dir + "/status.json";
    c.name = "Test";
    c.version = "test";
    std::unique_ptr<RelayClient> cl(makeRelayClient());
    std::string err;
    CHECK(cl->start(c, err), "start: %s", err.c_str());
    CHECK(waitFor([&] { return cl->status().state == "online"; }, 5000), "online (state %s %s)", cl->status().state.c_str(), cl->status().message.c_str());
    CHECK(cl->status().url == "https://abc123.relay.example.org/", "public url %s", cl->status().url.c_str());
    CHECK(waitFor([&] { return stage >= 1; }, 5000), "stage 1");
    {
        std::lock_guard<std::mutex> lk(m);
        CHECK(gotId.compare(0, 6, "onair-") == 0, "id %s", gotId.c_str());
        CHECK(gotReply.find("HTTP/1.1 200 OK") == 0, "the web server's answer came back: %s", gotReply.c_str());
        CHECK(gotReply.find("X-Forwarded-For: 203.0.113.9\r\n") != std::string::npos && gotReply.find("6.6.6.6") == std::string::npos, "listener address set, forged one dropped: %s", gotReply.c_str());
        CHECK(gotReply.find("Connection: close") != std::string::npos && gotReply.find("GET /web/v1/info HTTP/1.1") != std::string::npos, "head intact");
    }
    RelayStatus st;
    CHECK(waitFor([&] { RelayStatus x; std::ifstream f(c.statusFile); std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); return relayParseStatus(t, x) && x.state != "off"; }, 2000), "status file written");
    // the link was cut: it retries (about 2 s) and the second session is refused: failed, with the relay's words
    CHECK(waitFor([&] { return stage >= 2; }, 12000), "reconnected after the cut (stage %d)", (int)stage);
    CHECK(waitFor([&] { st = cl->status(); return st.state == "failed" && st.code == "banned"; }, 4000), "banned -> failed (%s / %s)", st.state.c_str(), st.code.c_str());
    CHECK(st.message.find("blocked") != std::string::npos && !st.advice.empty(), "message and advice: %s | %s", st.message.c_str(), st.advice.c_str());
    cl->stop();
    CHECK(cl->status().state == "off", "stopped");
    gQuit = true;
    relay.join();
    web.join();
    sockClose(rl);
    sockClose(wl);
    // no TLS build or a wss address with no library: start() says so instead of hanging (only when the build has no TLS)
    std::filesystem::remove_all(dir);
    return testSummary("web_relay");
}
