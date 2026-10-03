// Sockets for POSIX and Windows (Winsock) behind one small set of names.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
namespace dect2 {
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
inline void netInit() { static const bool once = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); (void)once; }
inline int sockClose(sock_t s) { return closesocket(s); }
inline long long sockSend(sock_t s, const void* d, size_t n) { return send(s, (const char*)d, (int)n, 0); }
inline long long sockSendTo(sock_t s, const void* d, size_t n, const sockaddr_in& to) { return sendto(s, (const char*)d, (int)n, 0, (const sockaddr*)&to, sizeof to); }
inline long long sockRecv(sock_t s, void* d, size_t n) { return recv(s, (char*)d, (int)n, 0); }
// 1 byte if one is waiting, 0 if the peer closed the connection, -1 if nothing is there yet
inline long long sockPeekClosed(sock_t s) {
    fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
    timeval tv{0, 0};
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return -1;
    char c; return recv(s, &c, 1, 0);
}
inline void sockShutdown(sock_t s) { shutdown(s, SD_BOTH); }
} // namespace dect2
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstddef>
namespace dect2 {
using sock_t = int;
constexpr sock_t kBadSock = -1;
inline void netInit() {}
inline int sockClose(sock_t s) { return ::close(s); }
inline long long sockSend(sock_t s, const void* d, size_t n) { return send(s, d, n, 0); }
inline long long sockSendTo(sock_t s, const void* d, size_t n, const sockaddr_in& to) { return sendto(s, d, n, 0, (const sockaddr*)&to, sizeof to); }
inline long long sockRecv(sock_t s, void* d, size_t n) { return recv(s, d, n, 0); }
inline long long sockPeekClosed(sock_t s) { char c; return recv(s, &c, 1, MSG_DONTWAIT); }
inline void sockShutdown(sock_t s) { shutdown(s, SHUT_RDWR); }
} // namespace dect2
#endif
