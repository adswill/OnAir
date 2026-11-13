// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/tv/tsout.h"

#include "dect2/common/platform.h"
#include "../net/netcompat.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "dect2/common/diag.h"
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

using Clock = std::chrono::steady_clock;

namespace {
bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
void setNonBlocking(sock_t s) {
#ifdef _WIN32
    u_long m = 1;
    ioctlsocket(s, FIONBIO, &m);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
// The recording file: the path is UTF-8 (from the file dialog); on Windows fopen() would read it in the ANSI code page.
FILE* openForWriting(const std::string& path) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"wb");
#else
    return fopen(path.c_str(), "wb");
#endif
}
}

struct OutputManager::Impl {
    mutable std::mutex mu;
    OutputConfig cfg;
    FILE* file = nullptr;
    sock_t sock = kBadSock;
    sockaddr_in dst{};
    OutputStats st;
    // service filter
    ServiceFilter filter;
    // udp pacing
    struct Dgram { std::vector<uint8_t> data; Clock::time_point due; bool toFile = false; double step = 0; };
    std::deque<Dgram> queue;
    std::deque<Dgram> fileQ;         // the recording, released on the same clock as the UDP datagrams
    std::vector<uint8_t> fileHold;   // packets not yet given a send time
    std::vector<uint8_t> building;   // datagram under construction (payload packets)
    std::vector<std::vector<uint8_t>> burst; // datagrams of the current frame
    double pendingSecs = 0;                  // signal time handed over since the last scheduled burst
    std::condition_variable cv;
    std::thread sender;
    bool running = false;
    // HTTP viewers
    sock_t hsock = kBadSock;
    std::thread hthread;
    std::atomic<bool> hrun{false};
    std::mutex hmu;
    struct Viewer { sock_t s; int blocked = 0; };
    std::vector<Viewer> viewers;
    std::atomic<int> httpClients{0};   // written under hmu, read by stats() under mu
    uint16_t rtpSeq = 0;
    uint32_t rtpTs = 0;
    Clock::time_point lastDue = Clock::now();

    // mu held. Whatever is still buffered for the recording goes out now (the file is about to be closed).
    void flushFileLocked() {
        if (!file) { fileHold.clear(); fileQ.clear(); return; }
        auto wr = [&](const uint8_t* p, size_t n) {
            if (!n) return;
            fwrite(p, 1, n, file);
            st.fileBytes += n;
            st.filePackets += n / 188;
        };
        wr(fileHold.data(), fileHold.size());
        fileHold.clear();
        for (const Dgram& d : fileQ) wr(d.data.data(), d.data.size());
        fileQ.clear();
        fflush(file);
    }
    // mu held (called from the sender). Raw packets to every viewer; a viewer whose buffer is full misses the chunk, one that stays full for 3 s is dropped.
    void sendHttp(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> hl(hmu);
        for (size_t i = 0; i < viewers.size();) {
            #ifdef MSG_NOSIGNAL
            const long long n = send(viewers[i].s, data.data(), data.size(), MSG_NOSIGNAL);
#else
            const long long n = sockSend(viewers[i].s, data.data(), data.size());
#endif
            if (n == (long long)data.size()) { viewers[i].blocked = 0; st.httpBytes += (uint64_t)n; i++; continue; }
            if (n < 0 && wouldBlock() && ++viewers[i].blocked < 150) { st.httpDropped++; i++; continue; }
            if (n > 0) { viewers[i].blocked = 1000; }   // a partial packet: the stream for this viewer is broken, drop it
            sockClose(viewers[i].s);
            viewers.erase(viewers.begin() + (std::ptrdiff_t)i);
        }
        httpClients.store((int)viewers.size());
    }
    void stopHttp() {
        if (hrun.exchange(false) && hthread.joinable()) hthread.join();
        if (hsock != kBadSock) { sockClose(hsock); hsock = kBadSock; }
        std::lock_guard<std::mutex> hl(hmu);
        for (auto& v : viewers) sockClose(v.s);
        viewers.clear();
        st.httpOpen = false; httpClients.store(0); st.httpPortInUse = 0;
    }
    // mu held
    void startHttp(const OutputConfig& c) {
        netInit();
        sock_t l = socket(AF_INET, SOCK_STREAM, 0);
        if (l == kBadSock) { st.error = "HTTP server: socket failed"; DIAG_ERROR(447978, "The built-in HTTP stream server could not create its listening socket, so the stream is not served."); return; }
        int one = 1;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(c.httpLan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)c.httpPort);
        if (bind(l, (sockaddr*)&a, sizeof a) != 0 || ::listen(l, 8) != 0) { st.error = "HTTP server: cannot listen on port " + std::to_string(c.httpPort); DIAG_ERROR(100458, "The built-in HTTP stream server could not listen on port %d (it may be in use), so the stream is not served.", c.httpPort); sockClose(l); return; }
        socklen_t al = sizeof a;
        getsockname(l, (sockaddr*)&a, &al);
        hsock = l;
        st.httpOpen = true; st.httpPortInUse = ntohs(a.sin_port); st.httpDropped = st.httpBytes = 0;
        hrun = true;
        const std::string key = c.httpKey;
        hthread = std::thread([this, l, key] {
            while (hrun) {
                fd_set rd; FD_ZERO(&rd); FD_SET(l, &rd);
                timeval tv{0, 100000};
                if (select((int)l + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                sock_t c = accept(l, nullptr, nullptr);
                if (c == kBadSock) continue;
                // the request line and headers (at most 2 s, 4 KB)
                std::string req;
                const auto t0 = Clock::now();
                char b[512];
                while (req.find("\r\n\r\n") == std::string::npos && req.size() < 4096 && Clock::now() - t0 < std::chrono::seconds(2)) {
                    fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
                    timeval t2{0, 100000};
                    if (select((int)c + 1, &r2, nullptr, nullptr, &t2) <= 0) continue;
                    const long long n = sockRecv(c, b, sizeof b);
                    if (n <= 0) break;
                    req.append(b, (size_t)n);
                }
                const bool complete = req.find("\r\n\r\n") != std::string::npos && req.compare(0, 4, "GET ") == 0;
                const bool keyOk = key.empty() || req.substr(0, req.find("\r\n")).find("key=" + key) != std::string::npos;
                const char* bad = !complete ? "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
                                : !keyOk ? "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" : nullptr;
                if (bad) { sockSend(c, bad, strlen(bad)); sockClose(c); continue; }
                const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-cache\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
                sockSend(c, ok, strlen(ok));
#ifdef __APPLE__
                { int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
                setNonBlocking(c);
                std::lock_guard<std::mutex> hl(hmu);
                if (viewers.size() < 16) { viewers.push_back({c, 0}); httpClients.store((int)viewers.size()); }
                else sockClose(c);
            }
        });
    }
    void startSender() {
        running = true;
        sender = dect2::namedThread("onair-tsout", [this] {
            // A condition_variable timed wait on a normal thread is coalesced by the OS to about 100 ms, so every datagram that
            // came due in that window left in one lump and the player stalled until the next lump. This thread asks for a tight
            // timer and sleeps until the datagram's own time.
            setThreadPriority(ThreadPriority::Realtime);
            std::unique_lock<std::mutex> lk(mu);
            while (running) {
                if (queue.empty() && fileQ.empty()) { cv.wait(lk, [&] { return !running || !queue.empty() || !fileQ.empty(); }); continue; }
                const bool fileFirst = queue.empty() || (!fileQ.empty() && fileQ.front().due < queue.front().due);
                const Clock::time_point due = (fileFirst ? fileQ : queue).front().due;
                if (Clock::now() < due) {
                    // long waits (a stopped player, a far due time) wake often enough that close() is not stuck behind them
                    const auto slice = std::min(due, Clock::now() + std::chrono::milliseconds(20));
                    lk.unlock();
                    std::this_thread::sleep_until(slice);
                    lk.lock();
                    continue;
                }
                Dgram d = std::move((fileFirst ? fileQ : queue).front());
                (fileFirst ? fileQ : queue).pop_front();
                if (d.toFile) {
                    if (file) {
                        fwrite(d.data.data(), 1, d.data.size(), file);
                        st.fileBytes += d.data.size();
                        st.filePackets += d.data.size() / 188;
                        fflush(file);   // a player tailing the file sees the packets as their time arrives, not a lump at the end of the burst
                    }
                } else {
                    sendHttp(d.data);
                    if (sock == kBadSock) continue;
                    if (cfg.rtp) {   // the RTP header is put on at send time (the HTTP viewers get the bare packets)
                        std::vector<uint8_t> h(12);
                        h[0] = 0x80; h[1] = 33;
                        h[2] = (uint8_t)(rtpSeq >> 8); h[3] = (uint8_t)(rtpSeq & 0xFF); rtpSeq++;
                        rtpTs += (uint32_t)(d.step * 90000);
                        h[4] = (uint8_t)(rtpTs >> 24); h[5] = (uint8_t)(rtpTs >> 16); h[6] = (uint8_t)(rtpTs >> 8); h[7] = (uint8_t)rtpTs;
                        h[8] = 0x44; h[9] = 0x54; h[10] = 0x32; h[11] = 0x00;
                        d.data.insert(d.data.begin(), h.begin(), h.end());
                    }
                    long long n = sockSendTo(sock, d.data.data(), d.data.size(), dst);
                    if (n > 0) { st.udpDatagrams++; st.udpBytes += (uint64_t)n; }
                    else st.udpSendErrors++;   // the system refused it (a full send buffer): it never reached the network
                }
                if (!queue.empty()) st.udpQueueMs = std::chrono::duration<double, std::milli>(queue.back().due - Clock::now()).count();
                else st.udpQueueMs = 0;
            }
        });
    }
    void stopSender() {
        { std::lock_guard<std::mutex> lk(mu); running = false; }
        cv.notify_all();
        if (sender.joinable()) sender.join();
    }
};

bool parseStreamUrl(const std::string& urlIn, OutputConfig& c, std::string& err) {
    std::string u = urlIn;
    while (!u.empty() && isspace((unsigned char)u.front())) u.erase(u.begin());
    while (!u.empty() && isspace((unsigned char)u.back())) u.pop_back();
    std::string scheme = "udp";
    const size_t sp = u.find("://");
    if (sp != std::string::npos) { scheme = u.substr(0, sp); u = u.substr(sp + 3); }
    for (auto& ch : scheme) ch = (char)tolower((unsigned char)ch);
    if (scheme != "udp" && scheme != "rtp" && scheme != "http") { err = "use udp://host:port, rtp://host:port or http://:port"; return false; }
    const size_t sl = u.find('/');
    if (sl != std::string::npos) u.resize(sl);
    if (!u.empty() && u[0] == '@') u.erase(0, 1);
    const size_t co = u.rfind(':');
    if (co == std::string::npos) { err = "the address needs a port, like 127.0.0.1:1234"; return false; }
    std::string host = u.substr(0, co);
    const std::string ps = u.substr(co + 1);
    char* end = nullptr;
    const long port = strtol(ps.c_str(), &end, 10);
    if (ps.empty() || *end || port < 1 || port > 65535) { err = "the port must be a number from 1 to 65535"; return false; }
    if (scheme == "http") {
        c.http = true; c.httpPort = (int)port;
        c.httpLan = !(host.empty() || host == "127.0.0.1" || host == "localhost");
        c.udp = false;
    } else {
        if (host.empty()) host = "127.0.0.1";
        in_addr t{};
        if (inet_pton(AF_INET, host.c_str(), &t) != 1) { err = "the address must be a dotted IPv4 address"; return false; }
        c.udp = true; c.rtp = scheme == "rtp"; c.host = host; c.port = (int)port;
        c.http = false;
    }
    return true;
}

OutputManager::OutputManager() : p_(new Impl) { p_->startSender(); }
OutputManager::~OutputManager() { close(); p_->stopSender(); delete p_; }

OutputConfig OutputManager::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }

void OutputManager::close() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->flushFileLocked();
    if (p_->file) { fclose(p_->file); p_->file = nullptr; }
    if (p_->sock != kBadSock) { sockClose(p_->sock); p_->sock = kBadSock; }
    p_->stopHttp();
    p_->queue.clear();
    p_->building.clear();
    p_->burst.clear();
    p_->pendingSecs = 0;
    p_->st.fileOpen = p_->st.udpOpen = false;
}

void OutputManager::configure(const OutputConfig& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    OutputConfig old = I.cfg;
    I.cfg = c;
    I.st.error.clear();
    if (I.file && (!c.file || c.path != old.path)) { I.flushFileLocked(); fclose(I.file); I.file = nullptr; I.st.fileOpen = false; }
    if (c.file && !I.file && c.path.empty()) I.st.error = "choose a file to record to first";
    if (c.file && !I.file && !c.path.empty()) {
        I.file = openForWriting(c.path);
        if (!I.file) { DIAG_ERROR(501834, "The transport stream output file %s could not be created (%s), so nothing is written to it.", c.path.c_str(), strerror(errno)); I.st.error = "cannot write " + c.path + ": " + strerror(errno); }
        else { I.st.fileOpen = true; I.st.fileBytes = I.st.filePackets = 0; }
    }
    if (I.sock != kBadSock && (!c.udp || c.host != old.host || c.port != old.port || c.iface != old.iface)) { sockClose(I.sock); I.sock = kBadSock; I.st.udpOpen = false; I.queue.clear(); }
    if (c.udp && I.sock == kBadSock) {
        netInit();
        I.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (I.sock == kBadSock) { I.st.error = "socket failed"; DIAG_ERROR(587873, "The UDP output socket could not be created, so the transport stream is not sent."); }
        else {
            memset(&I.dst, 0, sizeof I.dst);
            I.dst.sin_family = AF_INET;
            I.dst.sin_port = htons((uint16_t)c.port);
            if (inet_pton(AF_INET, c.host.c_str(), &I.dst.sin_addr) != 1) { I.st.error = "bad UDP address " + c.host; DIAG_ERROR(295862, "The UDP output address %s is not a valid IPv4 address, so the transport stream is not sent.", c.host.c_str()); sockClose(I.sock); I.sock = kBadSock; }
            else {
                int ttl = c.ttl;
                setsockopt(I.sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
                in_addr ifa{};
                if (!c.iface.empty() && inet_pton(AF_INET, c.iface.c_str(), &ifa) == 1) setsockopt(I.sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof ifa);
                // the default send buffer (64 KB on Windows) holds only a few milliseconds of a 30 Mbit/s multiplex
                int sndbuf = 4 << 20;
                setsockopt(I.sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof sndbuf);
                I.st.udpOpen = true;
                I.st.udpDatagrams = I.st.udpBytes = I.st.udpDropped = I.st.udpSendErrors = I.st.udpCatchUps = 0;
            }
        }
    }
    if (I.hsock != kBadSock && (!c.http || c.httpPort != old.httpPort || c.httpLan != old.httpLan || c.httpKey != old.httpKey)) I.stopHttp();
    if (c.http && I.hsock == kBadSock) I.startHttp(c);
    if (c.serviceId != old.serviceId) I.filter.select(c.serviceId);
}

void OutputManager::packet(const uint8_t* pkt, const TsSnapshot* snap) { packets(pkt, 1, snap); }

void OutputManager::packets(const uint8_t* data, size_t n, const TsSnapshot* snap) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    if (!I.file && I.sock == kBadSock && I.hsock == kBadSock) return;
    for (size_t i = 0; i < n; i++) {
    const uint8_t* pkt = data + i * 188;
    const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    uint8_t rewritten[188];
    const uint8_t* out = pkt;
    if (I.cfg.dropNull && pid == 0x1FFF) continue;
    if (I.cfg.serviceId >= 0) {
        if (I.filter.selected() != I.cfg.serviceId) I.filter.select(I.cfg.serviceId);
        if (!I.filter.process(pkt, snap, rewritten)) continue;
        out = rewritten;
    }
    if (I.file) I.fileHold.insert(I.fileHold.end(), out, out + 188);   // written later, at the packet's own time (see burstDone)
    if (I.sock != kBadSock || I.hsock != kBadSock) {
        I.building.insert(I.building.end(), out, out + 188);
        if (I.building.size() >= 7 * 188) {
            I.burst.push_back(std::move(I.building));
            I.building.clear();
        }
    }
    }
}

void OutputManager::burstDone(double frameSeconds) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    const bool udp = I.sock != kBadSock || I.hsock != kBadSock;   // "udp" here: a paced network output of either kind
    if (!udp && !I.file) { I.burst.clear(); I.building.clear(); I.fileHold.clear(); I.pendingSecs = 0; return; }
    if (!udp) { I.burst.clear(); I.building.clear(); }
    // DVB-T, ISDB-T, DTMB and ATSC hand over their packets after every symbol or segment (about a millisecond of signal), DVB-T2 once a
    // frame. Collect at least kMinSpan of signal before scheduling: each call used to count as 20 ms or more and was padded with null
    // packets, so the schedule ran ahead of the clock many times over and the queue was thrown away every few seconds (VLC stuttered
    // on DVB-T while the app played cleanly).
    constexpr double kMinSpan = 0.05;
    I.pendingSecs += std::max(0.0, frameSeconds);
    if (I.pendingSecs < kMinSpan) return;
    frameSeconds = I.pendingSecs;
    I.pendingSecs = 0;
    if (!I.building.empty()) { // pad the tail datagram with nulls so nothing waits for the next frame
        while (I.building.size() < 7 * 188) { uint8_t nul[188]; memset(nul, 0xFF, 188); nul[0] = 0x47; nul[1] = 0x1F; nul[2] = 0xFF; nul[3] = 0x10; I.building.insert(I.building.end(), nul, nul + 188); }
        I.burst.push_back(std::move(I.building));
        I.building.clear();
    }
    if (I.burst.empty() && I.fileHold.empty()) return;
    const double span = frameSeconds;
    const auto now = Clock::now();
    auto base = std::max(now, I.lastDue);
    // Frames do not come out of the receiver evenly: the error correction finishes several at once now and then (a busy CPU, the GPU
    // working in batches), and each one is spread over a frame's time, so the queue runs ahead of the clock for a moment. Throwing the
    // queue away then lost whole frames every few seconds (a viewer saw VLC stutter while the app played cleanly). Send the waiting
    // datagrams sooner instead: the player's own buffer (VLC keeps a second) takes the burst. Only a queue that is truly stuck (more than
    // kMaxBacklog seconds, say a dead network) is dropped.
    constexpr double kMaxBacklog = 5.0;
    const double ahead = I.queue.empty() ? 0 : std::chrono::duration<double>(I.queue.back().due - now).count();
    auto dueIn = [](double sec) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sec)); };
    if (ahead > kMaxBacklog) {
        I.st.udpDropped += I.queue.size();
        I.queue.clear();
        base = now;
        for (auto& d : I.fileQ) d.due = now;   // the recording does not wait on a dead network, and it stays in order
    } else if (ahead > 3.0 * span + 0.2 && !I.queue.empty()) {
        const double over = std::min(span, 0.25);   // what is waiting goes out over this time
        const double gap = over / (double)I.queue.size();
        for (size_t i = 0; i < I.queue.size(); i++) I.queue[i].due = now + dueIn(gap * (double)i);
        if (!I.fileQ.empty()) {
            const double fg = over / (double)I.fileQ.size();
            for (size_t i = 0; i < I.fileQ.size(); i++) I.fileQ[i].due = now + dueIn(fg * (double)i);
        }
        base = now + dueIn(over);
        I.st.udpCatchUps++;
    }
    // if a backlog has built up, drain it a little faster instead of letting the latency grow
    const double backlog = std::chrono::duration<double>(base - now).count();
    const double eff = backlog > span ? span * 0.6 : span;
    if (!I.burst.empty()) {
        const double step = eff / I.burst.size();
        for (size_t i = 0; i < I.burst.size(); i++) {
            Impl::Dgram d;
            d.step = step;
            {
                d.data = std::move(I.burst[i]);
            }
            d.due = base + dueIn(step * i);
            I.queue.push_back(std::move(d));
        }
    }
    // The same clock for the recording. A player that reads the file while it is still being written used to see a whole burst
    // (about 100 ms of samples from the radio, or one T2 frame) land at once and then nothing until the next one.
    if (I.file && !I.fileHold.empty()) {
        constexpr size_t sliceBytes = 7 * 188;
        const size_t n = I.fileHold.size();
        const size_t ns = std::max<size_t>(1, (n + sliceBytes - 1) / sliceBytes);
        size_t off = 0;
        for (size_t i = 0; off < n; i++) {
            const size_t m = std::min(sliceBytes, n - off);
            Impl::Dgram d;
            d.toFile = true;
            d.data.assign(I.fileHold.begin() + (std::ptrdiff_t)off, I.fileHold.begin() + (std::ptrdiff_t)(off + m));
            d.due = base + dueIn(span * (double)i / (double)ns);
            I.fileQ.push_back(std::move(d));
            off += m;
        }
        I.fileHold.clear();
    }
    I.lastDue = base + dueIn(span);
    I.burst.clear();
    I.cv.notify_all();
}

OutputStats OutputManager::stats() const { std::lock_guard<std::mutex> lk(p_->mu); OutputStats s = p_->st; s.httpClients = p_->httpClients.load(); return s; }

} // namespace dect2
