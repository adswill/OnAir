// The link between the Digital video generator and OnAir (vglink.h, docs/VIDEOGEN-LINK.md): RTP and RTCP over the local pipe or UDP
// (vglink_transport.cpp).
#include "dect2/tv/vglink.h"
#include "vglink_transport.h"
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

// ---- helpers --------------------------------------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

std::string text(const std::vector<uint8_t>& d) { return std::string(d.begin(), d.end()); }

uint32_t newSsrc(const void* self) {
    std::random_device rd;
    return rd() ^ (uint32_t)Clock::now().time_since_epoch().count() ^ (uint32_t)(reinterpret_cast<uintptr_t>(self) >> 4);
}

// RTCP or RTP: on the local pipe both come in one place (RFC 5761: RTCP packet types 200..204 sit where RTP has M + PT)
bool isRtcp(const uint8_t* d, size_t n) { return n >= 2 && d[1] >= 200 && d[1] <= 204; }

} // namespace

// ---- OnAir side -------------------------------------------------------------------------------------------------------------

struct VgLinkServer::Impl {
    struct Peer {
        uint32_t ssrc = 0;
        VgPeer info;
        int via = 0;               // the transport (index into tr)
        std::string from;          // its address there: RTP must come from it, the Reply goes there
        Clock::time_point seen;
        int stream = -1;
        bool haveSeq = false;
        uint16_t seq = 0;
        int raise = 0, quit = 0, start = 0;
    };
    mutable std::mutex mu;
    std::vector<std::unique_ptr<link::ServerTransport>> tr;   // changed only while stopped
    int port = 0;
    bool local = false;
    std::string localName;
    std::atomic<bool> stop{false};
    std::thread th;
    std::vector<Peer> peers;
    VgTxInfo tx;
    std::string ownToken;   // the token of the generator OnAir started
    std::function<void(const std::string&)>* onBye = nullptr;

    Peer* find(uint32_t ssrc) { for (auto& p : peers) if (p.ssrc == ssrc) return &p; return nullptr; }
    int freeSlot(int want) const {
        auto used = [&](int k) { for (const auto& p : peers) if (p.info.slot == k) return true; return false; };
        if (want >= 1 && want <= kMaxInstances && !used(want)) return want;
        for (int k = 1; k <= kMaxInstances; k++) if (!used(k)) return k;
        return 0;
    }
    void send(int via, const std::string& to, VgMsg t, const VgKv& kv) {
        const std::vector<uint8_t> d = vgRtcp(t, vgKvText(kv), 0, "onair");
        if (via >= 0 && via < (int)tr.size()) tr[(size_t)via]->send(to, d.data(), d.size());
    }
    void reply(const Peer& p) {   // under mu
        VgKv kv = vgTxInfoKv(tx, p.info.slot);
        kv["proto"] = std::to_string(kVgLinkVersion);
        kv["slot"] = std::to_string(p.info.slot);
        kv["raise"] = std::to_string(p.raise);
        kv["quit"] = std::to_string(p.quit);
        kv["start"] = std::to_string(p.start);
        kv["ours"] = !ownToken.empty() && p.info.token == ownToken ? "1" : "0";
        send(p.via, p.from, VgMsg::Reply, kv);
    }
    void drop(size_t i) {   // under mu
        generatorSetRate(peers[i].info.slot, 0);
        generatorClear(peers[i].info.slot);
        peers.erase(peers.begin() + (std::ptrdiff_t)i);
    }
    void onDatagram(int via, const uint8_t* d, size_t n, const std::string& from, const std::string& host);
    void onState(uint32_t ssrc, const VgKv& kv, int via, const std::string& from, const std::string& host);
    void onRtp(const VgPacket& m, int via, const std::string& from);
    void run();
};

void VgLinkServer::Impl::onDatagram(int via, const uint8_t* d, size_t n, const std::string& from, const std::string& host) {
    if (!isRtcp(d, n)) {
        VgPacket m;
        if (vgParseRtp(d, n, m)) onRtp(m, via, from);
        return;
    }
    std::vector<VgPacket> msgs;
    if (!vgParseRtcp(d, n, msgs)) return;
    for (const VgPacket& m : msgs) {
        if (m.type == VgMsg::State) onState(m.ssrc, vgKvParse(text(m.data)), via, from, host);
        else if (m.type == VgMsg::Bye) {
            std::string tok;
            bool closed = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                for (size_t i = 0; i < peers.size(); i++)
                    if (peers[i].ssrc == m.ssrc && peers[i].via == via && peers[i].from == from) { tok = peers[i].info.token; closed = text(m.data) == "closed"; drop(i); break; }
            }
            if (closed && onBye && *onBye) (*onBye)(tok);
        }
    }
}

void VgLinkServer::Impl::onState(uint32_t ssrc, const VgKv& kv, int via, const std::string& from, const std::string& host) {
    std::lock_guard<std::mutex> lk(mu);
    Peer* p = find(ssrc);
    if (!p) {
        if ((int)vgKvNum(kv, "proto", 0) < 1) return;
        const int slot = freeSlot((int)vgKvNum(kv, "slot", 1));
        if (slot == 0) {
            send(via, from, VgMsg::Refused, {{"error", "OnAir already has " + std::to_string(kMaxInstances) + " generators linked"}});
            return;
        }
        Peer n;
        n.ssrc = ssrc;
        n.info.slot = slot;
        generatorSetRate(slot, 0);
        generatorClear(slot);
        peers.push_back(n);
        p = &peers.back();
    }
    p->via = via;
    p->from = from;
    p->info.local = host.empty();
    p->info.host = host.empty() ? "this computer" : host;
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

void VgLinkServer::Impl::onRtp(const VgPacket& m, int via, const std::string& from) {
    int slot = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        Peer* p = find(m.ssrc);
        if (!p || p->via != via || p->from != from || p->info.rateBps <= 0) return;   // not one of ours, or not sending (a stale datagram)
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

void VgLinkServer::Impl::run() {   // the Replies each second, and the generators gone without a BYE
    auto lastReply = Clock::now();
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        if (now - lastReply < std::chrono::seconds(1)) continue;
        lastReply = now;
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = peers.size(); i-- > 0;) {
            if (now - peers[i].seen > std::chrono::duration<double>(kVgLinkTimeoutSec)) drop(i);
            else reply(peers[i]);
        }
    }
}

VgLinkServer::VgLinkServer() : p_(std::make_unique<Impl>()) { p_->onBye = &onBye; }
VgLinkServer::~VgLinkServer() { stop(); }

bool VgLinkServer::start(bool network, int port, bool anyAddress, std::string& err, const std::string& localName) {
    stop();
    err.clear();
    Impl* I = p_.get();
    auto cb = [I](int via) {
        return [I, via](const uint8_t* d, size_t n, const std::string& from, const std::string& host) { I->onDatagram(via, d, n, from, host); };
    };
    std::string e;
    // (the transports are added before their first datagram can need them: the callbacks only use their own index)
    std::lock_guard<std::mutex> lk(I->mu);
    if (auto t = link::listenLocal(localName, cb((int)I->tr.size()), e)) {
        I->local = true;
        I->localName = localName.empty() ? link::localName() : localName;
        I->tr.push_back(std::move(t));
    } else
        err = e;
    if (network) {
        e.clear();
        if (auto t = link::listenNetwork(port, anyAddress, cb((int)I->tr.size()), e)) {
            I->port = t->port();
            I->tr.push_back(std::move(t));
        } else
            err += (err.empty() ? "" : "; ") + e;
    }
    if (!I->tr.empty()) {
        I->stop = false;
        I->th = namedThread("vg-link", [I] { I->run(); });
    }
    return err.empty();
}

void VgLinkServer::stop() {
    p_->stop = true;
    if (p_->th.joinable()) p_->th.join();
    for (auto& t : p_->tr) t->stop();   // (not under mu: their threads may be waiting for it)
    std::lock_guard<std::mutex> lk(p_->mu);
    while (!p_->peers.empty()) p_->drop(p_->peers.size() - 1);
    p_->tr.clear();
    p_->port = 0;
    p_->local = false;
    p_->localName.clear();
}

bool VgLinkServer::listening() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->tr.empty(); }
bool VgLinkServer::listeningLocal() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->local; }
int VgLinkServer::port() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->port; }
std::string VgLinkServer::localName() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->localName; }

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
    for (const auto& p : p_->peers) p_->reply(p);
}

enum class Bump { Raise, Quit, Start };
static bool bumpByToken(VgLinkServer::Impl& I, const std::string& token, Bump what) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(I.mu);
    bool any = false;
    for (auto& p : I.peers)
        if (p.info.token == token) { (what == Bump::Quit ? p.quit : what == Bump::Start ? p.start : p.raise)++; I.reply(p); any = true; }
    return any;
}

bool VgLinkServer::raise(const std::string& token) { return bumpByToken(*p_, token, Bump::Raise); }
bool VgLinkServer::startStreaming(const std::string& token) { return bumpByToken(*p_, token, Bump::Start); }
void VgLinkServer::setOwnToken(const std::string& token) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->ownToken = token;
    for (const auto& p : p_->peers) p_->reply(p);   // at once: the generator learns whether it is OnAir's own
}
bool VgLinkServer::quit(const std::string& token) { return bumpByToken(*p_, token, Bump::Quit); }
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
    std::unique_ptr<link::ClientTransport> t;   // set in start, reset in stop (the thread is not running then)
    std::string name, token, where;
    int wantSlot = 1, slot = 0;
    std::atomic<bool> linked{false};
    Clock::time_point lastReply;
    std::string status;
    VgTxInfo tx;
    double usable = 0;
    bool ours = false;
    bool easy = true;
    std::vector<std::string> progs;
    double rate = 0;                // > 0: streaming
    int stream = 0;                 // counts the starts
    int raiseSeen = -1, quitSeen = -1, startSeen = -1;
    std::atomic<bool> raise{false}, quit{false}, start{false};
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
    void sendRtcp(VgMsg type, const std::string& txt) {
        std::string nm;
        { std::lock_guard<std::mutex> lk(mu); nm = name; }
        const std::vector<uint8_t> d = vgRtcp(type, txt, ssrc, "onair-videogen", nm);
        if (t && t->isOpen()) t->sendRtcp(d.data(), d.size());
    }
    void begin(std::unique_ptr<link::ClientTransport> tr, const std::string& nm, int want, const std::string& tok);
    void run();
};

void VgLinkClient::Impl::run() {
    std::vector<uint8_t> buf(65536);
    std::vector<VgPacket> msgs;
    const std::string at = where == "this computer" ? "on this computer" : "at " + where;
    auto lastState = Clock::now() - std::chrono::seconds(10), lastOpen = lastState;
    bool refused = false;
    while (!stop) {
        auto now = Clock::now();
        if (!t->isOpen() && now - lastOpen > std::chrono::seconds(1)) {   // not there yet (OnAir not running, no network): try again
            lastOpen = now;
            std::string why;
            if (!t->open(why)) { std::lock_guard<std::mutex> lk(mu); if (!linked) status = why; }
        }
        if (t->isOpen() && (dirty.exchange(false) || now - lastState >= std::chrono::milliseconds(refused ? 3000 : 1000))) {
            lastState = now;
            std::string st;
            { std::lock_guard<std::mutex> lk(mu); st = stateText(); }
            sendRtcp(VgMsg::State, st);
        }
        const long long n = t->recv(buf, 50);
        msgs.clear();
        if (n > 0 && isRtcp(buf.data(), (size_t)n) && vgParseRtcp(buf.data(), (size_t)n, msgs)) {
            for (const VgPacket& m : msgs) {
                const VgKv kv = vgKvParse(text(m.data));
                std::lock_guard<std::mutex> lk(mu);
                if (m.type == VgMsg::Reply) {
                    refused = false;
                    slot = (int)vgKvNum(kv, "slot");
                    tx = vgTxInfoFromKv(kv, usable);
                    ours = vgKvNum(kv, "ours") > 0.5;
                    lastReply = Clock::now();
                    if (!linked) status = "linked to OnAir " + at + " as generator:" + std::to_string(slot);
                    linked = true;
                    const int r = (int)vgKvNum(kv, "raise"), q = (int)vgKvNum(kv, "quit");
                    if (raiseSeen >= 0 && r > raiseSeen) raise = true;   // the first Reply only tells where the counters are
                    if (quitSeen >= 0 && q > quitSeen) quit = true;
                    raiseSeen = r; quitSeen = q;
                    const int st = (int)vgKvNum(kv, "start");
                    if (startSeen >= 0 && st > startSeen) start = true;
                    startSeen = st;
                } else if (m.type == VgMsg::Refused) {
                    refused = true;
                    linked = false;
                    slot = 0;
                    status = "OnAir refused the link: " + vgKvStr(kv, "error");
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
            raiseSeen = quitSeen = startSeen = -1;   // OnAir may have restarted: its counters start again
            status = "OnAir does not answer " + at + " (trying again)";
        } else if (!linked && !refused && t->isOpen() && now - lastState < std::chrono::milliseconds(60) && status.compare(0, 6, "linked") != 0 && status.compare(0, 12, "OnAir does n") != 0)
            status = "looking for OnAir " + at;
    }
}

VgLinkClient::VgLinkClient() : p_(std::make_unique<Impl>()) {
    p_->ssrc = newSsrc(this);   // a random synchronisation source (RFC 3550)
    p_->seq = (uint16_t)(p_->ssrc >> 16);
}
VgLinkClient::~VgLinkClient() { stop(); }

void VgLinkClient::Impl::begin(std::unique_ptr<link::ClientTransport> tr, const std::string& nm, int want, const std::string& tok) {
    t = std::move(tr);
    {
        std::lock_guard<std::mutex> lk(mu);
        where = t->where();
        name = nm;
        wantSlot = std::clamp(want, 1, kMaxInstances);
        token = tok;
        status = "looking for OnAir " + std::string(where == "this computer" ? "on " : "at ") + where;
        raiseSeen = quitSeen = startSeen = -1;
    }
    stop = false;
    th = namedThread("vg-link", [this] { run(); });
}

void VgLinkClient::start(const std::string& host, int port, const std::string& name, int wantSlot, const std::string& token) {
    stop();
    p_->begin(link::networkClient(host, port), name, wantSlot, token);
}

void VgLinkClient::startLocal(const std::string& localName, const std::string& name, int wantSlot, const std::string& token) {
    stop();
    p_->begin(link::localClient(localName), name, wantSlot, token);
}

void VgLinkClient::stop() {
    if (!p_->th.joinable()) return;
    for (int i = 0; i < 3; i++) p_->sendRtcp(VgMsg::Bye, "stop");   // three times: UDP may lose one
    p_->stop = true;
    p_->th.join();
    p_->linked = false;
    { std::lock_guard<std::mutex> lk(p_->rtpMu); p_->t.reset(); }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->slot = 0;
}

bool VgLinkClient::connected() const { return p_->linked; }
int VgLinkClient::slot() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->slot; }
std::string VgLinkClient::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->status; }
VgTxInfo VgLinkClient::txInfo(double& usableBps) const { std::lock_guard<std::mutex> lk(p_->mu); usableBps = p_->usable; return p_->tx; }
bool VgLinkClient::ours() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->ours && connected(); }

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
    if (!p_->linked) return false;
    const uint32_t t90 = (uint32_t)(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count() * 9 / 100);
    std::lock_guard<std::mutex> lk(p_->rtpMu);
    if (!p_->t || !p_->t->isOpen()) return false;
    for (size_t at = 0; at < n; at += kTsPerRtp) {
        const size_t k = std::min(kTsPerRtp, n - at);
        const std::vector<uint8_t> d = vgRtp(packets + at * 188, k, p_->seq++, t90, p_->ssrc);
        if (!p_->t->sendRtp(d.data(), d.size())) return false;   // full: the rest is dropped
        p_->sent += k;
    }
    return true;
}

void VgLinkClient::bye() { for (int i = 0; i < 3; i++) p_->sendRtcp(VgMsg::Bye, "closed"); }
bool VgLinkClient::takeRaise() { return p_->raise.exchange(false); }
bool VgLinkClient::takeStart() { return p_->start.exchange(false); }
bool VgLinkClient::takeQuit() { return p_->quit.exchange(false); }
uint64_t VgLinkClient::sentPackets() const { return p_->sent; }

} // namespace dect2::vgen
