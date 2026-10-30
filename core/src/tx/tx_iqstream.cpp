#include "dect2/tx/tx_iqstream.h"
#include "dect2/common/thread_name.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#endif

namespace dect2 {

const char* txIqFormatName(TxIqFormat f) {
    switch (f) {
    case TxIqFormat::Cf32: return "cf32";
    case TxIqFormat::Cs16: return "cs16";
    case TxIqFormat::Ci16: return "ci16";
    case TxIqFormat::Cs8: return "cs8";
    case TxIqFormat::Cu8: return "cu8";
    }
    return "cf32";
}
bool txIqFormatParse(const std::string& s, TxIqFormat& f) {
    std::string l;
    for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
    for (int i = 0; i <= 4; i++) if (l == txIqFormatName((TxIqFormat)i)) { f = (TxIqFormat)i; return true; }
    return false;
}
size_t txIqBytesPerSample(TxIqFormat f) {
    switch (f) {
    case TxIqFormat::Cf32: return 8;
    case TxIqFormat::Cs16: case TxIqFormat::Ci16: return 4;
    default: return 2;
    }
}
static inline float clip1(float v) { return std::isfinite(v) ? std::max(-1.f, std::min(1.f, v)) : 0.f; }
void txIqPack(const cf32* x, size_t n, TxIqFormat f, uint8_t* out) {
    switch (f) {
    case TxIqFormat::Cf32:   // the host's float layout is little endian on every platform OnAir runs on
        std::memcpy(out, x, n * sizeof(cf32));
        break;
    case TxIqFormat::Cs16: case TxIqFormat::Ci16:
        for (size_t i = 0; i < n; i++) {
            const int16_t a = (int16_t)std::lrintf(clip1(x[i].real()) * 32767.f), b = (int16_t)std::lrintf(clip1(x[i].imag()) * 32767.f);
            out[4 * i] = (uint8_t)(a & 0xFF); out[4 * i + 1] = (uint8_t)((uint16_t)a >> 8);
            out[4 * i + 2] = (uint8_t)(b & 0xFF); out[4 * i + 3] = (uint8_t)((uint16_t)b >> 8);
        }
        break;
    case TxIqFormat::Cs8:
        for (size_t i = 0; i < n; i++) {
            out[2 * i] = (uint8_t)(int8_t)std::lrintf(clip1(x[i].real()) * 127.f);
            out[2 * i + 1] = (uint8_t)(int8_t)std::lrintf(clip1(x[i].imag()) * 127.f);
        }
        break;
    case TxIqFormat::Cu8:
        for (size_t i = 0; i < n; i++) {
            out[2 * i] = (uint8_t)std::lrintf(clip1(x[i].real()) * 127.5f + 127.5f);
            out[2 * i + 1] = (uint8_t)std::lrintf(clip1(x[i].imag()) * 127.5f + 127.5f);
        }
        break;
    }
}

namespace {
std::mutex gCfgMu;
TxIqStreamConfig gCfg;

void nonBlock(sock_t s) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(s, FIONBIO, &m);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
sock_t listenOn(const std::string& addr, int port, int& boundPort, std::string& err) {
    netInit();
    const sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) { err = "no socket"; return kBadSock; }
    int one = 1;
#ifndef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr.empty() ? "127.0.0.1" : addr.c_str(), &a.sin_addr) != 1) { err = "bad bind address \"" + addr + "\""; sockClose(s); return kBadSock; }
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 4) != 0) { err = "port " + std::to_string(port) + " is in use or not allowed"; sockClose(s); return kBadSock; }
    socklen_t l = sizeof a;
    if (getsockname(s, (sockaddr*)&a, &l) == 0) boundPort = ntohs(a.sin_port);
    nonBlock(s);
    return s;
}
}  // namespace

struct TxIqStream::Impl {
    struct Client { sock_t s = kBadSock; std::string out; };
    TxIqStreamConfig cfg;
    std::string json;
    sock_t ls = kBadSock, ms = kBadSock;
    int dataPort = 0, metaPortBound = 0;
    std::mutex mu;
    std::vector<Client> cl;
    std::atomic<bool> run{false};
    std::thread th;
    std::atomic<uint64_t> sent{0}, dropped{0};
    std::vector<uint8_t> buf;

    static void configureClient(sock_t k) {
        nonBlock(k);
#ifdef SO_NOSIGPIPE
        int one = 1; setsockopt(k, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);
#endif
    }
    // sends what is pending; false: the client is gone
    bool flush(Client& c) {
        while (!c.out.empty()) {
#ifdef MSG_NOSIGNAL
            const long long r = send(c.s, c.out.data(), c.out.size(), MSG_NOSIGNAL);
#else
            const long long r = sockSend(c.s, c.out.data(), c.out.size());
#endif
            if (r > 0) { c.out.erase(0, (size_t)r); continue; }
            if (r < 0 && wouldBlock()) return true;
            return false;
        }
        return true;
    }
    void acceptLoop() {
        while (run) {
            bool any = false;
            for (;;) {
                sockaddr_in pa{}; socklen_t pl = sizeof pa;
                const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
                if (k == kBadSock) break;
                any = true;
                configureClient(k);
                Client c; c.s = k;
                if (cfg.headerLine) c.out = json;
                std::lock_guard<std::mutex> lk(mu);
                if (!flush(c)) { sockClose(k); continue; }
                cl.push_back(std::move(c));
            }
            if (ms != kBadSock) for (;;) {
                sockaddr_in pa{}; socklen_t pl = sizeof pa;
                const sock_t k = accept(ms, (sockaddr*)&pa, &pl);
                if (k == kBadSock) break;
                any = true;
                configureClient(k);
                sockSend(k, json.data(), json.size());
                sockClose(k);
            }
            {   // drop clients that went away while nothing was pushed
                std::lock_guard<std::mutex> lk(mu);
                for (size_t i = 0; i < cl.size();) {
                    if (sockPeekClosed(cl[i].s) == 0) { sockClose(cl[i].s); cl.erase(cl.begin() + (long)i); } else i++;
                }
            }
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

TxIqStream::TxIqStream() : p_(std::make_unique<Impl>()) {}
TxIqStream::~TxIqStream() { stop(); }

bool TxIqStream::start(const TxIqStreamConfig& c, double rate, double centerHz, const std::string& modeId, std::string& err) {
    stop();
    Impl& d = *p_;
    d.cfg = c;
    char b[320];
    snprintf(b, sizeof b, "{\"onair_tx_iq\":1,\"format\":\"%s\",\"sample_rate\":%.0f,\"center_freq\":%.0f,\"mode\":\"%s\",\"active\":true}\n",
             txIqFormatName(c.format), rate, centerHz, modeId.c_str());
    d.json = b;
    d.ls = listenOn(c.bindAddr, c.port, d.dataPort, err);
    if (d.ls == kBadSock) return false;
    if (c.metaPort) {
        std::string e2;
        d.ms = listenOn(c.bindAddr, c.port == 0 ? 0 : c.port + 1, d.metaPortBound, e2);
        if (d.ms == kBadSock) err = "the side channel could not be opened: " + e2;   // a warning; the data port works
    }
    d.sent = 0; d.dropped = 0;
    d.run = true;
    d.th = dect2::namedThread("onair-txiq", [this] { p_->acceptLoop(); });
    return true;
}

void TxIqStream::stop() {
    Impl& d = *p_;
    d.run = false;
    if (d.th.joinable()) d.th.join();
    std::lock_guard<std::mutex> lk(d.mu);
    for (auto& c : d.cl) sockClose(c.s);
    d.cl.clear();
    if (d.ls != kBadSock) { sockClose(d.ls); d.ls = kBadSock; }
    if (d.ms != kBadSock) { sockClose(d.ms); d.ms = kBadSock; }
}

void TxIqStream::push(const cf32* x, size_t n) {
    Impl& d = *p_;
    if (!d.run || !n) return;
    std::lock_guard<std::mutex> lk(d.mu);
    if (d.cl.empty()) return;
    const size_t bps = txIqBytesPerSample(d.cfg.format);
    d.buf.resize(n * bps);
    txIqPack(x, n, d.cfg.format, d.buf.data());
    for (size_t i = 0; i < d.cl.size();) {
        auto& c = d.cl[i];
        if (!d.flush(c)) { sockClose(c.s); d.cl.erase(d.cl.begin() + (long)i); continue; }
        if (c.out.size() + d.buf.size() <= d.cfg.maxPendingBytes) {
            c.out.append((const char*)d.buf.data(), d.buf.size());
            d.sent += n;
            if (!d.flush(c)) { sockClose(c.s); d.cl.erase(d.cl.begin() + (long)i); continue; }
        } else d.dropped++;   // a client that cannot keep up loses whole blocks
        i++;
    }
}

int TxIqStream::clients() const { std::lock_guard<std::mutex> lk(p_->mu); return (int)p_->cl.size(); }
int TxIqStream::port() const { return p_->dataPort; }
uint64_t TxIqStream::samplesSent() const { return p_->sent; }
uint64_t TxIqStream::blocksDropped() const { return p_->dropped; }

void txIqStreamSetConfig(const TxIqStreamConfig& c) { std::lock_guard<std::mutex> lk(gCfgMu); gCfg = c; }
TxIqStreamConfig txIqStreamConfig() { std::lock_guard<std::mutex> lk(gCfgMu); return gCfg; }

}  // namespace dect2
