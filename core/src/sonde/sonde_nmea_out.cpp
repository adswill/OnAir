// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The sender of the selected sonde's NMEA text (sonde_nmea_out.h): serial port, TCP client or UDP.
#include "dect2/sonde/sonde_nmea_out.h"
#include "dect2/common/diag.h"
#include "dect2/common/thread_name.h"
#include "../net/tcp_client.h"
#include <chrono>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace dect2 {

SondeNmeaOut& SondeNmeaOut::get() { static SondeNmeaOut* s = new SondeNmeaOut(); return *s; }   // never destroyed: the thread may still run at exit

namespace {
#ifndef _WIN32
speed_t speedOf(int baud) {
    switch (baud) {
    case 1200: return B1200; case 2400: return B2400; case 9600: return B9600; case 19200: return B19200;
    case 38400: return B38400; case 57600: return B57600; case 115200: return B115200; default: return B4800;
    }
}
#endif

// One open output. write() false: the link broke (reopen).
struct Link {
    int kind = 0;
    sock_t sock = kBadSock;
    sockaddr_in to{};
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool open(const SondeNmeaConfig& c, std::string& err) {
        kind = c.kind;
        if (c.kind == 1) { sock = tcpConnect(c.host, c.port, 3000, err); return sock != kBadSock; }
        if (c.kind == 2) {
            netInit();
            addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(c.host.c_str(), std::to_string(c.port).c_str(), &hints, &res) != 0 || !res) { err = "cannot find the address " + c.host; return false; }
            std::memcpy(&to, res->ai_addr, sizeof to); freeaddrinfo(res);
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock == kBadSock) { err = "cannot open a UDP socket"; return false; }
            return true;
        }
        if (c.path.empty()) { err = "choose a serial port"; return false; }
#ifdef _WIN32
        std::string p = c.path;
        const bool comName = p.size() >= 4 && (p[0] == 'C' || p[0] == 'c') && (p[1] == 'O' || p[1] == 'o') && (p[2] == 'M' || p[2] == 'm') && p.find_first_of("\\/.") == std::string::npos;   // COM10 and up need the device prefix; a file path (the test stands one in) is opened as it is
        if (comName && p.rfind("\\\\.\\", 0) != 0) p = "\\\\.\\" + p;
        h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { err = "cannot open " + c.path; return false; }
        DCB dcb{}; dcb.DCBlength = sizeof dcb;
        if (GetCommState(h, &dcb)) { dcb.BaudRate = (DWORD)c.baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT; SetCommState(h, &dcb); }   // a virtual port may refuse: it still takes the bytes
        return true;
#else
        fd = ::open(c.path.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) { err = "cannot open " + c.path; return false; }
        termios t;
        if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); t.c_cflag |= CLOCAL | CREAD; cfsetospeed(&t, speedOf(c.baud)); cfsetispeed(&t, speedOf(c.baud)); tcsetattr(fd, TCSANOW, &t); }   // a pty or a plain file has none: just write
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        return true;
#endif
    }
    bool write(const std::string& s) {
        if (kind == 1) return sockSendAll(sock, s.data(), s.size());
        if (kind == 2) return sockSendTo(sock, s.data(), s.size(), to) >= 0;
#ifdef _WIN32
        DWORD n = 0; return WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
#else
        size_t off = 0;
        while (off < s.size()) { const ssize_t n = ::write(fd, s.data() + off, s.size() - off); if (n <= 0) return false; off += (size_t)n; }
        return true;
#endif
    }
    void close() {
        if (sock != kBadSock) { sockClose(sock); sock = kBadSock; }
#ifdef _WIN32
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
#else
        if (fd >= 0) { ::close(fd); fd = -1; }
#endif
    }
};
}  // namespace

void SondeNmeaOut::configure(const SondeNmeaConfig& c) {
    stop();
    if (!c.enabled) return;
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = false; pending_.clear(); status_ = "starting"; sent_ = 0;
    const unsigned long long g = ++gen_;
    th_ = std::thread([this, c, g] { run(c, g); });
}

void SondeNmeaOut::send(const std::string& text) {
    if (text.empty()) return;
    { std::lock_guard<std::mutex> lk(mu_); pending_ = text; }
    cv_.notify_one();
}

std::string SondeNmeaOut::status() const { std::lock_guard<std::mutex> lk(mu_); return status_; }
unsigned long long SondeNmeaOut::sentCount() const { std::lock_guard<std::mutex> lk(mu_); return sent_; }

void SondeNmeaOut::stop() {
    quit_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    status_ = "off"; pending_.clear();
}

void SondeNmeaOut::run(SondeNmeaConfig c, unsigned long long myGen) {
    setThreadName("sondenmea");
    const std::string where = c.kind == 0 ? c.path + " at " + std::to_string(c.baud) + " baud" : (c.kind == 1 ? "TCP " : "UDP ") + c.host + ":" + std::to_string(c.port);
    Link link;
    bool isOpen = false;
    while (!quit_.load()) {
        if (!isOpen) {
            std::string err;
            if (link.open(c, err)) { isOpen = true; std::lock_guard<std::mutex> lk(mu_); status_ = "sending to " + where + ", waiting for a sonde"; }
            else {
                DIAG_EVERY(736305, 30, "The sonde NMEA output could not be opened (%s), so no GPS sentences are sent; trying again in a few seconds.", err.c_str());
                { std::lock_guard<std::mutex> lk(mu_); status_ = err + ", trying again"; }
                link.close();
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::seconds(3), [&] { return quit_.load(); });
                continue;
            }
        }
        std::string text;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return quit_.load() || !pending_.empty(); });
            if (myGen != gen_ || quit_.load()) break;
            text.swap(pending_);
        }
        if (text.empty()) continue;
        if (link.write(text)) { std::lock_guard<std::mutex> lk(mu_); sent_++; status_ = "sending to " + where; }
        else { link.close(); isOpen = false; std::lock_guard<std::mutex> lk(mu_); status_ = "the connection to " + where + " broke, reopening"; }
    }
    link.close();
}

}  // namespace dect2
