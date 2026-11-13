// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Share my radio": see dect2/net/share.h. One thread accepts, sends and reads commands with select(); the engine only converts and queues.
#include "dect2/common/thread_name.h"
#include "dect2/net/share.h"
#include "netcompat.h"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace dect2 {

namespace {
constexpr size_t kMaxPending = 8u << 20;   // bytes waiting for one client: 8 MB is about 2 s of 2 Msps; more is thrown away
bool wouldBlockNow() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
void setNonBlock(sock_t s) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(s, FIONBIO, &m);
#else
    const int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}
} // namespace

void shareToU8(const cf32* x, size_t n, std::vector<unsigned char>& out) {
    out.resize(n * 2);
    for (size_t i = 0; i < n; i++) {
        const float v[2] = {x[i].real(), x[i].imag()};
        for (int k = 0; k < 2; k++) {
            const float b = std::nearbyint(v[k] * 128.f + 127.4f);
            out[2 * i + k] = (unsigned char)(b < 0.f ? 0.f : b > 255.f ? 255.f : b);
        }
    }
}

struct ShareServer::Impl {
    struct Client { sock_t s = kBadSock; std::string name; std::string pending; unsigned char in[5] = {}; int inN = 0; };
    mutable std::mutex mu;
    ShareConfig cfg, live;
    std::vector<Client> clients;
    std::vector<ShareCommand> cmds;
    ShareStatus st;
    std::vector<unsigned char> scratch;
    std::thread th;
    std::atomic<bool> quit{false};

    void stopThread() {
        quit = true;
        if (th.joinable()) th.join();
        quit = false;
        std::lock_guard<std::mutex> lk(mu);
        for (Client& c : clients) sockClose(c.s);
        clients.clear();
        st.listening = false;
        st.clients.clear();
    }
    void publishClients() {   // under mu
        st.clients.clear();
        for (const Client& c : clients) st.clients.push_back(c.name);
    }
    void run(ShareConfig c) {
        netInit();
        sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) { std::lock_guard<std::mutex> lk(mu); st.lastError = "cannot open a socket"; return; }
#ifndef _WIN32
        { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(c.lan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)c.port);
        if (c.port < 0 || c.port > 65535 || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
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
        setNonBlock(ls);
        { std::lock_guard<std::mutex> lk(mu); st.listening = true; st.lastError.clear(); }
        while (!quit) {
            fd_set rd, wr;
            FD_ZERO(&rd); FD_ZERO(&wr);
            FD_SET(ls, &rd);
            sock_t mx = ls;
            bool anyClient = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                anyClient = !clients.empty();
                for (const Client& cl : clients) {
                    FD_SET(cl.s, &rd);
                    if (!cl.pending.empty()) FD_SET(cl.s, &wr);
                    if (cl.s > mx) mx = cl.s;
                }
            }
            timeval tv{0, anyClient ? 20000 : 250000};   // nobody connected: 4 wakeups a second, a new connection wakes select at once
            const int r = select((int)mx + 1, &rd, &wr, nullptr, &tv);
            if (r < 0) { if (wouldBlockNow()) continue; break; }
            std::lock_guard<std::mutex> lk(mu);
            if (FD_ISSET(ls, &rd)) {
                sockaddr_in pa{}; socklen_t pl = sizeof pa;
                const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
                if (k != kBadSock) {
                    setNonBlock(k);
#ifdef SO_NOSIGPIPE
                    { int one = 1; setsockopt(k, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
                    Client cl;
                    cl.s = k;
                    char ip[64] = "?";
                    inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
                    cl.name = std::string(ip) + ":" + std::to_string(ntohs(pa.sin_port));
                    // the greeting: "RTL0", tuner type 0 (not an RTL-SDR tuner: an OnAir client then takes any rate), 0 gains
                    const unsigned char hdr[12] = {'R', 'T', 'L', '0', 0, 0, 0, 0, 0, 0, 0, 0};
                    cl.pending.assign((const char*)hdr, sizeof hdr);
                    clients.push_back(std::move(cl));
                    publishClients();
                }
            }
            bool changed = false;
            for (size_t i = 0; i < clients.size();) {
                Client& cl = clients[i];
                bool dead = false;
                if (FD_ISSET(cl.s, &rd)) {
                    unsigned char b[256];
                    const long long k = sockRecv(cl.s, b, sizeof b);
                    if (k == 0 || (k < 0 && !wouldBlockNow())) dead = true;
                    else for (long long j = 0; j < k; j++) {
                        cl.in[cl.inN++] = b[j];
                        if (cl.inN == 5) {
                            cl.inN = 0;
                            if (live.allowControl && cmds.size() < 1000)
                                cmds.push_back({cl.in[0], (uint32_t)cl.in[1] << 24 | (uint32_t)cl.in[2] << 16 | (uint32_t)cl.in[3] << 8 | cl.in[4]});
                        }
                    }
                }
                if (!dead && FD_ISSET(cl.s, &wr) && !cl.pending.empty()) {
#if defined(MSG_NOSIGNAL)
                    const int fl = MSG_NOSIGNAL;
#else
                    const int fl = 0;
#endif
                    const long long k = send(cl.s, cl.pending.data(), cl.pending.size(), fl);
                    if (k > 0) { cl.pending.erase(0, (size_t)k); st.sentBytes += (uint64_t)k; }
                    else if (k < 0 && !wouldBlockNow()) dead = true;
                }
                if (dead) { sockClose(cl.s); clients.erase(clients.begin() + (long)i); changed = true; }
                else i++;
            }
            if (changed) publishClients();
        }
        sockClose(ls);
    }
};

ShareServer::ShareServer() : p_(new Impl) {}
ShareServer::~ShareServer() { p_->stopThread(); }
ShareServer& ShareServer::get() { static ShareServer s; return s; }

void ShareServer::configure(const ShareConfig& cIn) {
    ShareConfig c = cIn;
    Impl& p = *p_;
    bool restart;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        const int cur = p.cfg.on && p.cfg.port == 0 ? p.live.port : p.cfg.port;   // port 0 was asked for: the one it got
        restart = c.on != p.cfg.on || c.port != cur || c.lan != p.cfg.lan;
        if (!restart) c.port = p.cfg.port;
        p.cfg = c;
        p.live.allowControl = c.allowControl;
    }
    if (!restart) return;
    p.stopThread();
    active_ = false;
    if (!c.on) return;
    { std::lock_guard<std::mutex> lk(p.mu); p.live = c; p.st.lastError.clear(); p.st.sentBytes = 0; p.st.droppedBytes = 0; }
    active_ = true;
    p.th = dect2::namedThread("onair-share", [this, c] { p_->run(c); });
    // wait until it listens (or failed), so that port 0 can be read back
    for (int i = 0; i < 400; i++) {
        { std::lock_guard<std::mutex> lk(p.mu); if (p.st.listening || !p.st.lastError.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

ShareConfig ShareServer::config() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    ShareConfig c = p_->cfg;
    if (c.on && c.port == 0) c.port = p_->live.port;
    return c;
}

void ShareServer::offer(const cf32* x, size_t n) {
    if (!active_.load(std::memory_order_relaxed) || n == 0) return;
    Impl& p = *p_;
    std::unique_lock<std::mutex> lk(p.mu, std::try_to_lock);
    if (!lk.owns_lock() || p.clients.empty()) return;   // the engine never waits for the network
    shareToU8(x, n, p.scratch);
    for (Impl::Client& c : p.clients) {
        if (c.pending.size() + p.scratch.size() > kMaxPending) { p.st.droppedBytes += p.scratch.size(); continue; }
        c.pending.append((const char*)p.scratch.data(), p.scratch.size());
    }
}

std::vector<ShareCommand> ShareServer::takeCommands() {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<ShareCommand> v;
    v.swap(p_->cmds);
    return v;
}

ShareStatus ShareServer::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

} // namespace dect2
