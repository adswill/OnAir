// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/net/wsprnet.h"
#include "dect2/net/pskreporter.h"
#include "tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2 {
namespace wsprnet {
namespace {

std::string appendPair(std::string& o, const char* k, const std::string& v) {
    if (!o.empty()) o += '&';
    o += urlEncodeForm(k);
    o += '=';
    o += urlEncodeForm(v);
    return o;
}

void slotDateTime(int64_t slotUtc, char* date, size_t dn, char* time, size_t tn) {
    const time_t t = (time_t)std::max<int64_t>(0, slotUtc);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    snprintf(date, dn, "%02d%02d%02d", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday);
    snprintf(time, tn, "%02d%02d", tm.tm_hour, tm.tm_min);
}

} // namespace

std::string urlEncodeForm(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string encodeSpotBody(const SpotFields& s, const std::string& version) {
    char date[8], utime[8];
    slotDateTime(s.slotUtc, date, sizeof date, utime, sizeof utime);
    char rqrg[32], tqrg[32], sig[16], dt[16], drift[16], dbm[16];
    snprintf(rqrg, sizeof rqrg, "%.6f", s.rqrgMhz);
    snprintf(tqrg, sizeof tqrg, "%.6f", s.tqrgMhz);
    snprintf(sig, sizeof sig, "%d", s.sigDb);
    snprintf(dt, sizeof dt, "%.1f", s.dt);
    snprintf(drift, sizeof drift, "%d", s.drift);
    snprintf(dbm, sizeof dbm, "%d", s.dbm);
    std::string q;
    appendPair(q, "function", "wspr");
    appendPair(q, "rcall", s.rcall);
    appendPair(q, "rgrid", s.rgrid);
    appendPair(q, "rqrg", rqrg);
    appendPair(q, "mode", "2");
    appendPair(q, "date", date);
    appendPair(q, "time", utime);
    appendPair(q, "sig", sig);
    appendPair(q, "dt", dt);
    appendPair(q, "drift", drift);
    appendPair(q, "tqrg", tqrg);
    appendPair(q, "tcall", s.tcall);
    appendPair(q, "tgrid", s.tgrid);
    appendPair(q, "dbm", dbm);
    appendPair(q, "version", version);
    return q;
}

bool Dedupe::accept(const std::string& tcall, int64_t slotUtc, int freqHz10, int64_t now) {
    const int64_t slot = slotUtc / 120;
    const std::string key = tcall + "|" + std::to_string(slot) + "|" + std::to_string(freqHz10);
    seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const Seen& e) { return now - e.t >= window_; }), seen_.end());
    for (const Seen& e : seen_) if (e.key == key) return false;
    seen_.push_back({key, now});
    return true;
}

} // namespace wsprnet

namespace {

bool wsprHttpPost(const std::string& host, int port, const std::string& body, int timeoutMs, int& httpStatus, std::string& err) {
    httpStatus = 0;
    sock_t s = tcpConnect(host, port, timeoutMs, err);
    if (s == kBadSock) return false;
    const std::string req = "POST /post/ HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/x-www-form-urlencoded\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!sockSendAll(s, req.data(), req.size())) { err = "send failed"; sockClose(s); return false; }
    sockRecvTimeout(s, timeoutMs);
    char buf[1024];
    size_t n = 0;
    while (n < sizeof buf - 1) {
        const long long k = sockRecv(s, buf + n, sizeof buf - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        if (n >= 12) break;
    }
    sockClose(s);
    buf[n] = 0;
    if (n < 12 || std::strncmp(buf, "HTTP/", 5) != 0) { err = "no HTTP response"; return false; }
    const char* sp = strchr(buf, ' ');
    if (!sp) { err = "bad HTTP status"; return false; }
    httpStatus = atoi(sp + 1);
    if (httpStatus < 200 || httpStatus >= 300) { err = std::string("HTTP ") + std::to_string(httpStatus); return false; }
    return true;
}

} // namespace

struct WsprNet::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    WsprNetConfig cfg;
    WsprNetStatus st;
    wsprnet::Dedupe dedupe;
    std::thread th;
    bool quit = false;
    struct Pending { wsprnet::SpotFields spot; };
    std::vector<Pending> queue;
    int64_t lastPost = 0;
    static constexpr int kMinPostIntervalSec = 2;   // gentle on wsprnet.org
    static constexpr int kHttpTimeoutMs = 30000;
    static int64_t nowSec() { return dect2::unixNow(); }
    bool credsOk() const { return pskr::validCallsign(cfg.call) && pskr::validLocator(cfg.locator); }

    void sendOne(const wsprnet::SpotFields& spot) {
        const std::string ver = std::string("OnAir-") + ONAIR_VERSION;
        const std::string body = wsprnet::encodeSpotBody(spot, ver);
        int code = 0;
        std::string err;
        const bool ok = wsprHttpPost(cfg.host, cfg.port, body, kHttpTimeoutMs, code, err);
        const int64_t now = nowSec();
        if (ok) {
            st.active = true;
            st.lastError.clear();
            st.spotsSent++;
            st.lastSend = now;
            fprintf(stderr, "[wsprnet] spot %s %.6f MHz sig %d dBm %d sent (HTTP %d)\n", spot.tcall.c_str(), spot.tqrgMhz, spot.sigDb, spot.dbm, code);
            fflush(stderr);
        } else {
            st.active = false;
            st.failures++;
            st.lastError = err;
            fprintf(stderr, "[wsprnet] upload failed: %s\n", err.c_str());
            fflush(stderr);
        }
        lastPost = now;
    }

    void run() {
        std::unique_lock<std::mutex> lk(mu);
        while (!quit) {
            if (queue.empty()) cv.wait_for(lk, std::chrono::seconds(30));   // nothing to send: sleep until a spot or stop (both notify)
            else cv.wait_for(lk, std::chrono::milliseconds(250));
            if (quit) break;
            if (!cfg.on || queue.empty()) continue;
            const int64_t now = nowSec();
            if (lastPost && now - lastPost < kMinPostIntervalSec) continue;
            wsprnet::SpotFields spot = queue.front().spot;
            queue.erase(queue.begin());
            lk.unlock();
            sendOne(spot);
            lk.lock();
        }
    }
};

WsprNet::WsprNet() : impl_(new Impl) {}
WsprNet::~WsprNet() { stop(); }
WsprNet& WsprNet::get() { static WsprNet r; return r; }

void WsprNet::configure(const WsprNetConfig& c) {
    Impl& p = *impl_;
    WsprNetConfig cc = c;
    for (char& ch : cc.call) ch = (char)toupper((unsigned char)ch);
    for (char& ch : cc.locator) ch = (char)toupper((unsigned char)ch);
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.cfg = cc;
        want_.store(cc.on && p.credsOk(), std::memory_order_relaxed);
        if (!cc.on) { p.queue.clear(); p.dedupe.clear(); p.st.active = false; }
        start = cc.on && !p.th.joinable();
        if (start) p.quit = false;
    }
    if (start) p.th = dect2::namedThread("onair-wspr", [&p] { p.run(); });
    p.cv.notify_all();
}

WsprNetConfig WsprNet::config() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->cfg; }
WsprNetStatus WsprNet::status() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WsprNetStatus s = impl_->st;
    if (!impl_->cfg.on) s.state = "off";
    else if (!pskr::validCallsign(impl_->cfg.call)) s.state = "needs a valid callsign";
    else if (!pskr::validLocator(impl_->cfg.locator)) s.state = "needs a valid locator (e.g. KO24ab)";
    else s.state = "on, " + std::to_string(impl_->queue.size()) + " spots waiting";
    return s;
}

void WsprNet::stop() {
    Impl& p = *impl_;
    { std::lock_guard<std::mutex> lk(p.mu); p.quit = true; }
    p.cv.notify_all();
    if (p.th.joinable()) p.th.join();
    want_.store(false, std::memory_order_relaxed);
}

void WsprNet::spot(double audioHz, float snrDb, float dt, const std::string& call, const std::string& grid, int dbm, double slotUtc, double dialHz) {
    if (!want()) return;
    const double dial = dialHz > 0 ? dialHz : dial_.load(std::memory_order_relaxed);
    if (dial < 1e5) return;
    std::string c = call;
    for (char& ch : c) ch = (char)toupper((unsigned char)ch);
    if (c.size() > 2 && c.front() == '<' && c.back() == '>') c = c.substr(1, c.size() - 2);
    if (!pskr::validCallsign(c)) return;
    std::string g = grid;
    for (char& ch : g) ch = (char)toupper((unsigned char)ch);
    if (!g.empty() && !pskr::validLocator(g)) g.clear();
    const double rfHz = dial + audioHz;
    const int freqHz10 = (int)std::lround(rfHz / 10.0);
    wsprnet::SpotFields s;
    Impl& p = *impl_;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        if (!p.credsOk()) return;
        const int64_t now = Impl::nowSec();
        const int64_t slot = (int64_t)std::floor(slotUtc);
        if (!p.dedupe.accept(c, slot, freqHz10, now)) { p.st.duplicates++; return; }
        s.rcall = p.cfg.call;
        s.rgrid = p.cfg.locator;
        s.rqrgMhz = dial / 1e6;
        s.tcall = c;
        s.tgrid = g;
        s.tqrgMhz = rfHz / 1e6;
        s.sigDb = (int)std::lround(snrDb);
        s.dt = dt;
        s.drift = 0;
        s.dbm = dbm >= 0 ? dbm : 0;
        s.slotUtc = slot;
        p.queue.push_back({s});
    }
    p.cv.notify_all();
}

} // namespace dect2
