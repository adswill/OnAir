// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A TCP client connection with timeouts, for POSIX and Windows (Winsock): the network radios (source_rtltcp.cpp, netradio.cpp).
#pragma once
#include "netcompat.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace dect2 {

inline void sockSetNonBlocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long m = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &m);
#else
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

// a blocking recv() gives up after ms (returns -1)
inline void sockRecvTimeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// all n bytes, or false (the peer is gone); never raises SIGPIPE
inline bool sockSendAll(sock_t s, const void* d, size_t n) {
    const char* p = (const char*)d;
#if defined(MSG_NOSIGNAL)
    const int fl = MSG_NOSIGNAL;
#else
    const int fl = 0;
#endif
    while (n > 0) {
#ifdef _WIN32
        const int k = send(s, p, (int)n, fl);
#else
        const ssize_t k = send(s, p, n, fl);
#endif
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

// exactly n bytes within the receive timeout, or false
inline bool sockRecvAll(sock_t s, void* d, size_t n) {
    char* p = (char*)d;
    while (n > 0) {
        const long long k = sockRecv(s, p, n);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

// A connection to host:port within timeoutMs (name lookup included only as far as the system's resolver allows), or kBadSock with err set
inline sock_t tcpConnect(const std::string& host, int port, int timeoutMs, std::string& err) {
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        err = "cannot find the address " + host;
        return kBadSock;
    }
    sock_t ok = kBadSock;
    bool timedOut = false, refused = false;
    for (addrinfo* ai = res; ai && ok == kBadSock; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kBadSock) continue;
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
        { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
        sockSetNonBlocking(s, true);
        int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        bool connected = r == 0;
        if (!connected) {
#ifdef _WIN32
            const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool pending = errno == EINPROGRESS;
#endif
            if (pending) {
                fd_set wr, ex;
                FD_ZERO(&wr); FD_SET(s, &wr);
                FD_ZERO(&ex); FD_SET(s, &ex);
                timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
                const int n = select((int)s + 1, nullptr, &wr, &ex, &tv);
                if (n == 0) timedOut = true;
                else if (n > 0) {
                    int soErr = 0;
                    socklen_t len = sizeof soErr;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len);
                    connected = soErr == 0;
                    if (!connected) refused = true;
                }
            } else refused = true;
        }
        if (connected) {
            sockSetNonBlocking(s, false);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            // room for a fast stream (20 Msps of 8-bit IQ is 40 MB/s) while the receiver is busy: 16 MB, less if the system refuses
            for (int rcv : {16 << 20, 8 << 20, 4 << 20}) if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv) == 0) break;
            ok = s;
        } else sockClose(s);
    }
    freeaddrinfo(res);
    if (ok == kBadSock) {
        const std::string where = host + ":" + std::to_string(port);
        err = timedOut ? "no answer from " + where + " within " + std::to_string((timeoutMs + 500) / 1000) + " s (wrong address, or the radio is off)"
            : refused ? "connection to " + where + " refused (is the server running on that port?)"
            : "cannot connect to " + where;
    }
    return ok;
}

} // namespace dect2
