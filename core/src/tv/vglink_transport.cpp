// The video generator link's transports (vglink_transport.h): UDP (RTP / RTCP ports), and the local named pipe / Unix socket.
#include "vglink_transport.h"
#include "../net/netcompat.h"
#include "dect2/common/thread_name.h"
#include "dect2/tv/vglink.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

namespace dect2::vgen::link {

namespace {

// ---- UDP ------------------------------------------------------------------------------------------------------------------------

sock_t udpSocket(uint32_t addr, int port, int rcvBuf) {
    const sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return s;
    if (rcvBuf > 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof rcvBuf);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { sockClose(s); return kBadSock; }
    return s;
}

int boundPort(sock_t s) {
    sockaddr_in a{};
    socklen_t al = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &al);
    return ntohs(a.sin_port);
}

// waits up to ms for one of the sockets to be readable; a bit set per readable one, -1 on error
int waitReadable(sock_t a, sock_t b, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(a, &rd);
    if (b != kBadSock) FD_SET(b, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
#ifdef _WIN32
    const int r = select(0, &rd, nullptr, nullptr, &tv);
#else
    const int r = select(std::max(a, b) + 1, &rd, nullptr, nullptr, &tv);
#endif
    if (r < 0) return -1;
    return (FD_ISSET(a, &rd) ? 1 : 0) | (b != kBadSock && FD_ISSET(b, &rd) ? 2 : 0);
}

// "a.b.c.d:port" <-> sockaddr_in: the key of a peer and where its answers go
std::string keyOf(const sockaddr_in& a) {
    std::array<char, 64> b{};
    inet_ntop(AF_INET, &a.sin_addr, b.data(), (socklen_t)b.size());
    return std::string(b.data()) + ":" + std::to_string(ntohs(a.sin_port));
}
bool addrOf(const std::string& key, sockaddr_in& a) {
    const size_t c = key.rfind(':');
    if (c == std::string::npos) return false;
    a = sockaddr_in{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)std::atoi(key.c_str() + c + 1));
    return inet_pton(AF_INET, key.substr(0, c).c_str(), &a.sin_addr) == 1;
}

class UdpServer final : public ServerTransport {
public:
    UdpServer(sock_t rtp, sock_t rtcp, OnDatagram cb) : rtp_(rtp), rtcp_(rtcp), cb_(std::move(cb)) {
        th_ = namedThread("vg-link-udp", [this] { run(); });
    }
    ~UdpServer() override { stop(); }
    bool send(const std::string& to, const uint8_t* d, size_t n) override {
        sockaddr_in a{};
        if (!addrOf(to, a)) return false;
        std::lock_guard<std::mutex> lk(sendMu_);
        return rtcp_ != kBadSock && sockSendTo(rtcp_, d, n, a) == (long long)n;
    }
    void stop() override {
        stop_ = true;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(sendMu_);
        if (rtp_ != kBadSock) { sockClose(rtp_); rtp_ = kBadSock; }
        if (rtcp_ != kBadSock) { sockClose(rtcp_); rtcp_ = kBadSock; }
    }
    int port() const override { return port_; }
    int port_ = 0;

private:
    void run() {
        std::vector<uint8_t> buf(65536);
        while (!stop_) {
            const int w = waitReadable(rtp_, rtcp_, 100);
            if (w <= 0) continue;
            for (int k = 0; k < 2; k++) {
                if (!(w & (1 << k))) continue;
                const sock_t s = k == 0 ? rtp_ : rtcp_;
                for (int i = 0; i < (k == 0 ? 256 : 16); i++) {   // drain the RTP socket first: it carries the stream
                    sockaddr_in from{};
                    socklen_t fl = sizeof from;
                    const auto r = recvfrom(s, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
                    // (on Windows a former ICMP "port unreachable" shows up here as an error: harmless)
                    if (r > 0 && (size_t)r <= buf.size()) {
                        std::array<char, 64> h{};
                        inet_ntop(AF_INET, &from.sin_addr, h.data(), (socklen_t)h.size());
                        cb_(buf.data(), (size_t)r, keyOf(from), h.data());
                    }
                    if (waitReadable(s, kBadSock, 0) <= 0) break;
                }
            }
        }
    }
    sock_t rtp_, rtcp_;
    OnDatagram cb_;
    std::mutex sendMu_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

class UdpClient final : public ClientTransport {
public:
    UdpClient(std::string host, int port) : host_(std::move(host)), port_(port) {
        s_ = udpSocket(INADDR_ANY, 0, 0);
        int sndBuf = 1 << 20;
        if (s_ != kBadSock) setsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof sndBuf);
    }
    ~UdpClient() override { if (s_ != kBadSock) sockClose(s_); }
    bool open(std::string& why) override {
        if (s_ == kBadSock) { why = "cannot make a UDP socket"; return false; }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) { why = "cannot find the computer " + host_ + " (trying again)"; return false; }
        sockaddr_in a{};
        std::copy_n(reinterpret_cast<const uint8_t*>(res->ai_addr), std::min(sizeof a, (size_t)res->ai_addrlen), reinterpret_cast<uint8_t*>(&a));
        freeaddrinfo(res);
        a.sin_port = htons((uint16_t)port_);
        rtpTo_ = a;
        a.sin_port = htons((uint16_t)(port_ + 1));
        rtcpTo_ = a;
        open_ = true;
        return true;
    }
    bool isOpen() const override { return open_; }
    void close() override { open_ = false; }
    bool sendRtp(const uint8_t* d, size_t n) override { return open_ && sockSendTo(s_, d, n, rtpTo_) == (long long)n; }
    bool sendRtcp(const uint8_t* d, size_t n) override { return open_ && sockSendTo(s_, d, n, rtcpTo_) == (long long)n; }
    long long recv(std::vector<uint8_t>& buf, int ms) override {
        if (s_ == kBadSock || waitReadable(s_, kBadSock, ms) <= 0) return -1;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const auto r = recvfrom(s_, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (r <= 0 || (size_t)r > buf.size()) return -1;
        if (from.sin_addr.s_addr != rtcpTo_.sin_addr.s_addr || from.sin_port != rtcpTo_.sin_port) return -1;   // only OnAir's RTCP port
        return (long long)r;
    }
    std::string where() const override { return host_ + ":" + std::to_string(port_); }

private:
    std::string host_;
    int port_;
    sock_t s_ = kBadSock;
    sockaddr_in rtpTo_{}, rtcpTo_{};
    std::atomic<bool> open_{false};
};

std::string safeUser(const char* u) {
    std::string s;
    for (const char* p = u; p && *p && s.size() < 40; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') s += *p;
    return s.empty() ? "user" : s;
}

#ifdef _WIN32

// ---- Windows: a named pipe in message mode ------------------------------------------------------------------------------------

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // (ASCII only: localName() is)

constexpr DWORD kPipeBuf = 1 << 20;   // per direction and generator: about 1 s of a 8 Mbit/s multiplex

// one overlapped read kept pending across waits; one write at a time
struct PipeIo {
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED rd{}, wr{};
    std::vector<uint8_t> rbuf = std::vector<uint8_t>(65536);
    bool reading = false;
    std::mutex wmu;
    PipeIo() {
        rd.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wr.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeIo() {
        closeHandle();
        CloseHandle(rd.hEvent);
        CloseHandle(wr.hEvent);
    }
    void closeHandle() {
        if (h == INVALID_HANDLE_VALUE) return;
        CancelIoEx(h, nullptr);
        DWORD n = 0;
        if (reading) GetOverlappedResult(h, &rd, &n, TRUE);
        reading = false;
        std::lock_guard<std::mutex> lk(wmu);
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    // -1: nothing within ms, -2: the other end is gone, else a whole message's length (in rbuf)
    long long read(int ms) {
        DWORD n = 0;
        if (!reading) {
            ResetEvent(rd.hEvent);
            if (ReadFile(h, rbuf.data(), (DWORD)rbuf.size(), &n, &rd)) return (long long)n;
            const DWORD e = GetLastError();
            if (e == ERROR_MORE_DATA) { drainRest(); return -1; }
            if (e != ERROR_IO_PENDING) return -2;
            reading = true;
        }
        if (WaitForSingleObject(rd.hEvent, (DWORD)ms) != WAIT_OBJECT_0) return -1;
        reading = false;
        if (GetOverlappedResult(h, &rd, &n, FALSE)) return (long long)n;
        if (GetLastError() == ERROR_MORE_DATA) { drainRest(); return -1; }
        return -2;
    }
    void drainRest() {   // a message larger than any of ours: skipped
        DWORD n = 0;
        for (int i = 0; i < 64; i++) {
            ResetEvent(rd.hEvent);
            if (ReadFile(h, rbuf.data(), (DWORD)rbuf.size(), &n, &rd)) return;
            if (GetLastError() == ERROR_IO_PENDING) GetOverlappedResult(h, &rd, &n, TRUE);
            else if (GetLastError() != ERROR_MORE_DATA) return;
        }
    }
    // a whole message, or nothing: waits at most ms when the pipe is full (then it is dropped, as UDP would)
    bool write(const uint8_t* d, size_t n, int ms) {
        std::lock_guard<std::mutex> lk(wmu);
        if (h == INVALID_HANDLE_VALUE) return false;
        ResetEvent(wr.hEvent);
        DWORD done = 0;
        if (WriteFile(h, d, (DWORD)n, &done, &wr)) return done == n;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(wr.hEvent, (DWORD)ms) != WAIT_OBJECT_0) {
            CancelIoEx(h, &wr);
            GetOverlappedResult(h, &wr, &done, TRUE);
            return false;
        }
        return GetOverlappedResult(h, &wr, &done, FALSE) && done == n;
    }
};

HANDLE newInstance(const std::string& name, bool first) {
    return CreateNamedPipeW(wide(name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                            kPipeBuf, kPipeBuf, 0, nullptr);
}

class PipeServer final : public ServerTransport {
public:
    PipeServer(std::string name, HANDLE first, OnDatagram cb) : name_(std::move(name)), cb_(std::move(cb)) {
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        th_ = namedThread("vg-link-pipe", [this, first] { listen(first); });
    }
    ~PipeServer() override { stop(); CloseHandle(stopEv_); }
    bool send(const std::string& to, const uint8_t* d, size_t n) override {
        std::shared_ptr<Conn> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& x : conns_) if (x->key == to) c = x;
        }
        return c && c->io.write(d, n, 50);
    }
    void stop() override {
        SetEvent(stopEv_);
        if (th_.joinable()) th_.join();
        std::vector<std::shared_ptr<Conn>> cs;
        { std::lock_guard<std::mutex> lk(mu_); cs.swap(conns_); }
        for (auto& c : cs) if (c->th.joinable()) c->th.join();
    }

private:
    struct Conn {
        PipeIo io;
        std::string key;
        std::thread th;
        std::atomic<bool> done{false};
    };
    void listen(HANDLE h) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        int next = 1;
        while (h != INVALID_HANDLE_VALUE) {
            ResetEvent(ov.hEvent);
            bool connected = ConnectNamedPipe(h, &ov) != 0;
            if (!connected) {
                const DWORD e = GetLastError();
                if (e == ERROR_PIPE_CONNECTED) connected = true;
                else if (e == ERROR_IO_PENDING) {
                    const HANDLE w[2] = {ov.hEvent, stopEv_};
                    const DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
                    if (r != WAIT_OBJECT_0) {   // stop
                        CancelIoEx(h, &ov);
                        DWORD n = 0;
                        GetOverlappedResult(h, &ov, &n, TRUE);
                        CloseHandle(h);
                        break;
                    }
                    DWORD n = 0;
                    connected = GetOverlappedResult(h, &ov, &n, FALSE) != 0;
                }
            }
            if (connected) {
                auto c = std::make_shared<Conn>();
                c->io.h = h;
                c->key = "pipe:" + std::to_string(next++);
                std::lock_guard<std::mutex> lk(mu_);
                for (size_t i = conns_.size(); i-- > 0;)   // the ended ones
                    if (conns_[i]->done) { conns_[i]->th.join(); conns_.erase(conns_.begin() + (std::ptrdiff_t)i); }
                c->th = namedThread("vg-link-pipe", [this, c] { serve(*c); });
                conns_.push_back(c);
            } else
                CloseHandle(h);
            h = WaitForSingleObject(stopEv_, 0) == WAIT_OBJECT_0 ? INVALID_HANDLE_VALUE : newInstance(name_, false);
        }
        CloseHandle(ov.hEvent);
    }
    void serve(Conn& c) {
        while (WaitForSingleObject(stopEv_, 0) != WAIT_OBJECT_0) {
            const long long n = c.io.read(100);
            if (n == -2) break;   // the generator closed it
            if (n > 0) cb_(c.io.rbuf.data(), (size_t)n, c.key, "");
        }
        DisconnectNamedPipe(c.io.h);
        c.io.closeHandle();
        c.done = true;
    }
    std::string name_;
    OnDatagram cb_;
    HANDLE stopEv_ = nullptr;
    std::thread th_;
    std::mutex mu_;
    std::vector<std::shared_ptr<Conn>> conns_;
};

class PipeClient final : public ClientTransport {
public:
    explicit PipeClient(std::string name) : name_(std::move(name)) {}
    ~PipeClient() override { close(); }
    bool open(std::string& why) override {
        close();
        const HANDLE h = CreateFileW(wide(name_).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            why = GetLastError() == ERROR_PIPE_BUSY ? "OnAir is busy (trying again)" : "OnAir is not running (trying again)";
            return false;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        { std::lock_guard<std::mutex> lk(io_.wmu); io_.h = h; }
        open_ = true;
        return true;
    }
    bool isOpen() const override { return open_; }
    void close() override { open_ = false; io_.closeHandle(); }
    bool sendRtp(const uint8_t* d, size_t n) override { return open_ && io_.write(d, n, 20); }
    bool sendRtcp(const uint8_t* d, size_t n) override { return open_ && io_.write(d, n, 50); }
    long long recv(std::vector<uint8_t>& buf, int ms) override {
        if (!open_) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); return -1; }
        const long long n = io_.read(ms);
        if (n == -2) { open_ = false; return -1; }   // OnAir went away: open() again later
        if (n <= 0 || (size_t)n > buf.size()) return -1;
        std::copy_n(io_.rbuf.begin(), (size_t)n, buf.begin());
        return n;
    }
    std::string where() const override { return "this computer"; }

private:
    std::string name_;
    PipeIo io_;
    std::atomic<bool> open_{false};
};

#else

// ---- Linux / macOS: a Unix-domain datagram socket --------------------------------------------------------------------------------

// sockaddr_un from a path, or false when it does not fit (sun_path is ~104 bytes)
bool unAddr(const std::string& path, sockaddr_un& a, socklen_t& len) {
    a = sockaddr_un{};
    a.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof a.sun_path) return false;
    std::copy(path.begin(), path.end(), a.sun_path);
    len = (socklen_t)(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

// the bytes of a peer's address (a Linux autobind address starts with a 0 byte): its key
std::string unKey(const sockaddr_un& a, socklen_t len) {
    const size_t n = len > offsetof(sockaddr_un, sun_path) ? (size_t)len - offsetof(sockaddr_un, sun_path) : 0;
    return std::string(a.sun_path, a.sun_path + std::min(n, sizeof a.sun_path));
}

sock_t unSocket() {
    const sock_t s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s == kBadSock) return s;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int buf = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    return s;
}

class UnixServer final : public ServerTransport {
public:
    UnixServer(sock_t s, std::string path, OnDatagram cb) : s_(s), path_(std::move(path)), cb_(std::move(cb)) {
        th_ = namedThread("vg-link-local", [this] { run(); });
    }
    ~UnixServer() override { stop(); }
    bool send(const std::string& to, const uint8_t* d, size_t n) override {
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (to.empty() || to.size() > sizeof a.sun_path) return false;
        std::copy(to.begin(), to.end(), a.sun_path);
        std::lock_guard<std::mutex> lk(mu_);
        return s_ != kBadSock && sendto(s_, d, n, kSendFlags, reinterpret_cast<const sockaddr*>(&a), (socklen_t)(offsetof(sockaddr_un, sun_path) + to.size())) == (ssize_t)n;
    }
    void stop() override {
        stop_ = true;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(mu_);
        if (s_ != kBadSock) { sockClose(s_); s_ = kBadSock; unlink(path_.c_str()); }
    }

private:
    void run() {
        std::vector<uint8_t> buf(65536);
        while (!stop_) {
            if (waitReadable(s_, kBadSock, 100) <= 0) continue;
            for (int i = 0; i < 256; i++) {
                sockaddr_un from{};
                socklen_t fl = sizeof from;
                const ssize_t r = recvfrom(s_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (r <= 0 || (size_t)r > buf.size()) break;
                const std::string key = unKey(from, fl);
                if (!key.empty()) cb_(buf.data(), (size_t)r, key, "");   // (an unbound sender cannot get answers: ignored)
            }
        }
    }
    sock_t s_;
    std::string path_;
    OnDatagram cb_;
    std::mutex mu_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

class UnixClient final : public ClientTransport {
public:
    explicit UnixClient(std::string path) : path_(std::move(path)) {
        s_ = unSocket();
        if (s_ == kBadSock) return;
#ifdef __linux__
        sockaddr_un a{};   // autobind: a unique abstract address, nothing to clean up
        a.sun_family = AF_UNIX;
        bound_ = bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof(sa_family_t)) == 0;
#else
        static std::atomic<int> n{0};
        own_ = path_ + "." + std::to_string((long)getpid()) + "." + std::to_string(n++);
        sockaddr_un a{};
        socklen_t al = 0;
        unlink(own_.c_str());
        bound_ = unAddr(own_, a, al) && bind(s_, reinterpret_cast<sockaddr*>(&a), al) == 0;
        if (!bound_) own_.clear();
#endif
    }
    ~UnixClient() override {
        if (s_ != kBadSock) sockClose(s_);
        if (!own_.empty()) unlink(own_.c_str());
    }
    bool open(std::string& why) override {
        if (s_ == kBadSock || !bound_) { why = "cannot make a local socket"; return false; }
        if (!unAddr(path_, to_, toLen_)) { why = "the local socket's path is too long: " + path_; return false; }
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0) { why = "OnAir is not running (trying again)"; return false; }
        open_ = true;
        return true;
    }
    bool isOpen() const override { return open_; }
    void close() override { open_ = false; }
    bool sendRtp(const uint8_t* d, size_t n) override { return send(d, n); }
    bool sendRtcp(const uint8_t* d, size_t n) override { return send(d, n); }
    long long recv(std::vector<uint8_t>& buf, int ms) override {
        if (s_ == kBadSock || waitReadable(s_, kBadSock, ms) <= 0) return -1;
        sockaddr_un from{};
        socklen_t fl = sizeof from;
        const ssize_t r = recvfrom(s_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (r <= 0 || (size_t)r > buf.size()) return -1;
        if (unKey(from, fl) != unKey(to_, toLen_)) return -1;   // only OnAir
        return (long long)r;
    }
    std::string where() const override { return "this computer"; }

private:
    bool send(const uint8_t* d, size_t n) {
        if (!open_) return false;
        const ssize_t r = sendto(s_, d, n, kSendFlags, reinterpret_cast<const sockaddr*>(&to_), toLen_);
        if (r == (ssize_t)n) return true;
        if (r < 0 && (errno == ECONNREFUSED || errno == ENOENT)) open_ = false;   // OnAir went away (full: EAGAIN, just dropped)
        return false;
    }
    std::string path_, own_;
    sock_t s_ = kBadSock;
    bool bound_ = false;
    sockaddr_un to_{};
    socklen_t toLen_ = 0;
    std::atomic<bool> open_{false};
};

#endif

} // namespace

// ---- the factories ----------------------------------------------------------------------------------------------------------------

std::unique_ptr<ServerTransport> listenNetwork(int port, bool anyAddress, OnDatagram cb, std::string& err) {
    netInit();
    const uint32_t addr = anyAddress ? INADDR_ANY : INADDR_LOOPBACK;
    constexpr int kRcvBuf = 4 << 20;   // a few hundred ms of eight generators, should the thread be held up
    sock_t a = kBadSock, b = kBadSock;
    if (port > 0) {
        if (port % 2 || port >= 65535) { err = "the port must be even (RTP; RTCP uses the next one)"; return nullptr; }
        a = udpSocket(addr, port, kRcvBuf);
        b = a == kBadSock ? kBadSock : udpSocket(addr, port + 1, 0);
    } else {
        for (int tries = 0; tries < 40 && b == kBadSock; tries++) {   // a free pair: the system's choice for RTP, rounded to even
            if (a != kBadSock) sockClose(a);
            a = kBadSock;
            const sock_t t = udpSocket(addr, 0, 0);
            if (t == kBadSock) break;
            const int p = boundPort(t) & ~1;
            sockClose(t);
            if (p < 1024 || p >= 65534) continue;
            a = udpSocket(addr, p, kRcvBuf);
            if (a != kBadSock) b = udpSocket(addr, p + 1, 0);
        }
    }
    if (a == kBadSock || b == kBadSock) {
        if (a != kBadSock) sockClose(a);
        err = port > 0 ? "UDP port " + std::to_string(port) + " or " + std::to_string(port + 1) + " is in use (another OnAir?)" : "no free UDP ports";
        return nullptr;
    }
    const int bound = boundPort(a);
    auto s = std::make_unique<UdpServer>(a, b, std::move(cb));
    s->port_ = bound;
    return s;
}

std::string localName() {
#ifdef _WIN32
    return "\\\\.\\pipe\\onair-videogen-" + safeUser(std::getenv("USERNAME"));
#else
    const char* rt = std::getenv("XDG_RUNTIME_DIR");   // the user's own folder (mode 0700): no other user can reach the socket
    std::string dir = rt && *rt ? rt : "";
#ifdef __APPLE__
    if (dir.empty()) { const char* t = std::getenv("TMPDIR"); if (t && *t) dir = t; }   // per user on macOS
#endif
    if (dir.empty()) {
        dir = "/tmp/onair-" + std::to_string((long)getuid());
        mkdir(dir.c_str(), 0700);
    }
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir + "/onair-videogen.sock";
#endif
}

std::unique_ptr<ServerTransport> listenLocal(const std::string& nameIn, OnDatagram cb, std::string& err) {
    const std::string name = nameIn.empty() ? localName() : nameIn;
#ifdef _WIN32
    const HANDLE h = newInstance(name, true);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError() == ERROR_ACCESS_DENIED ? "another OnAir already has the generator pipe " + name : "cannot make the pipe " + name;
        return nullptr;
    }
    return std::make_unique<PipeServer>(name, h, std::move(cb));
#else
    sockaddr_un a{};
    socklen_t al = 0;
    if (!unAddr(name, a, al)) { err = "the local socket's path is too long: " + name; return nullptr; }
    const sock_t s = unSocket();
    if (s == kBadSock) { err = "cannot make a local socket"; return nullptr; }
    if (bind(s, reinterpret_cast<sockaddr*>(&a), al) != 0) {
        if (errno != EADDRINUSE) { sockClose(s); err = "cannot use the local socket " + name; return nullptr; }
        // in use: another OnAir, or a leftover of one that crashed (nobody answers there)
        const sock_t probe = socket(AF_UNIX, SOCK_DGRAM, 0);
        const bool alive = probe != kBadSock && connect(probe, reinterpret_cast<sockaddr*>(&a), al) == 0;
        if (probe != kBadSock) sockClose(probe);
        if (alive) { sockClose(s); err = "another OnAir already has the generator socket " + name; return nullptr; }
        unlink(name.c_str());
        if (bind(s, reinterpret_cast<sockaddr*>(&a), al) != 0) { sockClose(s); err = "cannot use the local socket " + name; return nullptr; }
    }
    chmod(name.c_str(), 0600);
    return std::make_unique<UnixServer>(s, name, std::move(cb));
#endif
}

std::unique_ptr<ClientTransport> networkClient(const std::string& host, int port) {
    netInit();
    return std::make_unique<UdpClient>(host.empty() ? "127.0.0.1" : host, port > 0 && port < 65535 ? port : kVgLinkDefaultPort);
}

std::unique_ptr<ClientTransport> localClient(const std::string& name) {
    return std::make_unique<
#ifdef _WIN32
        PipeClient
#else
        UnixClient
#endif
        >(name.empty() ? localName() : name);
}

} // namespace dect2::vgen::link
