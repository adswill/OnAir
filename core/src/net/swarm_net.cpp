// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm discovery over UDP multicast. See dect2/net/swarm_net.h.
#include "dect2/net/swarm_net.h"
#include "netcompat.h"
#include <atomic>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace dect2::swarm {
namespace {
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void closeSock(sock_t& s) { if (s != kBadSock) { sockClose(s); s = kBadSock; } }
}   // namespace

std::vector<IfaceAddr> privateInterfaces() {
    std::vector<IfaceAddr> v;
#ifdef _WIN32
    ULONG len = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &len);
    if (len == 0 || len > (4u << 20)) return v;
    std::vector<unsigned char> buf(len);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len) != NO_ERROR) return v;
    for (; aa; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK || aa->IfType == IF_TYPE_TUNNEL || (aa->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = aa->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            IfaceAddr f;
            f.ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            const unsigned bits = u->OnLinkPrefixLength;
            if (bits < 8 || bits > 30) continue;
            f.mask = 0xFFFFFFFFu << (32 - bits);
            if (isPrivateIpv4(f.ip) && (f.ip >> 24) != 127) v.push_back(f);
        }
    }
#else
    ifaddrs* l = nullptr;
    if (getifaddrs(&l) != 0) return v;
    for (ifaddrs* a = l; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_netmask || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) || !(a->ifa_flags & IFF_MULTICAST)) continue;
        IfaceAddr f;
        f.ip = ntohl(((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr);
        f.mask = ntohl(((sockaddr_in*)a->ifa_netmask)->sin_addr.s_addr);
        if (isPrivateIpv4(f.ip) && (f.ip >> 24) != 127 && onLocalSubnet(f.ip, f.ip, f.mask)) v.push_back(f);   // never a public address
    }
    freeifaddrs(l);
#endif
    return v;
}

struct SwarmNet::Impl {
    std::mutex mu;
    std::thread th;
    std::atomic<bool> stop{false};
    NodeTable table;
    NetStats st;
    sock_t rx = kBadSock, tx = kBadSock;
    std::vector<IfaceAddr> ifs;
    AnnounceFn announce;

    void closeAll() { closeSock(rx); closeSock(tx); }
    void run() {
        int64_t nextSend = 0;
        while (!stop) {
            int64_t now = nowMs();
            if (now >= nextSend) {
                nextSend = now + 5000;
                Announce a = announce ? announce() : Announce();
                std::string pkt = encodeAnnounce(a);
                for (auto& f : ifs) {
                    in_addr ia; ia.s_addr = htonl(f.ip);
                    if (setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia) != 0) continue;
                    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(kPort); inet_pton(AF_INET, kGroup, &to.sin_addr);
                    if (sockSendTo(tx, pkt.data(), pkt.size(), to) > 0) { std::lock_guard<std::mutex> g(mu); st.sent++; }
                }
                std::lock_guard<std::mutex> g(mu);
                table.expire(now);
            }
            fd_set rd;
            FD_ZERO(&rd); FD_SET(rx, &rd);
            timeval tv{0, 500000};
            if (select((int)rx + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t buf[kMaxAnnounceBytes + 64];
            sockaddr_in from{}; socklen_t fl = sizeof from;
#ifdef _WIN32
            int n = recvfrom(rx, (char*)buf, (int)sizeof buf, 0, (sockaddr*)&from, &fl);
#else
            ssize_t n = recvfrom(rx, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
#endif
            if (n <= 0) continue;
            uint32_t src = ntohl(from.sin_addr.s_addr);
            Announce a;
            std::string err;
            bool ok = n <= (long long)kMaxAnnounceBytes && parseAnnounce(buf, (size_t)n, a, err);
            std::lock_guard<std::mutex> g(mu);
            if (!ok) { st.rejected++; continue; }
            bool took = false, own = false;
            for (auto& f : ifs) if (src == f.ip) own = true;
            if (own) continue;   // our own packet looped back
            for (auto& f : ifs) {
                NodeTable::Result r = table.update(a, src, f.ip, f.mask, nowMs());
                if (r == NodeTable::WrongSubnet) continue;   // try the next interface
                took = (r == NodeTable::Accepted);
                break;
            }
            if (took) st.received++; else st.rejected++;
        }
    }
};

SwarmNet::SwarmNet() : p_(new Impl) {}
SwarmNet::~SwarmNet() { stop(); }

bool SwarmNet::start(const std::string& ownId, AnnounceFn announce, std::string& err) {
    stop();
    Impl& d = *p_;
    netInit();
    d.ifs = privateInterfaces();
    if (d.ifs.empty()) { err = "no private network interface found (LAN swarm stays off)"; return false; }
    d.rx = socket(AF_INET, SOCK_DGRAM, 0);
    d.tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (d.rx == kBadSock || d.tx == kBadSock) { err = "cannot open sockets"; d.closeAll(); return false; }
    int one = 1;
    setsockopt(d.rx, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    setsockopt(d.rx, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof one);
#endif
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_port = htons(kPort); me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(d.rx, (sockaddr*)&me, sizeof me) != 0) { err = "cannot listen on the swarm port"; d.closeAll(); return false; }
    int joined = 0;
    for (auto& f : d.ifs) {
        ip_mreq m{};
        inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
        m.imr_interface.s_addr = htonl(f.ip);
        if (setsockopt(d.rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) == 0) joined++;
    }
    if (!joined) { err = "cannot join the multicast group"; d.closeAll(); return false; }
#ifdef _WIN32
    DWORD ttl = 1, loop = 0;
#else
    unsigned char ttl = 1, loop = 0;
#endif
    setsockopt(d.tx, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);    // TTL 1: never leaves the local network
    setsockopt(d.tx, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
    {
        std::lock_guard<std::mutex> g(d.mu);
        d.table = NodeTable(ownId);
        d.st = NetStats();
        d.st.running = true;
        d.st.interfaces = (int)d.ifs.size();
    }
    d.announce = std::move(announce);
    d.stop = false;
    d.th = std::thread([&d] { d.run(); });
    return true;
}

void SwarmNet::stop() {
    Impl& d = *p_;
    d.stop = true;
    if (d.th.joinable()) d.th.join();
    d.closeAll();
    std::lock_guard<std::mutex> g(d.mu);
    d.st.running = false;
}

std::vector<Node> SwarmNet::nodes() {
    std::lock_guard<std::mutex> g(p_->mu);
    return p_->table.list();
}
NetStats SwarmNet::stats() const {
    std::lock_guard<std::mutex> g(p_->mu);
    return p_->st;
}

}   // namespace dect2::swarm
