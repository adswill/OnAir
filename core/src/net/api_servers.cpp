// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The rigctl TCP server and the UDP output of the remote API: see dect2/net/api_servers.h.
#include "dect2/net/api_servers.h"
#include "dect2/common/thread_name.h"
#include "netcompat.h"
#include "tcp_client.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace dect2 {

static double steadySec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool validUdpHost(const std::string& h) {
    if (h.empty() || h.size() > 15) return false;
    int parts = 0, v = -1;
    for (size_t i = 0; i <= h.size(); i++) {
        const char c = i < h.size() ? h[i] : '.';
        if (c == '.') {
            if (v < 0 || v > 255) return false;
            parts++; v = -1;
        } else if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v) * 10 + (c - '0');
        else return false;
    }
    return parts == 4;
}

// ------------------------------------------------------------------ rigctl
struct RigctlServer::Impl {
    explicit Impl(apipol::RigctlRadio& r) : radio(r) {}
    apipol::RigctlRadio& radio;
    mutable std::mutex mu;
    std::mutex radioMu;   // one command at a time into the radio
    RigctlConfig cfg;
    int livePort = 0;
    std::string err;
    std::thread th;
    std::atomic<bool> quit{false};
    std::atomic<int> nClients{0};
    struct Conn { sock_t s = kBadSock; std::thread th; std::atomic<bool> done{false}; };
    std::vector<std::unique_ptr<Conn>> conns;

    void stopAll() {
        quit = true;
        if (th.joinable()) th.join();
        quit = false;
        std::lock_guard<std::mutex> lk(mu);
        livePort = 0;
    }
    void serve(Conn* c) {
        sock_t s = c->s;
        sockRecvTimeout(s, 200);
        apipol::RateLimiter lim(50, 100);
        apipol::RigctlSession sess;
        // loopback: the HTTP guard protects; open to the LAN: changes need "\auth <token>" first
        { std::lock_guard<std::mutex> lk(mu); sess.token = cfg.token; sess.required = cfg.allowLan; }
        std::string buf;
        double lastData = steadySec();
        bool bye = false;
        while (!quit && !bye) {
            char b[512];
            const long long k = sockRecv(s, b, sizeof b);
            if (k == 0) break;
            if (k < 0) {
                if (steadySec() - lastData > 300) break;   // idle for five minutes
                continue;   // the 200 ms receive timeout
            }
            lastData = steadySec();
            buf.append(b, (size_t)k);
            size_t nl;
            while (!bye && (nl = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string reply;
                if (!lim.allow("c", steadySec())) reply = "RPRT -9\n";
                else {
                    bool q = false;
                    { std::lock_guard<std::mutex> lk(radioMu); reply = apipol::rigctlHandle(line, radio, q, sess); }
                    bye = q;
                }
                if (!reply.empty() && !sockSendAll(s, reply.data(), reply.size())) bye = true;
            }
            if (buf.size() > apipol::kMaxRigctlLine) {   // a line that never ends: refuse and hang up
                const std::string r = "RPRT -8\n";
                sockSendAll(s, r.data(), r.size());
                break;
            }
        }
        sockShutdown(s);
        nClients--;
        c->done = true;
    }
    void reap() {
        for (size_t i = 0; i < conns.size();) {
            if (conns[i]->done) { if (conns[i]->th.joinable()) conns[i]->th.join(); sockClose(conns[i]->s); conns.erase(conns.begin() + (long)i); }
            else i++;
        }
    }
    void run(RigctlConfig c) {
        netInit();
        sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) { std::lock_guard<std::mutex> lk(mu); err = "cannot open a socket"; return; }
#ifndef _WIN32
        { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(c.allowLan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)c.port);
        if (c.port < 0 || c.port > 65535 || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
            std::lock_guard<std::mutex> lk(mu);
            err = "cannot listen on TCP port " + std::to_string(c.port) + " (in use by another program?)";
            sockClose(ls);
            return;
        }
        {
            sockaddr_in b{}; socklen_t bl = sizeof b;
            std::lock_guard<std::mutex> lk(mu);
            if (getsockname(ls, (sockaddr*)&b, &bl) == 0) livePort = ntohs(b.sin_port);
            err.clear();
        }
        sockSetNonBlocking(ls, true);
        while (!quit) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 50000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) > 0) {
                sockaddr_in pa{}; socklen_t pl = sizeof pa;
                const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
                if (k != kBadSock) {
                    sockSetNonBlocking(k, false);
#ifdef SO_NOSIGPIPE
                    { int one = 1; setsockopt(k, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
                    reap();
                    if (conns.size() >= 4) { sockClose(k); continue; }
                    auto cn = std::make_unique<Conn>();
                    cn->s = k;
                    Conn* raw = cn.get();
                    nClients++;
                    cn->th = dect2::namedThread("onair-rigctl-c", [this, raw] { serve(raw); });
                    conns.push_back(std::move(cn));
                }
            } else reap();
        }
        sockClose(ls);
        for (auto& c2 : conns) sockShutdown(c2->s);
        for (auto& c2 : conns) { if (c2->th.joinable()) c2->th.join(); sockClose(c2->s); }
        conns.clear();
    }
};

RigctlServer::RigctlServer(apipol::RigctlRadio& r) : p_(new Impl(r)) {}
RigctlServer::~RigctlServer() { p_->stopAll(); }

bool RigctlServer::configure(const RigctlConfig& c) {
    Impl& p = *p_;
    p.stopAll();
    { std::lock_guard<std::mutex> lk(p.mu); p.cfg = c; p.err.clear(); }
    if (!c.on) return true;
    p.th = dect2::namedThread("onair-rigctl", [&p, c] { p.run(c); });
    for (int i = 0; i < 400; i++) {
        { std::lock_guard<std::mutex> lk(p.mu); if (p.livePort || !p.err.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lk(p.mu);
    return p.livePort != 0;
}
int RigctlServer::port() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->livePort; }
std::string RigctlServer::error() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->err; }
int RigctlServer::clients() const { return p_->nClients; }

// ------------------------------------------------------------------ UDP
struct UdpOut::Impl {
    mutable std::mutex mu;
    sock_t s = kBadSock;
    sockaddr_in to{};
    std::string err;
    std::atomic<uint64_t> n{0};
    void close() { if (s != kBadSock) { sockClose(s); s = kBadSock; } }
};

UdpOut::UdpOut() : p_(new Impl) {}
UdpOut::~UdpOut() { p_->close(); }

bool UdpOut::configure(const UdpOutConfig& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->close();
    p_->err.clear();
    if (!c.on) return true;
    if (!validUdpHost(c.host) || c.port < 1 || c.port > 65535) { p_->err = "the UDP address must be a dotted IPv4 address and a port from 1 to 65535"; return false; }
    netInit();
    p_->s = socket(AF_INET, SOCK_DGRAM, 0);
    if (p_->s == kBadSock) { p_->err = "cannot open a UDP socket"; return false; }
    int one = 1;
    setsockopt(p_->s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    sockSetNonBlocking(p_->s, true);
    p_->to = sockaddr_in{};
    p_->to.sin_family = AF_INET;
    p_->to.sin_port = htons((uint16_t)c.port);
    inet_pton(AF_INET, c.host.c_str(), &p_->to.sin_addr);
    return true;
}

bool UdpOut::send(const std::string& line) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->s == kBadSock || line.size() + 1 > apipol::kMaxUdpLine) return false;
    const std::string d = line + "\n";
    if (sockSendTo(p_->s, d.data(), d.size(), p_->to) != (long long)d.size()) return false;
    p_->n++;
    return true;
}
std::string UdpOut::error() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->err; }
uint64_t UdpOut::sent() const { return p_->n; }

} // namespace dect2
