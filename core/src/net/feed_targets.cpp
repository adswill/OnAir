// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Outbound feed targets (feed_targets.h): one thread, a bounded queue, a socket per enabled target.
#include "dect2/net/feed_targets.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "tcp_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dect2 {

const std::vector<FeedPreset>& feedPresets() {
    static const std::vector<FeedPreset> p = {
        // ADS-B Exchange documents Beast out to feed.adsbexchange.com:30004 (readsb beast_reduce_out); its MLAT uses a separate client and is not implemented here
        {"ADS-B Exchange (Beast)", FeedKind::AdsbBeast, FeedProto::Tcp, "feed.adsbexchange.com", 30004, "Beast feed only. MLAT is not supported."},
        // fr24feed (the official FR24 program) accepts Beast on a local port; its own link to FR24 is private and is not reimplemented
        {"Flightradar24 via fr24feed (Beast)", FeedKind::AdsbBeast, FeedProto::Tcp, "127.0.0.1", 30334, "Send to a fr24feed you run yourself (receiver set to beast-tcp)."},
        // the FlightAware / FlightTracker upload protocols are private; their programs read Beast from a port
        {"FlightAware / FlightTracker client (Beast)", FeedKind::AdsbBeast, FeedProto::Tcp, "127.0.0.1", 30005, "Send to a client program you run. Edit host and port to the one it listens on."},
        {"AISHub (NMEA UDP)", FeedKind::AisNmea, FeedProto::Udp, "data.aishub.net", 0, "The port is given to you when you register your station."},
        {"AIS Dispatcher (NMEA UDP)", FeedKind::AisNmea, FeedProto::Udp, "", 0, "Enter the address and port from your AIS Dispatcher account."},
        {"VesselFinder (NMEA UDP)", FeedKind::AisNmea, FeedProto::Udp, "", 0, "Enter the address and port VesselFinder gave your station."},
        {"VesselTracker (NMEA UDP)", FeedKind::AisNmea, FeedProto::Udp, "", 0, "Enter the address and port VesselTracker gave you."},
        {"Custom (Beast TCP)", FeedKind::AdsbBeast, FeedProto::Tcp, "", 0, ""},
        {"Custom (SBS-1 TCP)", FeedKind::AdsbSbs, FeedProto::Tcp, "", 0, ""},
        {"Custom (AIS NMEA UDP)", FeedKind::AisNmea, FeedProto::Udp, "", 0, ""},
    };
    return p;
}

namespace {
constexpr size_t kMaxQueue = 8192, kMaxPending = 256 * 1024;
}

struct FeedTargets::Impl {
    struct Item { FeedKind kind; std::string data; };
    struct Live {
        sock_t s = kBadSock; sockaddr_in to{};
        std::string pending;
        double retryAt = 0, wait = 2;
    };
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> q;
    std::vector<FeedTarget> cfg;
    std::vector<FeedTargetStatus> pub;
    std::atomic<bool> anyAdsb{false}, anyAis{false};
    bool dirty = false, quit = false;
    std::thread th;
    // the thread's own
    std::vector<FeedTarget> live;
    std::vector<Live> L;
    std::vector<FeedTargetStatus> st;

    void closeOne(size_t i) {
        if (L[i].s != kBadSock) sockClose(L[i].s);
        L[i].s = kBadSock; L[i].pending.clear();
        st[i].connected = false;
    }
    static bool same(const FeedTarget& a, const FeedTarget& b) {
        return a.enabled && b.enabled && a.host == b.host && a.port == b.port && a.proto == b.proto && a.key == b.key;
    }
    // keep the connection of a target whose address did not change; close the rest
    void applyConfig(const std::vector<FeedTarget>& next) {
        std::vector<Live> nl(next.size());
        std::vector<FeedTargetStatus> ns(next.size());
        std::vector<bool> used(live.size(), false);
        for (size_t i = 0; i < next.size(); i++) {
            for (size_t j = 0; j < live.size(); j++) {
                if (used[j] || !same(live[j], next[i])) continue;
                nl[i] = L[j]; ns[i] = st[j]; used[j] = true; L[j].s = kBadSock;
                break;
            }
        }
        for (size_t j = 0; j < L.size(); j++) if (L[j].s != kBadSock) sockClose(L[j].s);
        L = std::move(nl); st = std::move(ns); live = next;
        for (size_t i = 0; i < live.size(); i++) if (!live[i].enabled) { st[i] = FeedTargetStatus(); st[i].state = "off"; }
    }

    void service(size_t i, double t) {
        const FeedTarget& tg = live[i];
        Live& l = L[i]; FeedTargetStatus& s = st[i];
        if (!tg.enabled) return;
        if (tg.host.empty() || tg.port <= 0 || tg.port > 65535) { s.state = "needs a host and a port"; return; }
        if (tg.proto == FeedProto::Udp) {
            if (l.s != kBadSock) { s.connected = true; s.state = "sending to " + tg.host + ":" + std::to_string(tg.port) + " (UDP)"; return; }
            if (t < l.retryAt) return;
            addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(tg.host.c_str(), std::to_string(tg.port).c_str(), &hints, &res) != 0 || !res) {
                s.lastError = "cannot find the address " + tg.host; s.state = "waiting to retry";
                l.retryAt = t + l.wait; l.wait = std::min(60.0, l.wait * 2);
                if (res) freeaddrinfo(res);
                return;
            }
            memcpy(&l.to, res->ai_addr, sizeof l.to);
            freeaddrinfo(res);
            l.s = socket(AF_INET, SOCK_DGRAM, 0);
            if (l.s == kBadSock) { s.lastError = "cannot open a socket"; l.retryAt = t + 10; return; }
            sockSetNonBlocking(l.s, true);
            s.lastError.clear(); l.wait = 2; s.connected = true;
            return;
        }
        if (l.s == kBadSock) {
            if (t < l.retryAt) { s.state = "waiting to reconnect"; return; }
            s.state = "connecting to " + tg.host;
            std::string err;
            sock_t c = tcpConnect(tg.host, tg.port, 3000, err);
            if (c == kBadSock) {
                s.lastError = err; s.state = "waiting to reconnect";
                l.retryAt = t + l.wait; l.wait = std::min(120.0, l.wait * 2);
                return;
            }
            sockSetNonBlocking(c, true);
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            l.s = c; l.wait = 2; s.connected = true; s.lastError.clear();
            if (!tg.key.empty()) l.pending = tg.key + "\n";
        }
        s.state = "connected to " + tg.host + ":" + std::to_string(tg.port);
    }
    void flush(size_t i, double t) {
        if (live[i].proto != FeedProto::Tcp) return;
        Live& l = L[i];
        if (l.s == kBadSock) return;
        bool dead = false;
        if (!l.pending.empty()) {
#if defined(MSG_NOSIGNAL)
            const int fl = MSG_NOSIGNAL;
#else
            const int fl = 0;
#endif
            const long long k = send(l.s, l.pending.data(), (int)l.pending.size(), fl);
            if (k > 0) l.pending.erase(0, (size_t)k);
            else if (k < 0) {
#ifdef _WIN32
                const int e = WSAGetLastError(); dead = !(e == WSAEWOULDBLOCK || e == WSAEINTR);
#else
                dead = !(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
            }
        }
        char b[512];   // what the server sends back is not used; a zero read means it closed
        for (int n = 0; n < 8 && !dead; n++) {
            const long long r = recv(l.s, b, sizeof b, 0);
            if (r == 0) dead = true;
            else if (r < 0) break;
        }
        if (dead) {
            closeOne(i); st[i].lastError = "the server closed the connection"; st[i].reconnects++;
            l.retryAt = t + l.wait; l.wait = std::min(120.0, l.wait * 2);
        }
    }
    void deliver(const Item& it) {
        for (size_t i = 0; i < live.size(); i++) {
            const FeedTarget& tg = live[i];
            if (!tg.enabled || tg.kind != it.kind) continue;
            Live& l = L[i]; FeedTargetStatus& s = st[i];
            if (l.s == kBadSock) { s.dropped++; continue; }
            if (tg.proto == FeedProto::Udp) {
                if (sockSendTo(l.s, it.data.data(), it.data.size(), l.to) < 0) s.dropped++; else s.sent++;
            } else {
                if (l.pending.size() + it.data.size() > kMaxPending) { s.dropped++; continue; }
                l.pending += it.data; s.sent++;
            }
        }
    }
    int waitMs = 40;   // 40 ms while data is queued for a socket, 250 ms otherwise (new items and config wake the thread at once)
    void loop() {
        for (;;) {
            std::deque<Item> batch;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return quit || !q.empty() || dirty; });
                if (quit) break;
                batch.swap(q);
                if (dirty) { applyConfig(cfg); dirty = false; }
            }
            const double t = steadyNowSec();
            for (size_t i = 0; i < live.size(); i++) service(i, t);
            for (const Item& it : batch) deliver(it);
            for (size_t i = 0; i < live.size(); i++) flush(i, t);
            {
                bool busy = false;
                for (const Live& l : L) if (!l.pending.empty()) { busy = true; break; }
                waitMs = busy ? 40 : 250;
            }
            std::lock_guard<std::mutex> lk(mu);
            pub = st;
        }
        for (size_t i = 0; i < L.size(); i++) closeOne(i);
        L.clear(); st.clear(); live.clear();
    }
};

FeedTargets& FeedTargets::get() { static FeedTargets* f = new FeedTargets(); return *f; }   // never destroyed: decoders may still report at exit
FeedTargets::FeedTargets() : impl_(new Impl) {}

void FeedTargets::configure(const std::vector<FeedTarget>& t) {
    netInit();
    Impl& I = *impl_;
    bool a = false, s = false;
    for (const FeedTarget& x : t) {
        if (!x.enabled) continue;
        if (x.kind == FeedKind::AisNmea) s = true; else a = true;
    }
    I.anyAdsb.store(a); I.anyAis.store(s);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.cfg = t; I.dirty = true; I.quit = false;
        I.pub.assign(t.size(), FeedTargetStatus());
        for (auto& p : I.pub) p.state = "starting";
    }
    if ((a || s) && !I.th.joinable()) I.th = namedThread("onair-feedtargets", [&I] { I.loop(); });
    I.cv.notify_one();
}

std::vector<FeedTarget> FeedTargets::targets() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->cfg; }

FeedTargetStatus FeedTargets::status(size_t i) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    FeedTargetStatus s;
    if (i >= impl_->cfg.size() || !impl_->cfg[i].enabled) { s.state = "off"; return s; }
    return i < impl_->pub.size() ? impl_->pub[i] : s;
}

bool FeedTargets::wantAdsb() const { return impl_->anyAdsb.load(std::memory_order_relaxed); }
bool FeedTargets::wantAis() const { return impl_->anyAis.load(std::memory_order_relaxed); }

void FeedTargets::adsbBeast(const std::string& f) {
    Impl& I = *impl_;
    { std::lock_guard<std::mutex> lk(I.mu); if (I.q.size() >= kMaxQueue) return; I.q.push_back({FeedKind::AdsbBeast, f}); }
    I.cv.notify_one();
}
void FeedTargets::adsbSbs(const std::string& f) {
    Impl& I = *impl_;
    { std::lock_guard<std::mutex> lk(I.mu); if (I.q.size() >= kMaxQueue) return; I.q.push_back({FeedKind::AdsbSbs, f}); }
    I.cv.notify_one();
}
void FeedTargets::ais(const std::vector<std::string>& v) {
    Impl& I = *impl_;
    { std::lock_guard<std::mutex> lk(I.mu); for (const std::string& s : v) { if (I.q.size() >= kMaxQueue) break; I.q.push_back({FeedKind::AisNmea, s + "\r\n"}); } }
    I.cv.notify_one();
}

void FeedTargets::stop() {
    Impl& I = *impl_;
    I.anyAdsb.store(false); I.anyAis.store(false);
    { std::lock_guard<std::mutex> lk(I.mu); I.quit = true; I.q.clear(); I.cfg.clear(); I.pub.clear(); }
    I.cv.notify_one();
    if (I.th.joinable()) I.th.join();
    I.dirty = false;
}

} // namespace dect2
