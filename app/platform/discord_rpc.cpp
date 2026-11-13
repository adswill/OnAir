// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "discord_rpc.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace discord {

// Application ID of the Discord application named OnAir (see README, "Discord status"). The official builds ship with it; ONAIR_DISCORD_APP_ID overrides it.
static const char* const kDiscordAppId = "1558882824717602895";

const char* appId() {
    static std::string id;
    const char* e = getenv("ONAIR_DISCORD_APP_ID");
    id = (e && *e) ? e : kDiscordAppId;
    return id.c_str();
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;   // UTF-8 passes through
        }
    }
    return o;
}

std::string encodeFrame(uint32_t op, const std::string& json) {
    std::string f(8, '\0');
    uint32_t n = (uint32_t)json.size();
    for (int i = 0; i < 4; i++) { f[i] = (char)((op >> (8 * i)) & 0xff); f[4 + i] = (char)((n >> (8 * i)) & 0xff); }
    return f + json;
}

std::string handshakeJson(const std::string& id) { return "{\"v\":1,\"client_id\":\"" + jsonEscape(id) + "\"}"; }

std::string activityJson(const Presence& p, int pid, const std::string& nonce) {
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":";
    if (!p.enabled) j += "null";
    else {
        j += "{\"details\":\"" + jsonEscape(p.running ? "Listening to " + p.mode : std::string("Idle")) + "\"";
        if (p.running && !p.freq.empty()) j += ",\"state\":\"" + jsonEscape(p.freq) + "\"";
        if (p.running && p.startUnix > 0) j += ",\"timestamps\":{\"start\":" + std::to_string(p.startUnix) + "}";
        j += ",\"assets\":{\"large_image\":\"onair\",\"large_text\":\"OnAir\"}}";
    }
    return j + "},\"nonce\":\"" + jsonEscape(nonce) + "\"}";
}

namespace {

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// One connection to Discord. All calls have short time-outs; none throws.
struct Conn {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open() { return h != INVALID_HANDLE_VALUE; }
    bool connect() {
        for (int i = 0; i < 10; i++) {
            std::string n = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            h = CreateFileA(n.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }
    void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    bool writeAll(const std::string& s) { DWORD w = 0; return WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size(); }
    bool readAll(char* b, size_t n, int ms) {
        double end = nowSec() + ms / 1000.0;
        while (n) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (!avail) { if (nowSec() > end) return false; Sleep(10); continue; }
            DWORD r = 0;
            if (!ReadFile(h, b, (DWORD)std::min<size_t>(avail, n), &r, nullptr) || !r) return false;
            b += r; n -= r;
        }
        return true;
    }
#else
    int fd = -1;
    bool open() { return fd >= 0; }
    bool connect() {
        const char* envs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
        std::string dirs[5]; int nd = 0;
        for (const char* e : envs) { const char* v = getenv(e); if (v && *v) dirs[nd++] = v; }
        dirs[nd++] = "/tmp";
        for (int d = 0; d < nd; d++) for (int i = 0; i < 10; i++) {
            std::string dir = dirs[d]; if (dir.back() != '/') dir += '/';
            std::string path = dir + "discord-ipc-" + std::to_string(i);
            sockaddr_un sa{}; sa.sun_family = AF_UNIX;
            if (path.size() >= sizeof sa.sun_path) continue;
            memcpy(sa.sun_path, path.c_str(), path.size() + 1);
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            if (::connect(s, (sockaddr*)&sa, sizeof sa) == 0) { fd = s; return true; }
            ::close(s);
        }
        return false;
    }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
    bool writeAll(const std::string& s) {
#ifdef MSG_NOSIGNAL
        const int fl = MSG_NOSIGNAL;
#else
        const int fl = 0;
#endif
        size_t o = 0;
        while (o < s.size()) {
            ssize_t w = ::send(fd, s.data() + o, s.size() - o, fl);
            if (w > 0) { o += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }
    bool readAll(char* b, size_t n, int ms) {
        double end = nowSec() + ms / 1000.0;
        while (n) {
            int left = (int)((end - nowSec()) * 1000); if (left < 0) return false;
            pollfd p{fd, POLLIN, 0};
            int r = poll(&p, 1, left);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
            ssize_t g = ::recv(fd, b, n, 0);
            if (g <= 0) return false;
            b += g; n -= (size_t)g;
        }
        return true;
    }
#endif
    bool readFrame(uint32_t& op, std::string& body, int ms) {
        unsigned char h8[8];
        if (!readAll((char*)h8, 8, ms)) return false;
        op = h8[0] | h8[1] << 8 | h8[2] << 16 | (uint32_t)h8[3] << 24;
        uint32_t n = h8[4] | h8[5] << 8 | h8[6] << 16 | (uint32_t)h8[7] << 24;
        if (n > (1u << 20)) return false;
        body.assign(n, '\0');
        return n == 0 || readAll(&body[0], n, ms);
    }
};

}  // namespace

Rpc::Rpc() : id_(appId()) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    th_ = dect2::namedThread("onair-discord", [this] { loop(); });
}

Rpc::~Rpc() {
    { std::lock_guard<std::mutex> l(mu_); quit_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Rpc::set(const Presence& p) {
    { std::lock_guard<std::mutex> l(mu_); if (want_ == p) return; want_ = p; }
    cv_.notify_all();
}

void Rpc::setIntervals(double a, double b) { std::lock_guard<std::mutex> l(mu_); minSend_ = a; retry_ = b; }
void Rpc::setAppId(const std::string& id) { std::lock_guard<std::mutex> l(mu_); id_ = id; }

void Rpc::loop() {
    Conn c;
    Presence sent; bool haveSent = false;
    double nextConnect = 0, lastSend = -1e9;
    int nonce = 0;
#ifdef _WIN32
    const int pid = (int)GetCurrentProcessId();
#else
    const int pid = (int)getpid();
#endif
    auto drop = [&] { c.close(); haveSent = false; };
    auto send = [&](const Presence& p) {
        bool ok = c.writeAll(encodeFrame(1, activityJson(p, pid, std::to_string(++nonce))));
        uint32_t op; std::string body;
        if (ok) ok = c.readFrame(op, body, 2000);   // Discord answers each command
        if (!ok) { drop(); return; }
        sent = p; haveSent = true; lastSend = nowSec();
    };
    for (;;) {
        Presence w; double minSend, retry; std::string id; bool quit;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::milliseconds(1000));   // changes notify; the timer only drives reconnects and the minimum send interval
            w = want_; minSend = minSend_; retry = retry_; id = id_; quit = quit_;
        }
        if (quit) w.enabled = false;
        const double t = nowSec();
        if (!w.enabled) {   // clear the activity, then hang up
            if (c.open()) {
                if (haveSent && sent.enabled) send(w);
                if (c.open()) { c.writeAll(encodeFrame(2, "{}")); drop(); }
            }
            nextConnect = 0;
            if (quit) return;
            continue;
        }
        if (id.empty()) continue;
        if (!c.open()) {
            if (t < nextConnect) continue;
            nextConnect = t + retry;
            if (!c.connect()) continue;
            uint32_t op; std::string body;
            if (!c.writeAll(encodeFrame(0, handshakeJson(id))) || !c.readFrame(op, body, 3000) || op == 2) { drop(); continue; }   // op 2: refused
            haveSent = false; lastSend = -1e9;
        }
        if ((!haveSent || !(sent == w)) && t - lastSend >= minSend) send(w);
    }
}

}  // namespace discord
