// Two small socket helpers of the OnAir Web setup, kept in a file of their own so the Winsock headers stay away from the rest of the app.
#include "webshare.h"
#include "../../core/src/net/netcompat.h"
#include <cstring>

namespace webshare {

// The first port from `start` that this computer can listen on (127.0.0.1), or 0.
int freeLocalPort(int start) {
    dect2::netInit();
    for (int p = start; p < start + 40; p++) {
        dect2::sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == dect2::kBadSock) return 0;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)p);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ok = bind(s, (const sockaddr*)&a, sizeof a) == 0;
        dect2::sockClose(s);
        if (ok) return p;
    }
    return 0;
}

// This computer's address on the local network (the one a phone on the same Wi-Fi would use), "" when there is none. Nothing is sent.
std::string lanAddress() {
    dect2::netInit();
    dect2::sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == dect2::kBadSock) return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xC0A80101);   // 192.168.1.1: a connect() on UDP only picks the route, no packet leaves
    std::string r;
    if (connect(s, (const sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, (sockaddr*)&me, &len) == 0) {
            char b[64] = {0};
            if (inet_ntop(AF_INET, &me.sin_addr, b, sizeof b)) r = b;
        }
    }
    dect2::sockClose(s);
    if (r.compare(0, 4, "127.") == 0 || r == "0.0.0.0") r.clear();
    return r;
}

} // namespace webshare
