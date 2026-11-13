// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/net/pskreporter.h"
#include "tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#ifndef _WIN32
#include <netdb.h>
#endif
#include <random>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {
namespace pskr {
namespace {
void p16(std::vector<uint8_t>& o, unsigned v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
void p32(std::vector<uint8_t>& o, uint32_t v) { p16(o, v >> 16); p16(o, v & 0xFFFF); }
void pStr(std::vector<uint8_t>& o, const std::string& s) { const size_t n = std::min<size_t>(s.size(), 254); o.push_back((uint8_t)n); o.insert(o.end(), s.begin(), s.begin() + (long)n); }
void pad4(std::vector<uint8_t>& o, size_t start) { while ((o.size() - start) % 4) o.push_back(0); }
void setLen(std::vector<uint8_t>& o, size_t start) { o[start + 2] = (uint8_t)((o.size() - start) >> 8); o[start + 3] = (uint8_t)(o.size() - start); }
// an enterprise field specifier: 0x8000 | id, length, enterprise 30351
void field(std::vector<uint8_t>& o, unsigned id, unsigned len) { p16(o, 0x8000 | id); p16(o, len); p32(o, 30351); }
// ids: 1 senderCallsign 2 receiverCallsign 3 senderLocator 4 receiverLocator 5 frequency 6 sNR 8 decoderSoftware 9 antenna 10 mode 11 infoSource
void templates(std::vector<uint8_t>& o) {
    const size_t a = o.size();
    p16(o, 3); p16(o, 0); p16(o, kReceiverTemplateId); p16(o, 4); p16(o, 1);
    field(o, 2, 0xFFFF); field(o, 4, 0xFFFF); field(o, 8, 0xFFFF); field(o, 9, 0xFFFF);
    p16(o, 0);
    setLen(o, a);
    const size_t b = o.size();
    p16(o, 2); p16(o, 0); p16(o, kSenderTemplateId); p16(o, 7);
    field(o, 1, 0xFFFF); field(o, 5, 4); field(o, 6, 1); field(o, 10, 0xFFFF); field(o, 11, 1); field(o, 3, 0xFFFF);
    p16(o, 0x96); p16(o, 4);   // flowStartSeconds
    setLen(o, b);
}
std::vector<uint8_t> senderRecord(const Spot& s) {
    std::vector<uint8_t> r;
    pStr(r, s.call); p32(r, s.freqHz); r.push_back((uint8_t)(int8_t)std::max(-128, std::min(127, s.snr)));
    pStr(r, s.mode); r.push_back(1); pStr(r, s.locator); p32(r, (uint32_t)s.time);
    return r;
}
} // namespace

size_t encodePacket(std::vector<uint8_t>& o, const Receiver& rx, const std::vector<Spot>& spots, size_t first, int64_t now, uint32_t seq, uint32_t streamId, bool withTemplates) {
    o.clear();
    p16(o, 0x000A); p16(o, 0); p32(o, (uint32_t)now); p32(o, seq); p32(o, streamId);
    if (withTemplates) templates(o);
    const size_t r0 = o.size();
    p16(o, kReceiverTemplateId); p16(o, 0);
    pStr(o, rx.call); pStr(o, rx.locator); pStr(o, rx.software); pStr(o, rx.antenna);
    pad4(o, r0);
    setLen(o, r0);
    size_t used = 0;
    if (first < spots.size()) {
        const size_t s0 = o.size();
        p16(o, kSenderTemplateId); p16(o, 0);
        for (size_t i = first; i < spots.size(); i++) {
            const std::vector<uint8_t> r = senderRecord(spots[i]);
            if (o.size() + r.size() + 3 > kMaxPacket) break;   // +3: the padding
            o.insert(o.end(), r.begin(), r.end());
            used++;
        }
        if (!used) o.resize(s0);
        else { pad4(o, s0); setLen(o, s0); }
    }
    o[2] = (uint8_t)(o.size() >> 8); o[3] = (uint8_t)o.size();
    return used;
}

bool decodePacket(const uint8_t* d, size_t n, Decoded& out) {
    auto g16 = [&](size_t p) { return (unsigned)(d[p] << 8 | d[p + 1]); };
    auto g32 = [&](size_t p) { return (uint32_t)g16(p) << 16 | g16(p + 2); };
    if (n < 16 || g16(0) != 10 || g16(2) != n) return false;
    out = Decoded();
    out.time = g32(4); out.seq = g32(8); out.streamId = g32(12);
    size_t p = 16;
    auto str = [&](size_t& q, size_t end, std::string& s) { if (q >= end) return false; const size_t l = d[q++]; if (q + l > end) return false; s.assign((const char*)d + q, l); q += l; return true; };
    while (p + 4 <= n) {
        const unsigned id = g16(p), len = g16(p + 2);
        if (len < 4 || p + len > n) return false;
        const size_t end = p + len;
        if (id == 2 || id == 3) out.hasTemplates = true;
        else if (id == (unsigned)kReceiverTemplateId) {
            size_t q = p + 4;
            if (!str(q, end, out.rx.call) || !str(q, end, out.rx.locator) || !str(q, end, out.rx.software) || !str(q, end, out.rx.antenna)) return false;
        } else if (id == (unsigned)kSenderTemplateId) {
            size_t q = p + 4;
            while (end - q >= 12) {   // a record is at least 1+4+1+1+1+1+4 bytes (with a 1 byte call: 13); the rest is padding
                Spot s;
                if (!str(q, end, s.call)) return false;
                if (q + 5 > end) return false;
                s.freqHz = g32(q); q += 4; s.snr = (int8_t)d[q++];
                if (!str(q, end, s.mode)) return false;
                if (q >= end) return false;
                q++;   // informationSource
                if (!str(q, end, s.locator)) return false;
                if (q + 4 > end) return false;
                s.time = g32(q); q += 4;
                out.spots.push_back(s);
            }
        } else return false;
        p = end;
    }
    return p == n;
}

bool validCallsign(const std::string& c) {
    if (c.size() < 3 || c.size() > 12) return false;
    bool dig = false, let = false;
    for (char ch : c) {
        if (ch >= '0' && ch <= '9') dig = true;
        else if (ch >= 'A' && ch <= 'Z') let = true;
        else if (ch != '/') return false;
    }
    return dig && let;
}
bool validLocator(const std::string& l) {
    if (l.size() != 4 && l.size() != 6 && l.size() != 8) return false;
    for (size_t i = 0; i < l.size(); i++) {
        const char c = l[i];
        const int k = i < 2 ? 0 : i < 4 ? 1 : i < 6 ? 2 : 1;
        if (k == 0 && !(c >= 'A' && c <= 'R')) return false;
        if (k == 1 && !(c >= '0' && c <= '9')) return false;
        if (k == 2 && !((c >= 'A' && c <= 'X') || (c >= 'a' && c <= 'x'))) return false;
    }
    return true;
}

bool sendsInText(const std::string& text, const std::string& call) {
    const std::string k = "DE " + call;
    for (size_t p = text.find(k); p != std::string::npos; p = text.find(k, p + 1)) {
        const size_t e = p + k.size();
        if ((p == 0 || text[p - 1] == ' ' || text[p - 1] == '\n') && (e == text.size() || text[e] == ' ' || text[e] == '\n' || text[e] == '\r')) return true;
    }
    return false;
}

bool Batcher::add(const Spot& s, int64_t now) {
    if (s.call.empty() || s.freqHz == 0) return false;
    const std::string key = s.call + "|" + s.mode + "|" + std::to_string(s.freqHz / 100000);   // band: 100 kHz cells keep an FT8 slot and its neighbours together
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const Seen& e) { return now - e.t >= dedupe_; }), seen_.end());
    for (const Seen& e : seen_) if (e.key == key) return false;
    seen_.push_back({key, now});
    if (v_.empty()) last_ = now;
    v_.push_back(s);
    return true;
}
bool Batcher::due(int64_t now) const { return !v_.empty() && (v_.size() >= max_ || now - last_ >= interval_); }
std::vector<Spot> Batcher::take(int64_t now) { last_ = now; std::vector<Spot> o; o.swap(v_); return o; }

} // namespace pskr

struct PskReporter::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    PskReporterConfig cfg;
    PskReporterStatus st;
    pskr::Batcher bat;
    std::thread th;
    std::function<bool(const uint8_t*, size_t)> testSend;
    bool quit = false;
    uint32_t seq = 0, streamId = 0;
    int packetsWithTemplates = 0;
    int64_t lastTemplates = 0;
    sock_t sock = kBadSock; sockaddr_in to{};
    std::string toHost; int toPort = 0;
    static int64_t nowSec() { return dect2::unixNow(); }
    bool credsOk() const { return pskr::validCallsign(cfg.call) && pskr::validLocator(cfg.locator); }
    void closeSock() { if (sock != kBadSock) sockClose(sock); sock = kBadSock; toHost.clear(); }
    bool openSock() {
        if (sock != kBadSock && toHost == cfg.host && toPort == cfg.port) return true;
        closeSock();
        netInit();
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(cfg.host.c_str(), std::to_string(cfg.port).c_str(), &hints, &res) != 0 || !res) { st.lastError = "cannot find " + cfg.host; if (res) freeaddrinfo(res); return false; }
        memcpy(&to, res->ai_addr, sizeof to); freeaddrinfo(res);
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kBadSock) { st.lastError = "cannot open a socket"; return false; }
        toHost = cfg.host; toPort = cfg.port;
        return true;
    }
    void flush(int64_t now) {   // called with mu held
        const std::vector<pskr::Spot> sp = bat.take(now);
        if (!credsOk() || (!testSend && !openSock())) { st.active = false; return; }
        const pskr::Receiver rx{cfg.call, cfg.locator, "OnAir", cfg.antenna};
        size_t at = 0;
        while (at < sp.size()) {
            const bool tpl = packetsWithTemplates < 3 || now - lastTemplates >= 600;   // the first few packets, then every 10 minutes
            std::vector<uint8_t> pkt;
            const size_t n = pskr::encodePacket(pkt, rx, sp, at, now, seq, streamId, tpl);
            if (!n) break;
            if (testSend ? !testSend(pkt.data(), pkt.size()) : sockSendTo(sock, pkt.data(), pkt.size(), to) < 0) { st.lastError = "send failed"; st.active = false; return; }
            st.lastError.clear(); st.active = true; st.packets++; st.spotsSent += n; st.lastSend = now;
            seq += (uint32_t)n; at += n;
            if (tpl) { packetsWithTemplates++; lastTemplates = now; }
        }
    }
    void run() {
        std::unique_lock<std::mutex> lk(mu);
        while (!quit) {
            cv.wait_for(lk, std::chrono::seconds(2));
            if (quit) break;
            if (!cfg.on) continue;
            const int64_t now = nowSec();
            if (bat.due(now)) flush(now);
        }
    }
};

PskReporter::PskReporter() : impl_(new Impl) {
    std::random_device rd;
    impl_->streamId = rd();
}
PskReporter::~PskReporter() { stop(); }
PskReporter& PskReporter::get() { static PskReporter r; return r; }

void PskReporter::configure(const PskReporterConfig& c) {
    Impl& p = *impl_;
    PskReporterConfig cc = c;
    for (char& ch : cc.call) ch = (char)toupper((unsigned char)ch);
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.cfg = cc;
        want_.store(cc.on && p.credsOk(), std::memory_order_relaxed);
        if (!cc.on) { p.bat.take(0); p.closeSock(); p.st.active = false; }
        start = cc.on && !p.th.joinable();
        if (start) p.quit = false;
    }
    if (start) p.th = dect2::namedThread("onair-psk", [&p] { p.run(); });
    p.cv.notify_all();
}
PskReporterConfig PskReporter::config() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->cfg; }
PskReporterStatus PskReporter::status() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    PskReporterStatus s = impl_->st;
    if (!impl_->cfg.on) s.state = "off";
    else if (!pskr::validCallsign(impl_->cfg.call)) s.state = "needs a valid callsign";
    else if (!pskr::validLocator(impl_->cfg.locator)) s.state = "needs a valid locator (e.g. KO24ab)";
    else s.state = "on, " + std::to_string(impl_->bat.pending()) + " spots waiting";
    return s;
}
void PskReporter::stop() {
    Impl& p = *impl_;
    { std::lock_guard<std::mutex> lk(p.mu); p.quit = true; }
    p.cv.notify_all();
    if (p.th.joinable()) p.th.join();
    std::lock_guard<std::mutex> lk(p.mu);
    p.closeSock();
    want_.store(false, std::memory_order_relaxed);
}
void PskReporter::setSenderForTest(std::function<bool(const uint8_t*, size_t)> f) { std::lock_guard<std::mutex> lk(impl_->mu); impl_->testSend = std::move(f); }
void PskReporter::flushNowForTest(int64_t now) { std::lock_guard<std::mutex> lk(impl_->mu); impl_->flush(now); }
void PskReporter::spot(const char* mode, double audioHz, float snrDb, const std::string& call, const std::string& grid, double dialHz) {
    if (!want()) return;
    const double dial = dialHz > 0 ? dialHz : dial_.load(std::memory_order_relaxed);
    if (dial < 1e5) return;
    std::string c = call;
    for (char& ch : c) ch = (char)toupper((unsigned char)ch);
    if (c.size() > 2 && c.front() == '<' && c.back() == '>') c = c.substr(1, c.size() - 2);
    if (!pskr::validCallsign(c)) return;
    pskr::Spot s;
    s.call = c; s.mode = mode; s.freqHz = (uint32_t)std::max(0.0, dial + audioHz + 0.5); s.snr = (int)std::lround(snrDb);
    if (pskr::validLocator(grid)) s.locator = grid;
    s.time = Impl::nowSec();
    Impl& p = *impl_;
    std::lock_guard<std::mutex> lk(p.mu);
    if (!p.bat.add(s, s.time)) p.st.duplicates++;
}

} // namespace dect2
