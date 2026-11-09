// Wireshark output: the hub (wire.h). One worker thread does all file and socket work, so a decoder only ever copies a packet into a queue.
// It runs only while at least one switch is on: with all off there is no thread, no open port and no file.
#include "dect2/wirecap/wire.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#endif

namespace fs = std::filesystem;

namespace dect2::wire {

namespace {
constexpr size_t kQueueMax = 20000, kQueueBytesMax = 64u << 20, kClientBufMax = 8u << 20;
constexpr int kMaxClients = 32;
constexpr int64_t kHelloTimeoutMs = 5000;

std::string u8s(const fs::path& p) { const auto s = p.u8string(); return std::string((const char*)s.data(), s.size()); }
int64_t nowUsec() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void setNonBlocking(sock_t s) {
#ifdef _WIN32
    u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
}
bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}
long long sendQuiet(sock_t s, const void* d, size_t n) {
#if defined(MSG_NOSIGNAL) && !defined(_WIN32)
    return send(s, d, n, MSG_NOSIGNAL);
#else
    return sockSend(s, d, n);
#endif
}

struct Packet { int chan; int64_t t; Bytes data; };

struct Client {
    sock_t s = kBadSock;
    int chan = -1;              // -1: the hello line has not come yet
    std::string hello;
    Bytes out;
    size_t sent = 0;
    int64_t since = 0;
    bool closing = false;       // send what is left (the refusal line), then close
};

struct FileState {
    std::ofstream f;
    std::string path, error;
    uint64_t written = 0;
    bool open = false, full = false;
};
} // namespace

struct Hub::Impl {
    Hub* self = nullptr;
    mutable std::mutex mu;
    std::condition_variable cv, idle;
    std::deque<Packet> q;
    size_t qBytes = 0;
    bool busy = false;          // the worker is holding packets taken off the queue
    Config cfg;
    uint64_t cfgGen = 1;
    std::thread th;
    bool running = false, stopReq = false;
    Stats st[kChanCount];
    int bound = 0;

    // the worker's own
    sock_t listener = kBadSock, udp = kBadSock;
    sockaddr_in udpTo{};
    bool udpOk = false;
    std::vector<Client> clients;
    FileState files[kChanCount];
    bool wasOn[kChanCount] = {};
    uint64_t seenGen = 0;
    Config wc;

    void closeFile(int c) { FileState& fl = files[c]; if (fl.open) { fl.f.close(); fl.open = false; } fl.path.clear(); fl.error.clear(); fl.written = 0; fl.full = false; }

    void openListener() {
        if (listener != kBadSock || wc.streamPort < 0) return;   // (port 0: any free port, for the tests)
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == kBadSock) return;
        int one = 1;
#ifndef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#else
        (void)one;
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);      // this computer only
        a.sin_port = htons((uint16_t)wc.streamPort);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0) { sockClose(s); return; }
        setNonBlocking(s);
        listener = s;
        socklen_t al = sizeof a;
        getsockname(s, (sockaddr*)&a, &al);
        std::lock_guard<std::mutex> lk(mu);
        bound = ntohs(a.sin_port);
    }
    void closeListener() {
        if (listener != kBadSock) { sockClose(listener); listener = kBadSock; }
        for (auto& c : clients) sockClose(c.s);
        clients.clear();
        std::lock_guard<std::mutex> lk(mu);
        bound = 0;
        for (auto& s : st) s.clients = 0;
    }
    void applyConfig() {
        closeListener();
        for (int i = 0; i < kChanCount; i++) closeFile(i);
        if (udp != kBadSock) { sockClose(udp); udp = kBadSock; }
        udpOk = false;
        if (wc.gsmtap && !wc.gsmtapHost.empty() && wc.gsmtapPort > 0 && wc.gsmtapPort < 65536) {
            udpTo = {};
            udpTo.sin_family = AF_INET;
            udpTo.sin_port = htons((uint16_t)wc.gsmtapPort);
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(wc.gsmtapHost.c_str(), nullptr, &hints, &res) == 0 && res) {
                udpTo.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
                udp = socket(AF_INET, SOCK_DGRAM, 0);
                if (udp != kBadSock) { setNonBlocking(udp); udpOk = true; }
                freeaddrinfo(res);
            }
        }
        if (wc.live) openListener();
    }

    void writeFile(int c, const Packet& pk, const Bytes& frame) {
        if (!wc.files || wc.dir.empty()) return;
        FileState& fl = files[c];
        if (fl.full || !fl.error.empty()) return;
        if (!fl.open) {
            std::error_code ec;
            const fs::path dir = fs::u8path(wc.dir);
            fs::create_directories(dir, ec);
            char stamp[32];
            const time_t t = (time_t)(pk.t / 1000000);
            tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
            fs::path p = dir / (std::string(kChans[c].id) + "-" + stamp + ".pcapng");
            for (int k = 1; k < 100 && fs::exists(p, ec); k++) p = dir / (std::string(kChans[c].id) + "-" + stamp + "-" + std::to_string(k) + ".pcapng");
            fl.f.open(p, std::ios::binary | std::ios::trunc);
            if (!fl.f) { std::lock_guard<std::mutex> lk(mu); st[c].error = "cannot create the file in " + wc.dir; fl.error = st[c].error; return; }
            const Bytes shb = pcapngSectionHeader("OnAir");
            const Bytes idb = pcapngInterface(kChans[c].linktype, std::string("onair_") + kChans[c].id, std::string("OnAir: ") + kChans[c].display);
            fl.f.write((const char*)shb.data(), (std::streamsize)shb.size());
            fl.f.write((const char*)idb.data(), (std::streamsize)idb.size());
            fl.open = true; fl.written = shb.size() + idb.size();
            fl.path = u8s(p);
            std::lock_guard<std::mutex> lk(mu);
            st[c].file = fl.path; st[c].error.clear();
        }
        const Bytes epb = pcapngPacket(0, pk.t, frame.data(), frame.size());
        if (fl.written + epb.size() > wc.maxFileBytes) {
            fl.full = true;
            std::lock_guard<std::mutex> lk(mu);
            st[c].error = "file size limit reached: the file stopped, the live capture goes on";
            return;
        }
        fl.f.write((const char*)epb.data(), (std::streamsize)epb.size());
        fl.written += epb.size();
        if (!fl.f) { std::lock_guard<std::mutex> lk(mu); st[c].error = "writing the file failed (disk full?)"; fl.error = st[c].error; }
    }

    void handle(const Packet& pk) {
        const int c = pk.chan;
        const ChanInfo& ci = kChans[c];
        if (ci.encap == Encap::Gsmtap && udpOk) sockSendTo(udp, pk.data.data(), pk.data.size(), udpTo);
        Bytes wrapped;
        if (ci.encap != Encap::Direct) wrapped = udpInIpv4(ci.udpPort, pk.data.data(), pk.data.size());
        const Bytes& frame = ci.encap != Encap::Direct ? wrapped : pk.data;
        writeFile(c, pk, frame);
        for (auto& cl : clients) {
            if (cl.chan != c || cl.closing) continue;
            if (cl.out.size() - cl.sent + frame.size() + 16 > kClientBufMax) { std::lock_guard<std::mutex> lk(mu); st[c].dropped++; continue; }
            appendPcapRecord(cl.out, pk.t, frame.data(), frame.size());
        }
    }

    void flushClients() {
        for (auto& cl : clients) {
            while (cl.sent < cl.out.size()) {
                const long long w = sendQuiet(cl.s, cl.out.data() + cl.sent, cl.out.size() - cl.sent);
                if (w > 0) { cl.sent += (size_t)w; continue; }
                if (w < 0 && wouldBlock()) break;
                cl.closing = true; cl.sent = cl.out.size();   // gone
                break;
            }
            if (cl.sent == cl.out.size()) { cl.out.clear(); cl.sent = 0; }
        }
    }

    void pollSockets(int waitMs) {
        fd_set rd; FD_ZERO(&rd);
        fd_set wr; FD_ZERO(&wr);
        int maxFd = -1;
        auto add = [&](fd_set& set, sock_t s) {
#ifndef _WIN32
            if (s >= FD_SETSIZE) return;
#endif
            FD_SET(s, &set);
            maxFd = std::max(maxFd, (int)s);
        };
        if (listener != kBadSock) add(rd, listener);
        for (auto& cl : clients) { add(rd, cl.s); if (cl.sent < cl.out.size()) add(wr, cl.s); }
        if (maxFd < 0) { std::unique_lock<std::mutex> lk(mu); if (q.empty() && !stopReq) cv.wait_for(lk, std::chrono::milliseconds(waitMs)); return; }
        timeval tv{0, waitMs * 1000};
        if (select(maxFd + 1, &rd, &wr, nullptr, &tv) <= 0) return;
        if (listener != kBadSock && FD_ISSET(listener, &rd)) {
            sock_t s = accept(listener, nullptr, nullptr);
            if (s != kBadSock) {
                if ((int)clients.size() >= kMaxClients) sockClose(s);
                else { setNonBlocking(s); Client c; c.s = s; c.since = steadyMs(); clients.push_back(std::move(c)); }
            }
        }
        for (auto& cl : clients) {
            if (cl.closing) continue;
#ifndef _WIN32
            if (cl.s >= FD_SETSIZE) continue;
#endif
            if (!FD_ISSET(cl.s, &rd)) continue;
            char buf[128];
            const long long r = sockRecv(cl.s, buf, sizeof buf);
            if (r == 0 || (r < 0 && !wouldBlock())) { cl.closing = true; cl.sent = cl.out.size(); continue; }
            if (r < 0 || cl.chan >= 0) continue;         // after the hello a client has nothing to say
            cl.hello.append(buf, (size_t)r);
            const size_t nl = cl.hello.find('\n');
            if (nl == std::string::npos) { if (cl.hello.size() > 80) refuse(cl); continue; }
            const std::string line = cl.hello.substr(0, nl);
            const std::string head = "ONAIR-WIRE 1 ";
            int c = line.compare(0, head.size(), head) == 0 ? chanById(line.c_str() + head.size()) : -1;
            if (c < 0 || !self->enabled((Chan)c)) { refuse(cl); continue; }
            cl.chan = c;
            const std::string ok = "OK " + std::to_string(kChans[c].linktype) + "\n";
            cl.out.insert(cl.out.end(), ok.begin(), ok.end());
        }
    }
    void refuse(Client& cl) {
        static const char off[] = "OFF\n";
        cl.out.insert(cl.out.end(), off, off + 4);
        cl.closing = true;
    }
    void reapClients() {
        const int64_t t = steadyMs();
        for (auto& cl : clients) {
            if (cl.chan < 0 && !cl.closing && t - cl.since > kHelloTimeoutMs) { cl.closing = true; cl.sent = cl.out.size(); }
            if (cl.chan >= 0 && !cl.closing && !self->enabled((Chan)cl.chan)) { cl.closing = true; cl.sent = cl.out.size(); }
        }
        int counts[kChanCount] = {};
        clients.erase(std::remove_if(clients.begin(), clients.end(), [&](Client& c) {
            if (c.closing && c.sent >= c.out.size()) { sockClose(c.s); return true; }
            return false;
        }), clients.end());
        for (auto& cl : clients) if (cl.chan >= 0) counts[cl.chan]++;
        std::lock_guard<std::mutex> lk(mu);
        for (int i = 0; i < kChanCount; i++) st[i].clients = counts[i];
    }

    void worker() {
        for (;;) {
            std::deque<Packet> batch;
            {
                std::unique_lock<std::mutex> lk(mu);
                if (stopReq) { q.clear(); qBytes = 0; break; }
                if (cfgGen != seenGen) { seenGen = cfgGen; wc = cfg; lk.unlock(); applyConfig(); lk.lock(); }
                batch.swap(q); qBytes = 0;
                busy = !batch.empty();
            }
            for (int i = 0; i < kChanCount; i++) {
                const bool now = self->enabled((Chan)i);
                if (wasOn[i] && !now) closeFile(i);
                wasOn[i] = now;
            }
            for (const Packet& pk : batch) handle(pk);
            for (auto& fl : files) if (fl.open) fl.f.flush();
            flushClients();
            reapClients();
            {
                std::lock_guard<std::mutex> lk(mu);
                busy = false;
                if (q.empty()) idle.notify_all();
            }
            pollSockets(batch.empty() ? 20 : 0);
        }
        closeListener();
        for (int i = 0; i < kChanCount; i++) closeFile(i);
        if (udp != kBadSock) { sockClose(udp); udp = kBadSock; }
        udpOk = false;
        std::lock_guard<std::mutex> lk(mu);
        idle.notify_all();
    }

    void startLocked() {
        if (running) return;
        netInit();
        stopReq = false; running = true; seenGen = 0;
        th = std::thread([this] { worker(); });
    }
    void stopWorker() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!running) return;
            stopReq = true;
            cv.notify_all();
        }
        th.join();
        std::lock_guard<std::mutex> lk(mu);
        running = false; stopReq = false;
    }
};

Hub& Hub::get() { static Hub h; return h; }

Hub::Hub() : p_(std::make_unique<Impl>()) {
    p_->self = this;
    for (auto& o : on_) o.store(false);
}
Hub::~Hub() { shutdown(); }

void Hub::configure(const Config& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cfg = c;
    p_->cfgGen++;
    p_->cv.notify_all();
}
Config Hub::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }

void Hub::setEnabled(Chan c, bool on) {
    on_[c].store(on, std::memory_order_relaxed);
    bool any = false;
    for (const auto& o : on_) any = any || o.load(std::memory_order_relaxed);
    if (any) { std::lock_guard<std::mutex> lk(p_->mu); p_->startLocked(); p_->cv.notify_all(); }
    else p_->stopWorker();
}

void Hub::publish(Chan c, const uint8_t* d, size_t n, int64_t t) {
    if (!enabled(c)) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->running) return;
    Stats& s = p_->st[c];
    if (p_->q.size() >= kQueueMax || p_->qBytes + n > kQueueBytesMax) { s.dropped++; return; }
    s.packets++; s.bytes += n;
    p_->q.push_back({(int)c, t ? t : nowUsec(), Bytes(d, d + n)});
    p_->qBytes += n;
    p_->cv.notify_all();
}

Stats Hub::stats(Chan c) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st[c]; }
int Hub::boundPort() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->bound; }

bool Hub::waitIdle(int ms) {
    std::unique_lock<std::mutex> lk(p_->mu);
    return p_->idle.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !p_->running || (p_->q.empty() && !p_->busy); });
}

void Hub::shutdown() {
    for (auto& o : on_) o.store(false, std::memory_order_relaxed);
    p_->stopWorker();
}

InstallResult installIntoWireshark(const std::string& program, const std::string& home) {
    InstallResult r;
    if (home.empty()) { r.error = "the home folder is not known"; return r; }
    const fs::path src = fs::u8path(program);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) { r.error = "the extcap program was not found next to OnAir: " + program; return r; }
#ifdef _WIN32
    const fs::path base = fs::u8path(home) / "Wireshark";
#else
    const fs::path base = fs::u8path(home) / ".local" / "lib" / "wireshark";
#endif
    const fs::path ext = base / "extcap", plug = base / "plugins";
    fs::create_directories(ext, ec);
    if (ec) { r.error = "cannot create " + u8s(ext) + ": " + ec.message(); return r; }
    fs::create_directories(plug, ec);
    if (ec) { r.error = "cannot create " + u8s(plug) + ": " + ec.message(); return r; }
    const fs::path dst = ext / src.filename();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { r.error = "cannot copy to " + u8s(dst) + ": " + ec.message(); return r; }
    fs::permissions(dst, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    const fs::path lua = plug / "onair.lua";
    std::ofstream f(lua, std::ios::binary | std::ios::trunc);
    f << luaDissectors();
    f.close();
    if (!f) { r.error = "cannot write " + u8s(lua); return r; }
    r.ok = true; r.extcapPath = u8s(dst); r.luaPath = u8s(lua);
    return r;
}

} // namespace dect2::wire
