// The data feeds' thread: TCP servers (SBS, Beast, KISS), UDP senders (AIS, ACARS, VDL2) and the APRS-IS client.
// The decoders only append to a bounded queue; everything here is non-blocking (the APRS-IS connect excepted, which only
// holds up this thread). A slow client gets its data dropped once it has 512 kB waiting.
#include "dect2/common/thread_name.h"
#include "dect2/net/feeds.h"
#include "dect2/common/timecompat.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/net/feed_targets.h"
#include "tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef _WIN32
#include <netdb.h>
#include "dect2/common/timecompat.h"
#endif

namespace dect2 {

namespace {
using Clock = std::chrono::steady_clock;
double nowSec() { return dect2::steadyNowSec(); }

bool wouldBlock() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
std::string lastSockError() {
#ifdef _WIN32
    return "error " + std::to_string(WSAGetLastError());
#else
    return strerror(errno);
#endif
}
void noSigPipe(sock_t s) {
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
    int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)s;
#endif
}
// bytes sent (0 when it would block), -1 when the connection is broken
long long sendSome(sock_t s, const std::string& d) {
#if defined(MSG_NOSIGNAL)
    const int fl = MSG_NOSIGNAL;
#else
    const int fl = 0;
#endif
#ifdef _WIN32
    const long long k = send(s, d.data(), (int)d.size(), fl);
#else
    const long long k = send(s, d.data(), d.size(), fl);
#endif
    if (k >= 0) return k;
    return wouldBlock() ? 0 : -1;
}
// 1 data read and thrown away, 0 nothing waiting, -1 closed or broken
int drain(sock_t s, std::string* keep = nullptr) {
    char b[2048];
    for (int i = 0; i < 16; i++) {
        const long long k = sockRecv(s, b, sizeof b);
        if (k == 0) return -1;
        if (k < 0) return wouldBlock() ? 0 : -1;
        if (keep) keep->append(b, (size_t)k);
    }
    return 1;
}

constexpr size_t kMaxQueue = 8192;          // messages waiting for the thread
constexpr size_t kMaxPending = 512 * 1024;  // bytes waiting for one TCP client
} // namespace

struct Feeds::Impl {
    struct Item { int id; std::string data; };
    struct Client { sock_t s = kBadSock; std::string pending; };
    struct Server { sock_t ls = kBadSock; int port = 0; bool lan = false; double retryAt = 0; std::vector<Client> clients; };
    struct Udp { sock_t s = kBadSock; sockaddr_in to{}; std::string host; int port = 0; };

    // shared with the decoders and the UI (under mu)
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> q;
    FeedConfig cfg;
    AprsGate gate;                // the APRS-IS iGate rules (its own lock)
    bool cfgDirty = false, quit = false;
    uint64_t qDropped[kNumFeeds] = {};
    FeedStatus pub[kNumFeeds];
    std::thread th;

    // the thread's own
    FeedConfig live;
    FeedStatus st[kNumFeeds];
    Server srv[kNumFeeds];
    Udp udp[kNumFeeds];
    sock_t is = kBadSock;
    std::string isPending, isRecv, isKey;
    double isRetryAt = 0;
    bool isVerified = false, isAnswered = false;

    static bool isServer(int id) { return id == kFeedSbs || id == kFeedBeast || id == kFeedKiss; }
    static bool isUdp(int id) {
        return id == kFeedAis || id == kFeedAcars || id == kFeedVdl2 || id == kFeedGnss || id == kFeedBle || id == kFeedLora;
    }
    void udpTarget(int id, std::string& host, int& port) const {
        host = "127.0.0.1"; port = 0;
        if (id == kFeedAis) { host = live.aisHost; port = live.aisPort; }
        else if (id == kFeedAcars) { host = live.acarsHost; port = live.acarsPort; }
        else if (id == kFeedVdl2) { host = live.vdl2Host; port = live.vdl2Port; }
        else if (id == kFeedGnss) { host = live.gnssHost; port = live.gnssPort; }
        else if (id == kFeedBle) { host = live.bleHost; port = live.blePort; }
        else if (id == kFeedLora) { host = live.loraHost; port = live.loraPort; }
    }
    int serverPort(int id) const { return id == kFeedSbs ? live.sbsPort : id == kFeedBeast ? live.beastPort : live.kissPort; }

    void closeServer(int id) {
        Server& s = srv[id];
        for (Client& c : s.clients) sockClose(c.s);
        s.clients.clear();
        if (s.ls != kBadSock) sockClose(s.ls);
        s.ls = kBadSock; s.port = 0; s.retryAt = 0;
    }
    void openServer(int id, double now) {
        Server& s = srv[id];
        const int port = serverPort(id);
        if (s.ls != kBadSock && s.port == port && s.lan == live.lan) return;
        if (s.ls != kBadSock) closeServer(id);
        if (now < s.retryAt) return;
        s.retryAt = now + 5;
        sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) { st[id].lastError = "cannot open a socket: " + lastSockError(); return; }
#ifndef _WIN32
        { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(live.lan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (port <= 0 || port > 65535 || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
            st[id].lastError = "cannot listen on TCP port " + std::to_string(port) + " (in use by another program?)";
            sockClose(ls);
            return;
        }
        sockSetNonBlocking(ls, true);
        s.ls = ls; s.port = port; s.lan = live.lan;
        st[id].lastError.clear();
    }
    void openUdp(int id) {
        Udp& u = udp[id];
        std::string host; int port = 0;
        udpTarget(id, host, port);
        if (u.s != kBadSock && u.host == host && u.port == port) return;
        closeUdp(id);
        u.host = host; u.port = port;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (port <= 0 || port > 65535 || getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            st[id].lastError = "cannot find the address " + host + ":" + std::to_string(port);
            if (res) freeaddrinfo(res);
            return;
        }
        memcpy(&u.to, res->ai_addr, sizeof u.to);
        freeaddrinfo(res);
        u.s = socket(AF_INET, SOCK_DGRAM, 0);
        if (u.s == kBadSock) { st[id].lastError = "cannot open a socket: " + lastSockError(); return; }
        sockSetNonBlocking(u.s, true);
        st[id].lastError.clear();
    }
    void closeUdp(int id) {
        if (udp[id].s != kBadSock) sockClose(udp[id].s);
        udp[id].s = kBadSock; udp[id].host.clear(); udp[id].port = 0;
    }
    void closeIs() {
        if (is != kBadSock) sockClose(is);
        is = kBadSock; isPending.clear(); isRecv.clear(); isVerified = isAnswered = false;
    }

    void apply(double now) {
        for (int id = 0; id < kNumFeeds; id++) {
            const bool on = live.on[id];
            if (isServer(id)) { if (on) openServer(id, now); else { closeServer(id); st[id].lastError.clear(); } }
            else if (isUdp(id)) { if (on) openUdp(id); else { closeUdp(id); st[id].lastError.clear(); } }
        }
        const std::string key = live.on[kFeedAprsIs] ? live.aprsCall + "\n" + live.aprsPass + "\n" + live.aprsServer + ":" + std::to_string(live.aprsServerPort) : "";
        if (key != isKey) { closeIs(); isKey = key; isRetryAt = 0; st[kFeedAprsIs].lastError.clear(); }
    }

    void serviceServers(double now) {
        for (int id = 0; id < kNumFeeds; id++) {
            if (!isServer(id) || !live.on[id]) continue;
            if (srv[id].ls == kBadSock) { openServer(id, now); if (srv[id].ls == kBadSock) continue; }
            Server& s = srv[id];
            for (int k = 0; k < 8; k++) {
                sockaddr_in a{};
                socklen_t al = sizeof a;
                sock_t c = accept(s.ls, (sockaddr*)&a, &al);
                if (c == kBadSock) break;
                sockSetNonBlocking(c, true);
                noSigPipe(c);
                int one = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
                s.clients.push_back({c, {}});
            }
            for (size_t i = 0; i < s.clients.size();) {
                Client& c = s.clients[i];
                bool dead = drain(c.s) < 0;   // what clients send (Beast settings, KISS frames to transmit) is not used
                if (!dead && !c.pending.empty()) {
                    const long long k = sendSome(c.s, c.pending);
                    if (k < 0) dead = true; else c.pending.erase(0, (size_t)k);
                }
                if (dead) { sockClose(c.s); s.clients.erase(s.clients.begin() + (long)i); }
                else i++;
            }
        }
    }

    void serverSend(int id, const std::string& d) {
        Server& s = srv[id];
        if (s.ls == kBadSock || s.clients.empty()) { st[id].dropped++; return; }
        bool any = false;
        for (Client& c : s.clients) {
            if (c.pending.size() + d.size() > kMaxPending) { st[id].dropped++; continue; }   // too slow: this message is lost for it
            c.pending += d;
            any = true;
        }
        if (any) st[id].sent++;
    }

    void serviceIs(double now) {
        FeedStatus& S = st[kFeedAprsIs];
        if (!live.on[kFeedAprsIs]) { S.state = "off"; return; }
        if (live.aprsCall.empty()) { S.state = "needs a callsign"; return; }
        if (!aprsValidCallsign(live.aprsCall)) { S.state = "needs a valid callsign (like LY1ABC or LY1ABC-10)"; return; }
        if (is == kBadSock) {
            if (now < isRetryAt) { S.state = "waiting to connect again"; return; }
            std::string err;
            S.state = "connecting to " + live.aprsServer;
            sock_t s = tcpConnect(live.aprsServer, live.aprsServerPort, 3000, err);
            if (s == kBadSock) { S.lastError = err; isRetryAt = now + 30; S.state = "waiting to connect again"; return; }
            sockSetNonBlocking(s, true);
            noSigPipe(s);
            is = s;
            isPending = feedfmt::aprsIsLogin(live.aprsCall, live.aprsPass.empty() ? "-1" : live.aprsPass);
            S.lastError.clear();
        }
        const int r = drain(is, &isRecv);
        size_t nl;
        while ((nl = isRecv.find('\n')) != std::string::npos) {
            const std::string line = isRecv.substr(0, nl);
            isRecv.erase(0, nl + 1);
            if (line.rfind("# logresp", 0) == 0) {
                isAnswered = true;
                isVerified = line.find(" unverified") == std::string::npos && line.find(" verified") != std::string::npos;
                if (!isVerified) S.lastError = "the server did not accept the passcode (unverified): packets are not gated";
            }
        }
        if (isRecv.size() > 8192) isRecv.clear();
        bool dead = r < 0;
        if (!dead && !isPending.empty()) {
            const long long k = sendSome(is, isPending);
            if (k < 0) dead = true; else isPending.erase(0, (size_t)k);
        }
        if (dead) { closeIs(); S.lastError = "the server closed the connection"; isRetryAt = now + 10; S.state = "waiting to connect again"; return; }
        S.state = !isAnswered ? "logging in to " + live.aprsServer : isVerified ? "connected to " + live.aprsServer + ", verified" : "connected, not verified";
    }

    void deliver(const Item& it) {
        const int id = it.id;
        if (!live.on[id]) return;
        if (isServer(id)) serverSend(id, it.data);
        else if (isUdp(id)) {
            Udp& u = udp[id];
            if (u.s == kBadSock) { st[id].dropped++; return; }
            if (sockSendTo(u.s, it.data.data(), it.data.size(), u.to) < 0) {
                st[id].dropped++;
                if (!wouldBlock()) st[id].lastError = "send failed: " + lastSockError();
            } else st[id].sent++;
        } else if (id == kFeedAprsIs) {
            if (is == kBadSock || !isAnswered || !isVerified || isPending.size() > 65536) { st[id].dropped++; return; }
            isPending += it.data;
            st[id].sent++;
        }
    }

    int waitMs = 40;   // 40 ms while a client is connected or data is waiting, 250 ms when only listening (queued items and config wake the thread at once)
    void loop() {
        for (;;) {
            std::deque<Item> batch;
            bool dirty = false;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return quit || !q.empty() || cfgDirty; });
                if (quit) break;
                batch.swap(q);
                if (cfgDirty) { live = cfg; cfgDirty = false; dirty = true; }
            }
            const double now = nowSec();
            if (dirty) apply(now);
            serviceServers(now);
            serviceIs(now);
            for (const Item& it : batch) deliver(it);
            if (!batch.empty()) serviceServers(now);   // send what just arrived
            if (!batch.empty() && is != kBadSock && !isPending.empty()) {
                const long long k = sendSome(is, isPending);
                if (k > 0) isPending.erase(0, (size_t)k);
            }
            for (int id = 0; id < kNumFeeds; id++) {
                FeedStatus& S = st[id];
                if (isServer(id)) {
                    S.active = srv[id].ls != kBadSock;
                    S.clients = (int)srv[id].clients.size();
                    S.state = !live.on[id] ? "off" : S.active ? "listening on TCP port " + std::to_string(srv[id].port) : "not listening";
                } else if (isUdp(id)) {
                    S.active = udp[id].s != kBadSock;
                    S.state = !live.on[id] ? "off" : S.active ? "sending to " + udp[id].host + ":" + std::to_string(udp[id].port) + " (UDP)" : "not sending";
                } else {
                    S.active = is != kBadSock && isVerified;
                    S.clients = is != kBadSock ? 1 : 0;
                }
            }
            {
                bool busy = !isPending.empty() || (is != kBadSock && !isVerified);
                for (int id = 0; id < kNumFeeds && !busy; id++) if (isServer(id) && !srv[id].clients.empty()) busy = true;
                waitMs = busy ? 40 : 250;
            }
            std::lock_guard<std::mutex> lk(mu);
            for (int id = 0; id < kNumFeeds; id++) pub[id] = st[id];
        }
        for (int id = 0; id < kNumFeeds; id++) { closeServer(id); closeUdp(id); }
        closeIs();
        isKey.clear();
    }
};

Feeds& Feeds::get() {
    static Feeds* f = new Feeds();   // never destroyed: decoders may still report while the program exits
    return *f;
}

Feeds::Feeds() : impl_(new Impl) {
    for (auto& w : want_) w.store(false);
}

Feeds::~Feeds() { stop(); }

void Feeds::configure(const FeedConfig& c) {
    netInit();
    FeedTargets::get().configure(c.targets);
    Impl& I = *impl_;
    I.gate.configure(c.aprsGate);
    bool any = false;
    for (int id = 0; id < kNumFeeds; id++) {
        const bool on = c.on[id] && (id != kFeedAprsIs || aprsValidCallsign(c.aprsCall));
        want_[id].store(on, std::memory_order_relaxed);
        any = any || c.on[id];
    }
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.cfg = c;
        I.cfgDirty = true;
        I.quit = false;
    }
    if (any && !I.th.joinable()) I.th = dect2::namedThread("onair-feeds", [&I] { I.loop(); });
    I.cv.notify_one();
}

FeedConfig Feeds::config() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cfg;
}

FeedStatus Feeds::status(int id) const {
    if (id < 0 || id >= kNumFeeds) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    FeedStatus s = impl_->pub[id];
    if (!impl_->cfg.on[id]) { s = FeedStatus(); s.state = "off"; }
    s.dropped += impl_->qDropped[id];
    if (id == kFeedAprsIs) s.detail = impl_->gate.summary();
    return s;
}

void Feeds::stop() {
    FeedTargets::get().stop();
    Impl& I = *impl_;
    for (auto& w : want_) w.store(false);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.quit = true;
        I.q.clear();
        for (int id = 0; id < kNumFeeds; id++) { I.cfg.on[id] = false; I.pub[id] = FeedStatus(); I.st[id] = FeedStatus(); I.qDropped[id] = 0; }
    }
    I.cv.notify_one();
    if (I.th.joinable()) I.th.join();
    I.live = FeedConfig();
    I.cfgDirty = false;
}

void Feeds::push(int id, std::string&& payload) {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (I.q.size() >= kMaxQueue) { I.qDropped[id]++; return; }
        I.q.push_back({id, std::move(payload)});
    }
    I.cv.notify_one();
}

void Feeds::adsb(const FeedAdsbFrame& f) {
    FeedTargets& T = FeedTargets::get();
    if (T.wantAdsb()) {
        const uint64_t ts = (uint64_t)std::llround(std::max(0.0, f.timeSec) * 12e6) & 0xFFFFFFFFFFFFull;
        T.adsbBeast(feedfmt::beastFrame(f.bytes, f.bits, ts, feedfmt::beastSignal(f.levelDbfs)));
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::string l = feedfmt::sbsLine(f, ms);
        if (!l.empty()) T.adsbSbs(l);
    }
    if (want_[kFeedBeast].load(std::memory_order_relaxed)) {
        const uint64_t ts = (uint64_t)std::llround(std::max(0.0, f.timeSec) * 12e6) & 0xFFFFFFFFFFFFull;
        push(kFeedBeast, feedfmt::beastFrame(f.bytes, f.bits, ts, feedfmt::beastSignal(f.levelDbfs)));
    }
    if (want_[kFeedSbs].load(std::memory_order_relaxed)) {
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::string l = feedfmt::sbsLine(f, ms);
        if (!l.empty()) push(kFeedSbs, std::move(l));
    }
}

void Feeds::ais(const std::vector<std::string>& sentences) {
    if (!wantAis()) return;
    FeedTargets::get().ais(sentences);
    if (!want_[kFeedAis].load(std::memory_order_relaxed)) return;
    for (const std::string& s : sentences) push(kFeedAis, s + "\r\n");
}

void Feeds::aprs(const std::vector<uint8_t>& ax25, const std::string& from, const std::string& to, const std::string& path, const std::string& info, bool uiF0) {
    if (want_[kFeedKiss].load(std::memory_order_relaxed) && !ax25.empty()) push(kFeedKiss, feedfmt::kissFrame(ax25.data(), ax25.size()));
    if (want_[kFeedAprsIs].load(std::memory_order_relaxed)) {
        std::string call;
        { std::lock_guard<std::mutex> lk(impl_->mu); call = impl_->cfg.aprsCall; }
        // the iGate rules, duplicate window and rate limits (aprs_gate.h)
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::string l;
        if (impl_->gate.decide(from, to, path, info, call, uiF0, now, &l) == AprsGateVerdict::Gate && !l.empty()) push(kFeedAprsIs, std::move(l));
    }
}

void Feeds::kiss(const uint8_t* d, size_t n) {
    if (wantKiss() && n) push(kFeedKiss, feedfmt::kissFrame(d, n));
}

void Feeds::acars(const AcarsMessage& m) {
    if (!wantAcars()) return;
    std::string st;
    { std::lock_guard<std::mutex> lk(impl_->mu); st = impl_->cfg.stationId; }
    push(kFeedAcars, feedfmt::acarsJson(m, st) + "\n");
}

void Feeds::vdl2(const Vdl2Message& m) {
    if (!wantVdl2()) return;
    std::string st;
    { std::lock_guard<std::mutex> lk(impl_->mu); st = impl_->cfg.stationId; }
    push(kFeedVdl2, feedfmt::vdl2Json(m, st) + "\n");
}

void Feeds::gnss(const GnssFix& f) {
    if (!wantGnss() || !f.valid) return;
    std::string rmc = feedfmt::gnssRmc(f), gga = feedfmt::gnssGga(f);
    if (!rmc.empty()) push(kFeedGnss, std::move(rmc));
    if (!gga.empty()) push(kFeedGnss, std::move(gga));
}

void Feeds::ble(const std::string& mac, const std::string& name, float rssi) {
    if (!wantBle()) return;
    push(kFeedBle, feedfmt::bleJson(mac, name, rssi) + "\n");
}

void Feeds::lora(const char* kind, const std::string& hex, float snrDb, double freqHz, int sf, const std::string& extra) {
    if (!wantLora()) return;
    push(kFeedLora, feedfmt::loraJson(kind ? kind : "lora", hex, snrDb, freqHz, sf, extra) + "\n");
}

} // namespace dect2
