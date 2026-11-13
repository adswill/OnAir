// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Clock discipline, the one-click clock sync and the hobby NTP server (see time_ntp.h).
#include "dect2/common/thread_name.h"
#include "dect2/time/time_ntp.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include "dect2/common/endian.h"

namespace dect2 {

double systemUnixNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- discipline
ClockDiscipline& ClockDiscipline::get() { static ClockDiscipline d; return d; }
void ClockDiscipline::publish(int s, const ClockMeasurement& m) {
    if (s < 0 || s >= kClockSources) return;
    std::lock_guard<std::mutex> lk(mu_);
    m_[s] = m;
    m_[s].atUnix = systemUnixNow();
}
bool ClockDiscipline::latest(int s, ClockMeasurement& m, double maxAgeS) const {
    if (s < 0 || s >= kClockSources) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!m_[s].valid || !std::isfinite(m_[s].offsetMs) || systemUnixNow() - m_[s].atUnix > maxAgeS) return false;
    m = m_[s];
    return true;
}
bool ClockDiscipline::best(ClockMeasurement& m, int& source, double maxAgeS) const {
    for (int s = 0; s < kClockSources; s++) if (latest(s, m, maxAgeS)) { source = s; return true; }
    return false;
}
void ClockDiscipline::clearAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& m : m_) m = ClockMeasurement();
}

// ---------------------------------------------------------------- sync
ClockOs clockThisOs() {
#if defined(_WIN32)
    return OsWindows;
#elif defined(__APPLE__)
    return OsMac;
#else
    return OsLinux;
#endif
}

std::string clockSyncRefusal(const ClockMeasurement& m, double nowUnix) {
    char b[256];
    if (!m.valid || !std::isfinite(m.offsetMs)) return "the source is not locked yet: there is no measured offset";
    if (nowUnix - m.atUnix > 30) return "the measurement is stale: the receiver is not running";
    if (std::fabs(m.offsetMs) > kClockSyncMaxOffsetMs) {
        snprintf(b, sizeof b, "the offset (%.0f s) is more than 10 minutes: the signal's time is not to be trusted, set the date by hand first", m.offsetMs / 1000);
        return b;
    }
    return "";
}

static long long clampMs(double ms) {
    if (!std::isfinite(ms)) return 0;
    return (long long)std::llround(std::max(-kClockSyncMaxOffsetMs, std::min(kClockSyncMaxOffsetMs, ms)));
}

std::string clockSetLimit(ClockOs os, double offsetMs) {
    if (os == OsMac && std::llabs(clampMs(offsetMs)) < 500) return "macOS sets the clock in whole seconds: an offset under 0.5 s cannot be corrected from here";
    return "";
}

std::string clockSetCommand(ClockOs os, double offsetMs) {
    const long long ms = clampMs(offsetMs);   // positive: the computer is ahead and goes back
    char b[320];
    if (os == OsMac) {
        const long long s = (long long)std::llround((double)ms / 1000.0);
        snprintf(b, sizeof b, "sudo date -u $(date -u -r $(( $(date -u +%%s) - (%lld) )) +%%m%%d%%H%%M%%Y.%%S)", s);
    } else if (os == OsWindows) {
        snprintf(b, sizeof b, "powershell -NoProfile -Command \"Set-Date -Adjust ([TimeSpan]::FromMilliseconds(%lld))\"", -ms);
    } else {
        snprintf(b, sizeof b, "sudo date -u -s \"@$(date -u -d \"now - (%lld) milliseconds\" +%%s.%%N)\"", ms);
    }
    return b;
}

std::string clockElevatedCommand(ClockOs os, double offsetMs) {
    const long long ms = clampMs(offsetMs);
    char b[512];
    if (os == OsMac) {
        const long long s = (long long)std::llround((double)ms / 1000.0);
        snprintf(b, sizeof b, "osascript -e 'do shell script \"date -u $(date -u -r $(( $(date -u +%%s) - (%lld) )) +%%m%%d%%H%%M%%Y.%%S)\" with administrator privileges'", s);
    } else if (os == OsWindows) {
        snprintf(b, sizeof b, "powershell -NoProfile -Command \"Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-Command','Set-Date -Adjust ([TimeSpan]::FromMilliseconds(%lld))'\"", -ms);
    } else {
        snprintf(b, sizeof b, "pkexec sh -c 'date -u -s \"@$(date -u -d \"now - (%lld) milliseconds\" +%%s.%%N)\"'", ms);
    }
    return b;
}

bool clockSyncRun(double offsetMs, std::string& message) {
    const ClockOs os = clockThisOs();
    const std::string lim = clockSetLimit(os, offsetMs);
    if (!lim.empty()) { message = lim; return false; }
    const std::string cmd = clockElevatedCommand(os, offsetMs) + " 2>&1";
#ifdef _WIN32
    FILE* f = _popen(cmd.c_str(), "r");
#else
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) { message = "could not start the system's administrator prompt"; return false; }
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) { if (out.size() < 400) out += buf; }
#ifdef _WIN32
    const int rc = _pclose(f);
#else
    const int rc = pclose(f);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0) {
        message = out.empty() ? "the administrator prompt was cancelled or refused" : out;
        return false;
    }
    ClockDiscipline::get().clearAll();   // the clock moved: the old offsets are void
    message = os == OsWindows ? "the clock was adjusted (check the time: Windows does not report a refusal inside the prompt)" : "the clock was set";
    return true;
}

// ---------------------------------------------------------------- NTP packets
namespace ntp {

static void put32(uint8_t* p, uint32_t v) { dect2::endian::storeBe32(p, v); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe32(p); }
static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

bool parse(const uint8_t* b, size_t n, Packet& p) {
    if (n < kPacketLen) return false;
    p.li = b[0] >> 6; p.vn = b[0] >> 3 & 7; p.mode = b[0] & 7;
    p.stratum = b[1]; p.poll = (int8_t)b[2]; p.precision = (int8_t)b[3];
    p.rootDelay = get32(b + 4); p.rootDispersion = get32(b + 8);
    memcpy(p.refId, b + 12, 4);
    p.reference = get64(b + 16); p.origin = get64(b + 24); p.receive = get64(b + 32); p.transmit = get64(b + 40);
    return true;
}
void serialize(const Packet& p, uint8_t* b) {
    b[0] = (uint8_t)((p.li & 3) << 6 | (p.vn & 7) << 3 | (p.mode & 7));
    b[1] = (uint8_t)p.stratum; b[2] = (uint8_t)(int8_t)p.poll; b[3] = (uint8_t)(int8_t)p.precision;
    put32(b + 4, p.rootDelay); put32(b + 8, p.rootDispersion);
    memcpy(b + 12, p.refId, 4);
    put64(b + 16, p.reference); put64(b + 24, p.origin); put64(b + 32, p.receive); put64(b + 40, p.transmit);
}
uint64_t fromUnix(double u) {
    const double whole = std::floor(u);
    const double frac = u - whole;
    const uint64_t s = (uint64_t)((int64_t)whole + (int64_t)kUnixToNtpSeconds) & 0xFFFFFFFFULL;
    uint64_t f = (uint64_t)(frac * 4294967296.0);
    if (f > 0xFFFFFFFFULL) f = 0xFFFFFFFFULL;
    return s << 32 | f;
}
double toUnix(uint64_t t) { return (double)((int64_t)(t >> 32) - (int64_t)kUnixToNtpSeconds) + (double)(t & 0xFFFFFFFFULL) / 4294967296.0; }
uint32_t toShort(double s) { return s <= 0 ? 0 : s >= 65535.99 ? 0xFFFFFFFFu : (uint32_t)(s * 65536.0); }
double fromShort(uint32_t v) { return (double)v / 65536.0; }

ServerState stateFor(const ClockMeasurement* m, int source, double nowUnix) {
    ServerState s;
    if (!m || !m->valid) { s.synced = false; s.stratum = 16; return s; }
    s.synced = true;
    const double age = std::max(0.0, nowUnix - m->atUnix);
    s.referenceUnix = m->atUnix;
    s.rootDelayS = 0;
    // the measurement's accuracy, the oscillator drift since (15 ppm), and a floor for a USB radio and a hobby set-up
    s.rootDispersionS = std::max(0.002, m->accuracyMs * 1e-3) + age * 15e-6;
    if (source == ClockGps) {
        s.stratum = 1;
        memcpy(s.refId, "GPS\0", 4);
    } else {
        s.stratum = 2;
        char id[5] = {0, 0, 0, 0, 0};
        snprintf(id, sizeof id, "%.4s", m->label.empty() ? "TIME" : m->label.c_str());
        memcpy(s.refId, id, 4);
    }
    return s;
}

bool makeReply(const Packet& req, const ServerState& st, double rxUnix, double txUnix, Packet& out) {
    if (req.mode != 3 || req.vn < 1 || req.vn > 4) return false;
    out = Packet();
    out.vn = req.vn;
    out.mode = 4;
    out.li = st.synced ? 0 : 3;
    out.stratum = st.synced ? st.stratum : 16;
    out.poll = req.poll < 4 ? 4 : req.poll > 10 ? 10 : req.poll;
    out.precision = -20;
    out.rootDelay = toShort(st.rootDelayS);
    out.rootDispersion = toShort(st.synced ? st.rootDispersionS : 1.0);
    if (st.synced) memcpy(out.refId, st.refId, 4);
    out.reference = st.synced ? fromUnix(st.referenceUnix) : 0;
    out.origin = req.transmit;     // the client matches its reply by this field
    out.receive = fromUnix(rxUnix);
    out.transmit = fromUnix(txUnix);
    return true;
}

bool isLanAddress(uint32_t a) {
    const uint32_t b0 = a >> 24, b1 = a >> 16 & 255;
    return b0 == 10 || b0 == 127 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168) || (b0 == 169 && b1 == 254);
}
std::string ipText(uint32_t a) {
    char b[24];
    snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
    return b;
}

bool RateLimiter::allow(uint32_t client, double now) {
    if (secStart_ < 0 || now - secStart_ >= 1 || now < secStart_) { secStart_ = now; secCount_ = 0; }
    if (secCount_ >= kTotalPerS) return false;
    Entry& e = e_[(client * 2654435761u >> 8) % kSlots];
    if (e.ip != client) { e = Entry(); e.ip = client; }
    int keep = 0;
    for (int i = 0; i < e.n; i++) if (now - e.t[i] < kWindowS && now >= e.t[i]) e.t[keep++] = e.t[i];
    e.n = keep;
    if (e.n >= kPerClient) return false;
    e.t[e.n++] = now;
    secCount_++;
    return true;
}

struct Server::Impl {
    std::atomic<bool> run{false};
    std::thread th;
    sock_t sock = kBadSock;
    int port = 0;
    mutable std::mutex mu;
    ServerStats st;

    void loop() {
        RateLimiter lim;
        uint8_t buf[512];
        while (run.load()) {
            sockaddr_in from;
            memset(&from, 0, sizeof from);
#ifdef _WIN32
            int fl = sizeof from;
            const int n = recvfrom(sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
#else
            socklen_t fl = sizeof from;
            const ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
#endif
            if (n <= 0) continue;   // the receive timeout
            const double rx = systemUnixNow();
            const uint32_t ip = ntohl(from.sin_addr.s_addr);
            Packet req, rep;
            if (!isLanAddress(ip) || !parse(buf, (size_t)n, req) || req.mode != 3 || req.vn < 1 || req.vn > 4) {
                std::lock_guard<std::mutex> lk(mu); st.refused++;
                continue;
            }
            if (!lim.allow(ip, rx)) { std::lock_guard<std::mutex> lk(mu); st.limited++; continue; }
            ClockMeasurement m;
            int src = ClockGps;
            const bool have = ClockDiscipline::get().best(m, src, 600);
            const double off = have ? m.offsetMs * 1e-3 : 0;
            const ServerState state = stateFor(have ? &m : nullptr, src, rx);
            if (!makeReply(req, state, rx - off, systemUnixNow() - off, rep)) continue;
            uint8_t out[kPacketLen];
            serialize(rep, out);
            sockSendTo(sock, out, sizeof out, from);
            std::lock_guard<std::mutex> lk(mu);
            st.requests++;
            if (!state.synced) st.unsynced++;
            st.lastClient = ip; st.lastAtUnix = rx;
        }
    }
};

Server::Server() : p_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

bool Server::start(int fallbackPort, std::string& message, bool loopbackOnly, int firstPort) {
    stop();
    netInit();
    Impl& m = *p_;
    int usedPort = 0;
    sock_t s = kBadSock;
    std::string why;
    for (int pass = 0; pass < 2 && usedPort == 0; pass++) {
        const int port = pass == 0 ? firstPort : fallbackPort;
        if (pass == 1 && port == firstPort) break;
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kBadSock) { message = "could not create the UDP socket"; return false; }
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(loopbackOnly ? 0x7F000001u : 0u);
        if (bind(s, (sockaddr*)&a, sizeof a) == 0) { usedPort = port; break; }
        why = port < 1024 ? "port " + std::to_string(port) + " needs administrator rights" : "port " + std::to_string(port) + " is in use";
        sockClose(s);
        s = kBadSock;
    }
    if (usedPort == 0) { message = "could not open a UDP port: " + why; return false; }
#ifdef _WIN32
    DWORD to = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
#else
    timeval tv{0, 200000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    m.sock = s; m.port = usedPort;
    { std::lock_guard<std::mutex> lk(m.mu); m.st = ServerStats(); }
    m.run = true;
    m.th = dect2::namedThread("onair-ntp", [&m] { m.loop(); });
    char b[320];
    if (usedPort == firstPort) snprintf(b, sizeof b, "NTP server on UDP %d", usedPort);
    else snprintf(b, sizeof b, "%s, so the NTP server listens on UDP %d instead: clients must be told that port (or forward 123 to it)", why.c_str(), usedPort);
    message = b;
    return true;
}

void Server::stop() {
    Impl& m = *p_;
    if (!m.run.exchange(false)) { if (m.th.joinable()) m.th.join(); return; }
    if (m.th.joinable()) m.th.join();
    if (m.sock != kBadSock) { sockClose(m.sock); m.sock = kBadSock; }
    m.port = 0;
}
bool Server::running() const { return p_->run.load(); }
int Server::port() const { return p_->run.load() ? p_->port : 0; }
ServerStats Server::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

} // namespace ntp
} // namespace dect2
