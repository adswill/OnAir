// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/ts_udp_in.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace dect2 {

struct TsUdpBuffer::Impl {
    mutable std::mutex mu;
    std::vector<uint8_t> acc;                       // stream bytes not yet turned into packets
    std::deque<std::array<uint8_t, 188>> q;
    size_t cap, prefill;
    bool filling = true;                            // before the first output (and after an underrun) the buffer fills to `prefill` first
    bool synced = false;
    TsUdpStats st;
    Impl(size_t c, size_t p) : cap(std::max<size_t>(c, 16)), prefill(std::min(p, std::max<size_t>(c, 16) / 2)) {}

    void push(const uint8_t* pk) {
        std::array<uint8_t, 188> a;
        memcpy(a.data(), pk, 188);
        if (q.size() >= cap) { q.pop_front(); st.dropped++; }
        q.push_back(a);
        st.packets++;
    }
    // sync byte three packets in a row (or fewer at the end of the data we have)
    bool alignedAt(size_t i, bool final) const {
        if (acc[i] != 0x47) return false;
        for (size_t k = 1; k <= 2; k++) {
            const size_t j = i + 188 * k;
            if (j < acc.size()) { if (acc[j] != 0x47) return false; }
            else if (!final) return false;            // cannot tell yet: wait for more bytes
            else break;
        }
        return true;
    }
    void drain() {
        size_t pos = 0;
        while (acc.size() - pos >= 188) {
            if (synced) {
                if (acc[pos] == 0x47) { push(&acc[pos]); pos += 188; continue; }
                synced = false; st.resyncs++;
            }
            // search: need the next two sync bytes in view to trust one (up to 3 packets of data)
            if (acc.size() - pos < 188 * 3) break;
            size_t i = pos;
            bool found = false;
            for (; i + 188 * 3 <= acc.size(); i++) if (alignedAt(i, false)) { found = true; break; }
            if (!found) { st.junkBytes += i - pos; pos = i; break; }
            st.junkBytes += i - pos;
            pos = i;
            synced = true;
        }
        acc.erase(acc.begin(), acc.begin() + (long)pos);
        if (acc.size() > 188 * 64) { st.junkBytes += acc.size() - 188 * 3; acc.erase(acc.begin(), acc.end() - 188 * 3); }
    }
};

TsUdpBuffer::TsUdpBuffer(size_t capacityPackets, size_t prefillPackets) : p_(std::make_shared<Impl>(capacityPackets, prefillPackets)) { p_->st.capacity = p_->cap; }

void TsUdpBuffer::feedBytes(const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->acc.insert(p_->acc.end(), d, d + n);
    p_->drain();
}

void TsUdpBuffer::feedDatagram(const uint8_t* d, size_t n) {
    if (!d || n == 0) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->st.datagrams++;
    }
    // RTP: version 2 (a TS datagram starts with the sync byte 0x47, whose top bits are 01), payload type 33 (MP2T)
    if (n >= 12 && (d[0] >> 6) == 2 && (d[1] & 0x7F) == 33) {
        size_t off = 12 + 4 * (size_t)(d[0] & 15);
        if (d[0] & 0x10) { if (off + 4 <= n) off += 4 + 4 * (size_t)((d[off + 2] << 8) | d[off + 3]); else off = n; }
        size_t end = n;
        if ((d[0] & 0x20) && n > off) end = n - std::min<size_t>(d[n - 1], n - off);   // padding
        if (off < end) {
            { std::lock_guard<std::mutex> lk(p_->mu); p_->st.rtpDatagrams++; }
            feedBytes(d + off, end - off);
        }
        return;
    }
    feedBytes(d, n);
}

void TsUdpBuffer::pull(uint8_t* pk) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (p_->filling && p_->q.size() >= std::max<size_t>(p_->prefill, 1)) p_->filling = false;
        if (!p_->filling && !p_->q.empty()) {
            memcpy(pk, p_->q.front().data(), 188);
            p_->q.pop_front();
            return;
        }
        if (!p_->filling) p_->filling = true;           // ran dry: let it fill a little again, so one late datagram is not followed by another gap
        p_->st.nullsInserted++;
    }
    pk[0] = 0x47; pk[1] = 0x1F; pk[2] = 0xFF; pk[3] = 0x10;
    memset(pk + 4, 0xFF, 184);
}

TsUdpStats TsUdpBuffer::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    TsUdpStats s = p_->st;
    s.queued = p_->q.size();
    return s;
}

// ---- address ---------------------------------------------------------------------------------------------------------------------

bool isTsUdpSpec(const std::string& s) { return s.rfind("udp://", 0) == 0 || s.rfind("rtp://", 0) == 0; }

bool parseTsUdpSpec(const std::string& spec, std::string& host, int& port) {
    if (!isTsUdpSpec(spec)) return false;
    std::string r = spec.substr(6);
    const size_t q = r.find('?');
    if (q != std::string::npos) r.resize(q);
    if (!r.empty() && r[0] == '@') r.erase(0, 1);
    const size_t c = r.rfind(':');
    if (c == std::string::npos) return false;
    host = r.substr(0, c);
    if (host.empty()) host = "0.0.0.0";
    const std::string ps = r.substr(c + 1);
    if (ps.empty() || ps.size() > 5 || ps.find_first_not_of("0123456789") != std::string::npos) return false;
    port = atoi(ps.c_str());
    if (port < 1 || port > 65535) return false;
    in_addr a{};
    return inet_pton(AF_INET, host.c_str(), &a) == 1;
}

// ---- listener --------------------------------------------------------------------------------------------------------------------

namespace {
struct Listener {
    TsUdpBuffer buf;
    sock_t sock = kBadSock;
    std::atomic<bool> stop{false};
    std::thread th;
    std::string address;
    Listener() : buf(6000, 64) {}
    ~Listener() {
        stop = true;
        if (th.joinable()) th.join();
        if (sock != kBadSock) sockClose(sock);
    }
};
std::mutex gMu;
std::weak_ptr<Listener> gLast;
}

bool tsUdpOpen(const std::string& spec, std::function<void(uint8_t*)>& out, std::string& err) {
    out = {};
    std::string host; int port = 0;
    if (!parseTsUdpSpec(spec, host, port)) { err = "the UDP address is not valid: use udp://@:1234 or udp://239.1.1.1:1234"; return false; }
    netInit();
    auto L = std::make_shared<Listener>();
    L->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (L->sock == kBadSock) { err = "cannot open a UDP socket"; return false; }
    int one = 1;
    setsockopt(L->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    setsockopt(L->sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof one);
#endif
    int rb = 4 << 20;
    setsockopt(L->sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
#ifdef _WIN32
    DWORD tmo = 100;
    setsockopt(L->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#else
    timeval tmo{0, 100000};
    setsockopt(L->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#endif
    in_addr ia{};
    inet_pton(AF_INET, host.c_str(), &ia);
    const bool multicast = (ntohl(ia.s_addr) >> 28) == 0xE;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : ia.s_addr;
    if (bind(L->sock, (sockaddr*)&a, sizeof a) != 0) {
        err = "cannot listen on UDP port " + std::to_string(port) + " (is another program using it?)";
        return false;
    }
    if (multicast) {
        ip_mreq m{};
        m.imr_multiaddr = ia;
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(L->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) != 0) { err = "cannot join the multicast group " + host; return false; }
    }
    L->address = host + ":" + std::to_string(port) + (multicast ? " (multicast)" : "");
    Listener* lp = L.get();
    lp->th = std::thread([lp] {
        std::vector<uint8_t> b(65536);
        while (!lp->stop) {
            const auto n = recv(lp->sock, (char*)b.data(), (int)b.size(), 0);
            if (n > 0) lp->buf.feedDatagram(b.data(), (size_t)n);
        }
    });
    { std::lock_guard<std::mutex> lk(gMu); gLast = L; }
    out = [L](uint8_t* pk) { L->buf.pull(pk); };
    return true;
}

TsUdpStats tsUdpLastStats() {
    std::shared_ptr<Listener> L;
    { std::lock_guard<std::mutex> lk(gMu); L = gLast.lock(); }
    if (!L) return {};
    TsUdpStats s = L->buf.stats();
    s.running = true;
    s.address = L->address;
    return s;
}

} // namespace dect2
