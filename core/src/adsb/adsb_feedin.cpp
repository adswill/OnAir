// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/adsb/adsb_feedin.h"
#include "dect2/adsb/adsb_msg.h"
#include "../net/netcompat.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace dect2 {

// ---------------------------------------------------------------- Beast

void BeastParser::push(const uint8_t* d, size_t n, std::vector<BeastFrame>& out) {
    auto start = [&](uint8_t type) {   // after 0x1A, the type byte
        if (type == '1') need_ = 6 + 1 + 2;
        else if (type == '2') need_ = 6 + 1 + 7;
        else if (type == '3') need_ = 6 + 1 + 14;
        else { need_ = 0; return false; }
        buf_.clear();
        buf_.push_back(type);
        return true;
    };
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = d[i];
        switch (state_) {
        case 0: if (b == 0x1A) state_ = 1; break;
        case 1:
            if (start(b)) state_ = 2;
            else { state_ = 0; if (b != 0x1A) resyncs_++; }   // 1A 1A outside a frame: an escaped byte of something we missed
            break;
        case 2:
        case 3:
            if (state_ == 3) {
                if (b == 0x1A) { buf_.push_back(0x1A); state_ = 2; }
                else {   // a new frame began before this one was complete
                    resyncs_++;
                    if (start(b)) state_ = 2; else state_ = 0;
                    break;
                }
            } else if (b == 0x1A) { state_ = 3; break; }
            else buf_.push_back(b);
            if ((int)buf_.size() == 1 + need_) {
                BeastFrame f;
                const uint8_t type = buf_[0];
                for (int k = 0; k < 6; k++) f.ts12 = (f.ts12 << 8) | buf_[1 + k];
                f.signal = buf_[7];
                if (type != '1') {
                    f.bits = type == '2' ? 56 : 112;
                    memcpy(f.bytes, &buf_[8], f.bits / 8);
                    out.push_back(f);
                }
                state_ = 0;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- SBS

static std::vector<std::string> csv(const std::string& s) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(',', a);
        if (b == std::string::npos) { v.push_back(s.substr(a)); break; }
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}
static bool num(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtod(s.c_str(), &e);
    return e && !*e;
}

bool sbsParseLine(const std::string& line, AdsbExtUpdate& u) {
    if (line.compare(0, 4, "MSG,") != 0) return false;
    const std::vector<std::string> f = csv(line);
    if (f.size() < 5) return false;
    const std::string& h = f[4];
    if (h.size() != 6) return false;
    char* e = nullptr;
    const unsigned long icao = strtoul(h.c_str(), &e, 16);
    if (!e || *e || icao == 0) return false;
    u = AdsbExtUpdate();
    u.icao = (uint32_t)icao;
    auto at = [&](size_t i) -> const std::string& { static const std::string none; return i < f.size() ? f[i] : none; };
    double v;
    std::string cs = at(10);
    while (!cs.empty() && cs.back() == ' ') cs.pop_back();
    u.callsign = cs;
    if (num(at(11), v)) { u.hasAlt = true; u.altFt = (int)v; }
    if (num(at(12), v)) { u.hasSpeed = true; u.speedKt = v; }
    if (num(at(13), v)) { u.hasHeading = true; u.headingDeg = v; }
    double la, lo;
    if (num(at(14), la) && num(at(15), lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180) { u.hasPos = true; u.lat = la; u.lon = lo; }
    if (num(at(16), v)) { u.hasVrate = true; u.vrateFpm = (int)v; }
    if (num(at(17), v) && v >= 0 && v <= 7777) { u.hasSquawk = true; u.squawk = (int)v; }
    if (!at(21).empty()) { u.hasGround = true; u.ground = at(21) == "-1" || at(21) == "1"; }
    return true;
}

// ---------------------------------------------------------------- addresses

bool adsbHostIsLan(const std::string& host0) {
    std::string h;
    for (char c : host0) h += (char)tolower((unsigned char)c);
    if (h.size() > 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    if (h.empty()) return false;
    if (h == "localhost" || h == "::1") return true;
    auto ends = [&](const char* s) { const size_t n = strlen(s); return h.size() > n && h.compare(h.size() - n, n, s) == 0; };
    if (ends(".local") || ends(".lan") || ends(".home") || ends(".internal") || ends(".localdomain") || ends(".home.arpa")) return true;
    if (h.find(':') != std::string::npos) return h.compare(0, 4, "fe80") == 0 || h.compare(0, 2, "fc") == 0 || h.compare(0, 2, "fd") == 0;
    int a, b, c, d;
    char x;
    if (sscanf(h.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &x) != 4) return false;   // a public name
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    return a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
}

bool adsbSplitHostPort(const std::string& s0, std::string& host, int& port, std::string& err) {
    std::string s = s0;
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a])) a++;
    s = s.substr(a);
    size_t colon;
    if (!s.empty() && s[0] == '[') { const size_t r = s.find(']'); colon = r == std::string::npos ? r : (r + 1 < s.size() && s[r + 1] == ':' ? r + 1 : std::string::npos); }
    else colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || s.find(':') != colon && s[0] != '[') { err = "write it as host:port, for example 192.168.1.20:30005"; return false; }
    host = s.substr(0, colon);
    if (!host.empty() && host[0] == '[') host = host.substr(1, host.size() - 2);
    char* e = nullptr;
    const long p = strtol(s.c_str() + colon + 1, &e, 10);
    if (!e || *e || p < 1 || p > 65535) { err = "the port must be a number from 1 to 65535"; return false; }
    port = (int)p;
    return true;
}

// ---------------------------------------------------------------- the connections

struct AdsbFeedIn::Impl {
    struct Src {
        std::string spec, host;
        int port = 0;
        std::atomic<bool> stop{false};
        std::atomic<int> state{0};
        std::atomic<uint64_t> msgs{0};
        std::atomic<int> format{0};   // 0 not known yet, 1 Beast, 2 SBS
        std::mutex mu;
        std::string error;
        std::thread th;
    };
    mutable std::mutex mu;
    std::vector<std::unique_ptr<Src>> src;
    Sink sink;

    void run(Src* s, int id) {
        BeastParser bp;
        std::vector<BeastFrame> frames;
        std::string text;
        while (!s->stop) {
            std::string err;
            sock_t k = tcpConnect(s->host, s->port, 4000, err);
            if (k == kBadSock) {
                { std::lock_guard<std::mutex> lk(s->mu); s->error = err; }
                s->state = 0;
                for (int i = 0; i < 30 && !s->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            sockRecvTimeout(k, 500);
            { std::lock_guard<std::mutex> lk(s->mu); s->error.clear(); }
            s->state = 1; s->format = 0; text.clear();
            bp = BeastParser();
            uint8_t buf[4096];
            Sink sk;
            { std::lock_guard<std::mutex> lk(mu); sk = sink; }
            while (!s->stop) {
                const long long n = sockRecv(k, buf, sizeof buf);
                if (n == 0) break;
                if (n < 0) {
#ifdef _WIN32
                    const int e = WSAGetLastError();
                    if (e == WSAETIMEDOUT) continue;
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                    break;
                }
                if (s->format == 0) s->format = buf[0] == 0x1A ? 1 : 2;   // Beast starts with 0x1A, SBS with "MSG,"
                if (s->format == 1) {
                    frames.clear();
                    bp.push(buf, (size_t)n, frames);
                    for (const BeastFrame& f : frames) { s->msgs++; if (sk.frame) sk.frame(id, f.bytes, f.bits); }
                } else {
                    text.append((const char*)buf, (size_t)n);
                    if (text.size() > 65536) text.clear();   // not a text stream at all
                    size_t a = 0, b;
                    while ((b = text.find('\n', a)) != std::string::npos) {
                        std::string line = text.substr(a, b - a);
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                        a = b + 1;
                        AdsbExtUpdate u;
                        if (sbsParseLine(line, u)) { s->msgs++; if (sk.update) sk.update(id, u); }
                    }
                    text.erase(0, a);
                }
            }
            sockClose(k);
            s->state = 0;
            { std::lock_guard<std::mutex> lk(s->mu); s->error = "connection lost, trying again"; }
            for (int i = 0; i < 30 && !s->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

AdsbFeedIn::AdsbFeedIn() : p_(std::make_unique<Impl>()) {}
AdsbFeedIn::~AdsbFeedIn() { stop(); }

void AdsbFeedIn::setSink(Sink s) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = std::move(s); }

void AdsbFeedIn::stop() {
    std::vector<std::unique_ptr<Impl::Src>> v;
    { std::lock_guard<std::mutex> lk(p_->mu); v.swap(p_->src); }
    for (auto& s : v) s->stop = true;
    for (auto& s : v) if (s->th.joinable()) s->th.join();
}

void AdsbFeedIn::setSources(const std::vector<std::string>& specs, bool allowNonLan) {
    // every connection is started again so that the source numbers follow the list (source 1 first)
    std::vector<std::unique_ptr<Impl::Src>> old;
    { std::lock_guard<std::mutex> lk(p_->mu); old.swap(p_->src); }
    std::vector<std::unique_ptr<Impl::Src>> next;
    for (size_t i = 0; i < specs.size(); i++) {
        auto s = std::make_unique<Impl::Src>();
        s->spec = specs[i];
        std::string err;
        if (!adsbSplitHostPort(specs[i], s->host, s->port, err)) s->error = err;
        else if (!allowNonLan && !adsbHostIsLan(s->host)) s->error = "not on the local network (allow other hosts in the options to use it)";
        next.push_back(std::move(s));
    }
    for (auto& o : old) o->stop = true;
    for (auto& o : old) if (o->th.joinable()) o->th.join();
    for (size_t i = 0; i < next.size(); i++) {
        Impl::Src* s = next[i].get();
        if (s->error.empty()) s->th = dect2::namedThread("onair-adsbin", [this, s, i] { p_->run(s, (int)i + 1); });
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->src = std::move(next);
}

std::vector<AdsbSourceInfo> AdsbFeedIn::info() const {
    std::vector<AdsbSourceInfo> v;
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& s : p_->src) {
        AdsbSourceInfo i;
        i.name = s->spec;
        i.format = s->format == 1 ? "Beast" : s->format == 2 ? "SBS" : "";
        i.state = s->state;
        i.msgs = s->msgs;
        { std::lock_guard<std::mutex> l2(s->mu); i.error = s->error; }
        v.push_back(std::move(i));
    }
    return v;
}

} // namespace dect2
