// The link between the Digital video generator and OnAir (vglink.h, docs/VIDEOGEN-LINK.md): RTP and RTCP over UDP.
#include "dect2/tv/vglink.h"
#include "../net/netcompat.h"
#include "dect2/common/thread_name.h"
#include "dect2/tv/videogen.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#ifndef _WIN32
#include <netdb.h>
#include <sys/select.h>
#include <arpa/inet.h>
#endif

namespace dect2::vgen {

// ---- packets: RTP and RTCP (RFC 3550) ----------------------------------------------------------------------------------------

namespace {
constexpr uint8_t kRtcpRr = 201, kRtcpSdes = 202, kRtcpBye = 203, kRtcpApp = 204;
constexpr uint8_t kPtMp2t = 33;            // RFC 3551: MPEG-2 transport stream
constexpr size_t kTsPerRtp = 7;            // RFC 2250: whole TS packets, 7 x 188 = 1316 bytes
const char kAppName[4] = {'O', 'A', 'V', 'G'};

void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xFFFF); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void pad4(std::vector<uint8_t>& v, size_t from) { while ((v.size() - from) % 4) v.push_back(0); }
// an RTCP packet's length field: its size in 32-bit words minus one
void setLength(std::vector<uint8_t>& v, size_t at) { const size_t w = (v.size() - at) / 4 - 1; v[at + 2] = (uint8_t)(w >> 8); v[at + 3] = (uint8_t)w; }
void header(std::vector<uint8_t>& v, uint8_t count, uint8_t pt) { v.push_back((uint8_t)(0x80 | (count & 0x1F))); v.push_back(pt); put16(v, 0); }
} // namespace

std::vector<uint8_t> vgRtcp(VgMsg type, const std::string& text, uint32_t ssrc, const std::string& cname, const std::string& name) {
    std::vector<uint8_t> p;
    header(p, 0, kRtcpRr);   // a compound packet starts with SR or RR (we receive nothing to report on)
    put32(p, ssrc);
    setLength(p, 0);
    size_t at = p.size();
    header(p, 1, kRtcpSdes);
    put32(p, ssrc);
    const std::string c = cname.substr(0, 255), nm = name.substr(0, 255);
    p.push_back(1); p.push_back((uint8_t)c.size()); p.insert(p.end(), c.begin(), c.end());
    if (!nm.empty()) { p.push_back(2); p.push_back((uint8_t)nm.size()); p.insert(p.end(), nm.begin(), nm.end()); }
    p.push_back(0);
    pad4(p, at);
    setLength(p, at);
    at = p.size();
    if (type == VgMsg::Bye) {
        header(p, 1, kRtcpBye);
        put32(p, ssrc);
        const std::string r = text.substr(0, 255);
        p.push_back((uint8_t)r.size());
        p.insert(p.end(), r.begin(), r.end());
    } else {
        header(p, (uint8_t)type, kRtcpApp);
        put32(p, ssrc);
        p.insert(p.end(), kAppName, kAppName + 4);
        p.insert(p.end(), text.begin(), text.end());   // padded with NUL to 32 bits
    }
    pad4(p, at);
    setLength(p, at);
    return p;
}

std::vector<uint8_t> vgRtp(const uint8_t* ts, size_t n, uint16_t seq, uint32_t ts90k, uint32_t ssrc) {
    n = std::min(n, kTsPerRtp);
    std::vector<uint8_t> p;
    p.reserve(12 + n * 188);
    p.push_back(0x80);           // V=2
    p.push_back(kPtMp2t);
    put16(p, seq);
    put32(p, ts90k);
    put32(p, ssrc);
    p.insert(p.end(), ts, ts + n * 188);
    return p;
}

bool vgParseRtp(const uint8_t* p, size_t n, VgPacket& out) {
    if (n < 12 || (p[0] >> 6) != 2 || (p[1] & 0x7F) != kPtMp2t) return false;
    size_t h = 12 + 4 * (size_t)(p[0] & 0x0F);   // the CSRC list
    if (h > n) return false;
    if (p[0] & 0x10) {   // a header extension
        if (h + 4 > n) return false;
        h += 4 + 4 * (size_t)((p[h + 2] << 8) | p[h + 3]);
        if (h > n) return false;
    }
    size_t end = n;
    if (p[0] & 0x20) {   // padding
        if (p[n - 1] > n - h) return false;
        end = n - p[n - 1];
    }
    out.type = VgMsg::Ts;
    out.seq = (uint16_t)((p[2] << 8) | p[3]);
    out.ssrc = get32(p + 8);
    out.data.assign(p + h, p + h + (end - h) / 188 * 188);
    return true;
}

bool vgParseRtcp(const uint8_t* p, size_t n, std::vector<VgPacket>& out) {
    std::vector<VgPacket> got;
    size_t at = 0;
    while (at + 4 <= n) {
        const uint8_t* q = p + at;
        const size_t len = ((size_t)((q[2] << 8) | q[3]) + 1) * 4;
        if ((q[0] >> 6) != 2 || q[1] < 200 || q[1] > 206 || at + len > n) return false;
        if (q[1] == kRtcpApp && len >= 12 && std::equal(kAppName, kAppName + 4, q + 8)) {
            size_t e = len;
            while (e > 12 && q[e - 1] == 0) e--;   // the NUL padding
            VgPacket m;
            m.type = (VgMsg)(q[0] & 0x1F);
            m.ssrc = get32(q + 4);
            m.data.assign(q + 12, q + e);
            got.push_back(std::move(m));
        } else if (q[1] == kRtcpBye && len >= 8) {
            const size_t sc = q[0] & 0x1F, r = 4 + 4 * sc;
            VgPacket m;
            m.type = VgMsg::Bye;
            m.ssrc = get32(q + 4);
            if (r < len && r + 1 + (size_t)q[r] <= len) m.data.assign(q + r + 1, q + r + 1 + q[r]);
            got.push_back(std::move(m));
        }   // SR, RR, SDES and other applications' APP packets: nothing for us
        at += len;
    }
    if (at != n) return false;
    out.insert(out.end(), got.begin(), got.end());
    return true;
}

std::string vgKvText(const VgKv& kv) {
    std::string o;
    for (const auto& [k, v] : kv) {
        std::string val = v;
        for (char& c : val) if (c == '\n' || c == '\r') c = ' ';
        o += k + "=" + val + "\n";
    }
    return o;
}

VgKv vgKvParse(const std::string& text) {
    VgKv kv;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

double vgKvNum(const VgKv& kv, const char* key, double def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    char* end = nullptr;
    const double v = strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() && std::isfinite(v) ? v : def;
}

std::string vgKvStr(const VgKv& kv, const char* key, const std::string& def) {
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

// ---- transmitter info -------------------------------------------------------------------------------------------------------

double vgUsableBps(const VgTxInfo& t, int slot) {
    if (t.channelBps <= 0) return 0;
    const size_t n = t.slots.size();
    const bool in = std::find(t.slots.begin(), t.slots.end(), slot) != t.slots.end();
    return t.channelBps * 0.97 / (double)(in && n > 0 ? n : 1);
}

VgKv vgTxInfoKv(const VgTxInfo& t, int slot) {
    VgKv kv;
    std::array<char, 48> b{};
    kv["running"] = t.running ? "1" : "0";
    kv["tv"] = t.digitalTv ? "1" : "0";
    snprintf(b.data(), b.size(), "%.0f", t.channelBps);
    kv["channel"] = b.data();
    snprintf(b.data(), b.size(), "%.0f", vgUsableBps(t, slot));
    kv["usable"] = b.data();
    kv["system"] = t.system;
    kv["params"] = t.params;
    kv["generators"] = std::to_string(t.slots.size());
    kv["inmux"] = std::find(t.slots.begin(), t.slots.end(), slot) != t.slots.end() ? "1" : "0";
    return kv;
}

VgTxInfo vgTxInfoFromKv(const VgKv& kv, double& usableBps) {
    VgTxInfo t;
    t.running = vgKvNum(kv, "running") != 0;
    t.digitalTv = vgKvNum(kv, "tv") != 0;
    t.channelBps = std::max(0.0, vgKvNum(kv, "channel"));
    t.system = vgKvStr(kv, "system");
    t.params = vgKvStr(kv, "params");
    const int n = std::clamp((int)vgKvNum(kv, "generators"), 0, kMaxInstances);
    for (int i = 0; i < n; i++) t.slots.push_back(0);   // how many share the channel (their numbers are OnAir's business)
    usableBps = std::max(0.0, vgKvNum(kv, "usable"));
    return t;
}

// ---- sockets ----------------------------------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

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

// one datagram; -1 when none (or an error: on Windows a former ICMP "port unreachable" shows up here and is harmless)
long long recvFrom(sock_t s, std::vector<uint8_t>& buf, sockaddr_in& from) {
    socklen_t fl = sizeof from;
    const auto r = recvfrom(s, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    return r < 0 || (size_t)r > buf.size() ? -1 : (long long)r;
}

std::string hostOf(const sockaddr_in& a) {
    std::array<char, 64> b{};
    inet_ntop(AF_INET, &a.sin_addr, b.data(), (socklen_t)b.size());
    return b.data();
}

std::string text(const std::vector<uint8_t>& d) { return std::string(d.begin(), d.end()); }

uint32_t newSsrc(const void* self) {
    std::random_device rd;
    return rd() ^ (uint32_t)Clock::now().time_since_epoch().count() ^ (uint32_t)(reinterpret_cast<uintptr_t>(self) >> 4);
}

} // namespace

// ---- OnAir side -------------------------------------------------------------------------------------------------------------

struct VgLinkServer::Impl {
    struct Peer {
        uint32_t ssrc = 0;
        VgPeer info;
        sockaddr_in rtcp{};        // where its State came from: the Reply goes there
        uint32_t ip = 0;
        Clock::time_point seen;
        int stream = -1;
        bool haveSeq = false;
        uint16_t seq = 0;
        int raise = 0, quit = 0;
    };
    mutable std::mutex mu;
    sock_t rtp = kBadSock, rtcp = kBadSock;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::vector<Peer> peers;
    VgTxInfo tx;
    std::function<void(const std::string&)>* onBye = nullptr;

    Peer* find(uint32_t ssrc) { for (auto& p : peers) if (p.ssrc == ssrc) return &p; return nullptr; }
    int freeSlot(int want) const {
        auto used = [&](int k) { for (const auto& p : peers) if (p.info.slot == k) return true; return false; };
        if (want >= 1 && want <= kMaxInstances && !used(want)) return want;
        for (int k = 1; k <= kMaxInstances; k++) if (!used(k)) return k;
        return 0;
    }
    void send(const sockaddr_in& to, VgMsg t, const VgKv& kv) {
        const std::vector<uint8_t> d = vgRtcp(t, vgKvText(kv), 0, "onair");
        sockSendTo(rtcp, d.data(), d.size(), to);
    }
    void reply(const Peer& p) {   // under mu
        VgKv kv = vgTxInfoKv(tx, p.info.slot);
        kv["proto"] = std::to_string(kVgLinkVersion);
        kv["slot"] = std::to_string(p.info.slot);
        kv["raise"] = std::to_string(p.raise);
        kv["quit"] = std::to_string(p.quit);
        send(p.rtcp, VgMsg::Reply, kv);
    }
    void drop(size_t i) {   // under mu
        generatorSetRate(peers[i].info.slot, 0);
        generatorClear(peers[i].info.slot);
        peers.erase(peers.begin() + (std::ptrdiff_t)i);
    }
    void onState(uint32_t ssrc, const VgKv& kv, const sockaddr_in& from);
    void onRtp(const VgPacket& m, const sockaddr_in& from);
    void run();
};

void VgLinkServer::Impl::onState(uint32_t ssrc, const VgKv& kv, const sockaddr_in& from) {
    std::lock_guard<std::mutex> lk(mu);
    Peer* p = find(ssrc);
    if (!p) {
        if ((int)vgKvNum(kv, "proto", 0) < 1) return;
        const int slot = freeSlot((int)vgKvNum(kv, "slot", 1));
        if (slot == 0) {
            send(from, VgMsg::Refused, {{"error", "OnAir already has " + std::to_string(kMaxInstances) + " generators linked"}});
            return;
        }
        Peer n;
        n.ssrc = ssrc;
        n.info.slot = slot;
        n.info.host = hostOf(from);
        n.ip = from.sin_addr.s_addr;
        generatorSetRate(slot, 0);
        generatorClear(slot);
        peers.push_back(n);
        p = &peers.back();
    }
    p->rtcp = from;
    p->seen = Clock::now();
    p->info.name = vgKvStr(kv, "name").substr(0, 60);
    p->info.token = vgKvStr(kv, "token").substr(0, 80);
    p->info.easy = vgKvNum(kv, "easy", 1) != 0;
    p->info.programmes.clear();
    const int np = std::clamp((int)vgKvNum(kv, "programmes"), 0, 16);
    for (int i = 0; i < np; i++) p->info.programmes.push_back(vgKvStr(kv, ("p" + std::to_string(i)).c_str()).substr(0, 60));
    const double rate = std::max(0.0, vgKvNum(kv, "rate"));
    const int stream = (int)vgKvNum(kv, "stream", 0);
    if (rate > 0 && stream != p->stream) { generatorClear(p->info.slot); p->haveSeq = false; }   // a starting generator empties its queue (#494)
    p->stream = stream;
    if (rate != p->info.rateBps) generatorSetRate(p->info.slot, rate);
    p->info.rateBps = rate;
    reply(*p);
}

void VgLinkServer::Impl::onRtp(const VgPacket& m, const sockaddr_in& from) {
    int slot = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        Peer* p = find(m.ssrc);
        if (!p || p->ip != from.sin_addr.s_addr || p->info.rateBps <= 0) return;   // not one of ours, or not sending (a stale datagram)
        if (p->haveSeq) {
            const int16_t d = (int16_t)(uint16_t)(m.seq - p->seq);
            if (d <= 0) { p->info.late++; return; }   // late or twice: the queue only goes forward
            p->info.lost += (uint64_t)(d - 1);
        }
        p->haveSeq = true;
        p->seq = m.seq;
        p->info.packets += m.data.size() / 188;
        slot = p->info.slot;
    }
    if (!m.data.empty()) generatorPush(slot, m.data.data(), m.data.size() / 188);
}

void VgLinkServer::Impl::run() {
    std::vector<uint8_t> buf(65536);
    std::vector<VgPacket> msgs;
    auto lastReply = Clock::now();
    while (!stop) {
        const int w = waitReadable(rtp, rtcp, 100);
        sockaddr_in from{};
        if (w > 0 && (w & 1)) {
            for (int i = 0; i < 256; i++) {   // drain the RTP socket first: it carries the stream
                const long long n = recvFrom(rtp, buf, from);
                if (n < 0) break;
                VgPacket m;
                if (vgParseRtp(buf.data(), (size_t)n, m)) onRtp(m, from);
                if (waitReadable(rtp, kBadSock, 0) <= 0) break;
            }
        }
        if (w > 0 && (w & 2)) {
            const long long n = recvFrom(rtcp, buf, from);
            msgs.clear();
            if (n > 0 && vgParseRtcp(buf.data(), (size_t)n, msgs)) {
                for (const VgPacket& m : msgs) {
                    if (m.type == VgMsg::State) onState(m.ssrc, vgKvParse(text(m.data)), from);
                    else if (m.type == VgMsg::Bye) {
                        std::string tok;
                        bool closed = false;
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            for (size_t i = 0; i < peers.size(); i++)
                                if (peers[i].ssrc == m.ssrc && peers[i].ip == from.sin_addr.s_addr) { tok = peers[i].info.token; closed = text(m.data) == "closed"; drop(i); break; }
                        }
                        if (closed && onBye && *onBye) (*onBye)(tok);
                    }
                }
            }
        }
        const auto now = Clock::now();
        if (now - lastReply >= std::chrono::seconds(1)) {
            lastReply = now;
            std::lock_guard<std::mutex> lk(mu);
            for (size_t i = peers.size(); i-- > 0;) {
                if (now - peers[i].seen > std::chrono::duration<double>(kVgLinkTimeoutSec)) drop(i);   // gone without a BYE
                else reply(peers[i]);
            }
        }
    }
}

VgLinkServer::VgLinkServer() : p_(std::make_unique<Impl>()) { p_->onBye = &onBye; }
VgLinkServer::~VgLinkServer() { stop(); }

bool VgLinkServer::start(int port, bool anyAddress, std::string& err) {
    stop();
    netInit();
    const uint32_t addr = anyAddress ? INADDR_ANY : INADDR_LOOPBACK;
    constexpr int kRcvBuf = 4 << 20;   // a few hundred ms of eight generators, should the thread be held up
    sock_t a = kBadSock, b = kBadSock;
    if (port > 0) {
        if (port % 2 || port >= 65535) { err = "the port must be even (RTP; RTCP uses the next one)"; return false; }
        a = udpSocket(addr, port, kRcvBuf);
        b = a == kBadSock ? kBadSock : udpSocket(addr, port + 1, 0);
    } else {
        for (int tries = 0; tries < 40 && b == kBadSock; tries++) {   // a free pair: the system's choice for RTP, rounded to even
            if (a != kBadSock) sockClose(a);
            const sock_t t = udpSocket(addr, 0, 0);
            if (t == kBadSock) break;
            int p = boundPort(t) & ~1;
            sockClose(t);
            if (p < 1024 || p >= 65534) continue;
            a = udpSocket(addr, p, kRcvBuf);
            if (a != kBadSock) b = udpSocket(addr, p + 1, 0);
        }
    }
    if (a == kBadSock || b == kBadSock) {
        if (a != kBadSock) sockClose(a);
        err = port > 0 ? "UDP port " + std::to_string(port) + " or " + std::to_string(port + 1) + " is in use (another OnAir?)" : "no free UDP ports";
        return false;
    }
    p_->rtp = a;
    p_->rtcp = b;
    p_->port = boundPort(a);
    p_->stop = false;
    p_->th = namedThread("vg-link", [this] { p_->run(); });
    return true;
}

void VgLinkServer::stop() {
    p_->stop = true;
    if (p_->th.joinable()) p_->th.join();
    std::lock_guard<std::mutex> lk(p_->mu);
    while (!p_->peers.empty()) p_->drop(p_->peers.size() - 1);
    if (p_->rtp != kBadSock) { sockClose(p_->rtp); p_->rtp = kBadSock; }
    if (p_->rtcp != kBadSock) { sockClose(p_->rtcp); p_->rtcp = kBadSock; }
    p_->port = 0;
}

bool VgLinkServer::listening() const { return p_->rtp != kBadSock; }
int VgLinkServer::port() const { return p_->port; }

std::vector<VgPeer> VgLinkServer::peers() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<VgPeer> v;
    for (const auto& p : p_->peers) v.push_back(p.info);
    std::sort(v.begin(), v.end(), [](const VgPeer& a, const VgPeer& b) { return a.slot < b.slot; });
    return v;
}

void VgLinkServer::setTxInfo(const VgTxInfo& t) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const auto same = [](const VgTxInfo& a, const VgTxInfo& b) {
        return a.running == b.running && a.digitalTv == b.digitalTv && std::fabs(a.channelBps - b.channelBps) < 1 && a.system == b.system && a.params == b.params && a.slots == b.slots;
    };
    if (same(t, p_->tx)) return;
    p_->tx = t;
    if (p_->rtcp != kBadSock) for (const auto& p : p_->peers) p_->reply(p);
}

static bool bumpByToken(VgLinkServer::Impl& I, const std::string& token, bool quit) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(I.mu);
    bool any = false;
    for (auto& p : I.peers)
        if (p.info.token == token) { (quit ? p.quit : p.raise)++; I.reply(p); any = true; }
    return any;
}

bool VgLinkServer::raise(const std::string& token) { return bumpByToken(*p_, token, false); }
bool VgLinkServer::quit(const std::string& token) { return bumpByToken(*p_, token, true); }
void VgLinkServer::quitAll() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& p : p_->peers) { p.quit++; p_->reply(p); }
}

VgLinkServer& vgLinkServer() {
    static VgLinkServer s;
    return s;
}

// ---- generator side ---------------------------------------------------------------------------------------------------------

struct VgLinkClient::Impl {
    mutable std::mutex mu;          // the state below
    std::mutex rtpMu;               // the sequence number
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> dirty{false}; // send the State now
    sock_t s = kBadSock;
    sockaddr_in rtpTo{}, rtcpTo{};
    std::atomic<bool> haveAddr{false};
    std::string host, name, token;
    int port = 0, wantSlot = 1, slot = 0;
    std::atomic<bool> linked{false};
    Clock::time_point lastReply;
    std::string status;
    VgTxInfo tx;
    double usable = 0;
    bool easy = true;
    std::vector<std::string> progs;
    double rate = 0;                // > 0: streaming
    int stream = 0;                 // counts the starts
    int raiseSeen = -1, quitSeen = -1;
    std::atomic<bool> raise{false}, quit{false};
    std::atomic<uint64_t> sent{0};
    uint32_t ssrc = 0;
    uint16_t seq = 0;

    std::string stateText() {   // under mu
        VgKv kv{{"proto", std::to_string(kVgLinkVersion)}, {"name", name}, {"slot", std::to_string(wantSlot)}, {"token", token},
                {"rate", std::to_string((long long)rate)}, {"stream", std::to_string(stream)}, {"easy", easy ? "1" : "0"},
                {"programmes", std::to_string(std::min<size_t>(progs.size(), 16))}};
        for (size_t i = 0; i < progs.size() && i < 16; i++) kv["p" + std::to_string(i)] = progs[i].substr(0, 60);
        return vgKvText(kv);
    }
    void sendRtcp(VgMsg t, const std::string& txt) {
        std::string nm;
        { std::lock_guard<std::mutex> lk(mu); nm = name; }
        const std::vector<uint8_t> d = vgRtcp(t, txt, ssrc, "onair-videogen", nm);
        if (haveAddr) sockSendTo(s, d.data(), d.size(), rtcpTo);
    }
    bool resolve(const std::string& h, int p) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        sockaddr_in a{};
        std::copy_n(reinterpret_cast<const uint8_t*>(res->ai_addr), std::min(sizeof a, (size_t)res->ai_addrlen), reinterpret_cast<uint8_t*>(&a));
        freeaddrinfo(res);
        a.sin_port = htons((uint16_t)p);
        rtpTo = a;
        a.sin_port = htons((uint16_t)(p + 1));
        rtcpTo = a;
        return true;
    }
    void run();
};

void VgLinkClient::Impl::run() {
    std::vector<uint8_t> buf(65536);
    std::vector<VgPacket> msgs;
    std::string h;
    int p = 0;
    { std::lock_guard<std::mutex> lk(mu); h = host; p = port; }
    const std::string where = h + ":" + std::to_string(p);
    auto lastState = Clock::now() - std::chrono::seconds(10), lastResolve = lastState;
    bool refused = false;
    while (!stop) {
        auto now = Clock::now();
        if (!haveAddr && now - lastResolve > std::chrono::seconds(2)) {   // a name that does not resolve yet (no network): try again
            lastResolve = now;
            if (resolve(h, p)) haveAddr = true;
            else { std::lock_guard<std::mutex> lk(mu); status = "cannot find the computer " + h + " (trying again)"; }
        }
        if (haveAddr && (dirty.exchange(false) || now - lastState >= std::chrono::milliseconds(refused ? 3000 : 1000))) {
            lastState = now;
            std::string st;
            { std::lock_guard<std::mutex> lk(mu); st = stateText(); }
            sendRtcp(VgMsg::State, st);
        }
        if (waitReadable(s, kBadSock, 50) > 0) {
            sockaddr_in from{};
            const long long n = recvFrom(s, buf, from);
            msgs.clear();
            const bool fromOnAir = n > 0 && from.sin_addr.s_addr == rtcpTo.sin_addr.s_addr && from.sin_port == rtcpTo.sin_port;
            if (fromOnAir && vgParseRtcp(buf.data(), (size_t)n, msgs)) {
                for (const VgPacket& m : msgs) {
                    const VgKv kv = vgKvParse(text(m.data));
                    std::lock_guard<std::mutex> lk(mu);
                    if (m.type == VgMsg::Reply) {
                        refused = false;
                        slot = (int)vgKvNum(kv, "slot");
                        tx = vgTxInfoFromKv(kv, usable);
                        lastReply = Clock::now();
                        if (!linked) status = "linked to OnAir at " + where + " as generator:" + std::to_string(slot);
                        linked = true;
                        const int r = (int)vgKvNum(kv, "raise"), q = (int)vgKvNum(kv, "quit");
                        if (raiseSeen >= 0 && r > raiseSeen) raise = true;   // the first Reply only tells where the counters are
                        if (quitSeen >= 0 && q > quitSeen) quit = true;
                        raiseSeen = r; quitSeen = q;
                    } else if (m.type == VgMsg::Refused) {
                        refused = true;
                        linked = false;
                        slot = 0;
                        status = "OnAir refused the link: " + vgKvStr(kv, "error");
                    }
                }
            }
        }
        now = Clock::now();
        std::lock_guard<std::mutex> lk(mu);
        if (linked && now - lastReply > std::chrono::seconds(3)) {
            linked = false;
            slot = 0;
            tx = VgTxInfo();
            usable = 0;
            raiseSeen = quitSeen = -1;   // OnAir may have restarted: its counters start again
            status = "OnAir does not answer at " + where + " (trying again)";
        } else if (!linked && !refused && haveAddr && now - lastState < std::chrono::milliseconds(60) && status.compare(0, 6, "linked") != 0 && status.compare(0, 12, "OnAir does n") != 0)
            status = "looking for OnAir at " + where;
    }
}

VgLinkClient::VgLinkClient() : p_(std::make_unique<Impl>()) {
    p_->ssrc = newSsrc(this);   // a random synchronisation source (RFC 3550)
    p_->seq = (uint16_t)(p_->ssrc >> 16);
}
VgLinkClient::~VgLinkClient() { stop(); }

void VgLinkClient::start(const std::string& host, int port, const std::string& name, int wantSlot, const std::string& token) {
    stop();
    netInit();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->host = host.empty() ? "127.0.0.1" : host;
        p_->port = port > 0 && port < 65535 ? port : kVgLinkDefaultPort;
        p_->name = name;
        p_->wantSlot = std::clamp(wantSlot, 1, kMaxInstances);
        p_->token = token;
        p_->status = "looking for OnAir at " + p_->host + ":" + std::to_string(p_->port);
        p_->raiseSeen = p_->quitSeen = -1;
    }
    p_->s = udpSocket(INADDR_ANY, 0, 0);
    if (p_->s == kBadSock) { std::lock_guard<std::mutex> lk(p_->mu); p_->status = "cannot make a UDP socket"; return; }
    int sndBuf = 1 << 20;
    setsockopt(p_->s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof sndBuf);
    p_->haveAddr = false;
    p_->stop = false;
    p_->th = namedThread("vg-link", [this] { p_->run(); });
}

void VgLinkClient::stop() {
    if (!p_->th.joinable()) return;
    for (int i = 0; i < 3; i++) p_->sendRtcp(VgMsg::Bye, "stop");   // three times: UDP may lose one
    p_->stop = true;
    p_->th.join();
    sockClose(p_->s);
    p_->s = kBadSock;
    p_->haveAddr = false;
    p_->linked = false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->slot = 0;
}

bool VgLinkClient::connected() const { return p_->linked; }
int VgLinkClient::slot() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->slot; }
std::string VgLinkClient::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->status; }
VgTxInfo VgLinkClient::txInfo(double& usableBps) const { std::lock_guard<std::mutex> lk(p_->mu); usableBps = p_->usable; return p_->tx; }

void VgLinkClient::setStatus(bool easy, const std::vector<std::string>& programmes) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (easy == p_->easy && programmes == p_->progs) return;
    p_->easy = easy;
    p_->progs = programmes;
    p_->dirty = true;
}

void VgLinkClient::startStream(double muxBps) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = std::max(1.0, muxBps);
    p_->stream++;
    p_->dirty = true;
}

void VgLinkClient::setRate(double muxBps) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rate == muxBps || p_->rate <= 0) return;
    p_->rate = muxBps;
    p_->dirty = true;
}

void VgLinkClient::stopStream() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = 0;
    p_->dirty = true;
}

bool VgLinkClient::sendTs(const uint8_t* packets, size_t n) {
    if (n == 0) return true;
    if (!p_->linked || !p_->haveAddr) return false;
    const uint32_t t90 = (uint32_t)(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count() * 9 / 100);
    std::lock_guard<std::mutex> lk(p_->rtpMu);
    for (size_t at = 0; at < n; at += kTsPerRtp) {
        const size_t k = std::min(kTsPerRtp, n - at);
        const std::vector<uint8_t> d = vgRtp(packets + at * 188, k, p_->seq++, t90, p_->ssrc);
        if (sockSendTo(p_->s, d.data(), d.size(), p_->rtpTo) < 0) return false;   // the send buffer is full: the rest is dropped
        p_->sent += k;
    }
    return true;
}

void VgLinkClient::bye() { for (int i = 0; i < 3; i++) p_->sendRtcp(VgMsg::Bye, "closed"); }
bool VgLinkClient::takeRaise() { return p_->raise.exchange(false); }
bool VgLinkClient::takeQuit() { return p_->quit.exchange(false); }
uint64_t VgLinkClient::sentPackets() const { return p_->sent; }

} // namespace dect2::vgen
