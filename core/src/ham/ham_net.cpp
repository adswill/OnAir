// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The spot feeds of the Ham screen (4.0): see ham_net.h.
#include "dect2/ham/ham_net.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace dect2::ham {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
double nowUtc() { return (double)time(nullptr); }
std::string hhmm(double t) {
    const long long d = ((long long)t % 86400 + 86400) % 86400;
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", (int)(d / 3600), (int)(d % 3600 / 60));
    return b;
}
std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
}  // namespace

// ---- telnet text ----
std::vector<std::string> TelnetLines::feed(const char* d, size_t n) {
    std::vector<std::string> out;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)d[i];
        if (iac_ > 0) { iac_--; continue; }
        if (c == 255) { iac_ = 2; continue; }            // IAC <command> <option>
        if (c == '\r' || c == 0) continue;
        if (c == '\n' || c == 7) {                       // a bell ends a line on some clusters
            if (!cur_.empty()) out.push_back(cur_);
            cur_.clear();
            continue;
        }
        if (cur_.size() < 2048) cur_ += (char)c;
    }
    return out;
}
bool telnetAsksCall(const std::string& t) {
    const std::string l = lower(t);
    return l.find("login:") != std::string::npos || l.find("your call") != std::string::npos || l.find("callsign:") != std::string::npos ||
           l.find("call:") != std::string::npos;
}

// ---- HTTPS GET through curl (the request goes in on stdin, as for the uploads) ----
HttpGet curlGet() {
    return [](const std::string& url) {
        UpResponse r;
        if (url.compare(0, 8, "https://") != 0) { r.err = "refused: not https"; return r; }
        const char* t = getenv("TMPDIR"); if (!t) t = getenv("TEMP"); if (!t) t = "/tmp";
        const std::string stem = std::string(t) + "/onair_spots_" + std::to_string((long long)time(nullptr)) + "_" + std::to_string(rand());
        const std::string outF = stem + ".out", codeF = stem + ".code";
        std::string cfg = "silent\nshow-error\nproto = \"=https\"\nmax-time = 20\nconnect-timeout = 10\nmax-filesize = 2097152\ncompressed\n";
        cfg += "url = \"" + url + "\"\nheader = \"User-Agent: OnAir-ham-spots\"\noutput = \"" + outF + "\"\nwrite-out = \"%output{" + codeF + "}%{http_code}\"\n";
#ifdef _WIN32
        FILE* p = popen("curl --config - >NUL 2>&1", "w");
#else
        FILE* p = popen("curl --config - >/dev/null 2>&1", "w");
#endif
        if (!p) { r.err = "curl not available"; return r; }
        fwrite(cfg.data(), 1, cfg.size(), p);
        pclose(p);
        std::ifstream cf(codeF), of(outF, std::ios::binary);
        std::string code; cf >> code;
        r.status = std::atoi(code.c_str());
        std::ostringstream ss; ss << of.rdbuf(); r.body = ss.str();
        cf.close(); of.close();
        if (!r.status) r.err = "connection failed";
        std::remove(outF.c_str()); std::remove(codeF.c_str());
        return r;
    };
}

std::string potaUrl() { return "https://api.pota.app/spot/activator"; }
std::string sotaUrl() { return "https://api2.sota.org.uk/api/spots/50/all"; }
std::string pskrUrl(const std::string& myCall) {
    return "https://retrieve.pskreporter.info/query?senderCallsign=" + urlenc(myCall) + "&flowStartSeconds=-900&rronly=1";
}

// ---- the feeds ----
void SpotFeeds::setConfig(const SpotFeedConfig& c, bool worker) {
    bool any;
    {
        std::lock_guard<std::mutex> l(mu_);
        cfg_ = c;
        any = !c.myCall.empty() && (c.cluster || c.rbn || c.rbnDigital || c.pskr) ? true : (c.pota || c.sota);
        for (int i = 0; i < kSpotSrcs; i++) status_[i].clear();
    }
    gen_++;
    if (any && worker && !th_.joinable()) { quit_ = false; th_ = std::thread([this] { run(); }); }
    else if (!any || !worker) stop();
}
SpotFeedConfig SpotFeeds::config() const { std::lock_guard<std::mutex> l(mu_); return cfg_; }
void SpotFeeds::stop() {
    quit_ = true;
    if (th_.joinable()) th_.join();
}
std::vector<ExtSpot> SpotFeeds::take() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<ExtSpot> v;
    v.swap(in_);
    return v;
}
std::string SpotFeeds::status(SpotSrc s) const { std::lock_guard<std::mutex> l(mu_); return status_[(int)s]; }
void SpotFeeds::setStatus(SpotSrc s, const std::string& t) { std::lock_guard<std::mutex> l(mu_); status_[(int)s] = t; }
void SpotFeeds::push(const std::vector<ExtSpot>& v) {
    std::lock_guard<std::mutex> l(mu_);
    in_.insert(in_.end(), v.begin(), v.end());
    if (in_.size() > 20000) in_.erase(in_.begin(), in_.begin() + (long)(in_.size() - 20000));
}

void SpotFeeds::pollHttp(double now) {
    const SpotFeedConfig c = config();
    auto poll = [&](bool on, double every, double& last, SpotSrc src, const std::string& url, auto parse) {
        if (!on || now - last < every) return;
        last = now;
        const UpResponse r = get_(url);
        if (r.status < 200 || r.status >= 300) { setStatus(src, "error: " + (r.err.empty() ? "HTTP " + std::to_string(r.status) : r.err)); return; }
        const std::vector<ExtSpot> v = parse(r.body);
        push(v);
        setStatus(src, std::to_string(v.size()) + " spots " + hhmm(now));
    };
    poll(c.pota, c.potaEverySec, lastPota_, SpotSrc::Pota, potaUrl(), [](const std::string& b) { return parsePotaSpots(b); });
    poll(c.sota, c.sotaEverySec, lastSota_, SpotSrc::Sota, sotaUrl(), [](const std::string& b) { return parseSotaSpots(b); });
    poll(c.pskr && !c.myCall.empty(), std::max(300.0, c.pskrEverySec), lastPskr_, SpotSrc::PskReporter, pskrUrl(c.myCall),
         [](const std::string& b) { return parsePskReporterXml(b); });
}

void SpotFeeds::run() {
    std::thread t[3];
    for (int i = 0; i < 3; i++) t[i] = std::thread([this, i] { telnetLoop(i); });
    while (!quit_) {
        pollHttp(nowUtc());
        for (int k = 0; k < 5 && !quit_; k++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    for (auto& x : t) if (x.joinable()) x.join();
}

void SpotFeeds::telnetLoop(int which) {
    const SpotSrc src = which == 0 ? SpotSrc::Cluster : SpotSrc::Rbn;
    auto nap = [this](int ms, int gen) {   // false: quit or the settings changed
        for (int k = 0; k < ms / 200; k++) { if (quit_ || gen_ != gen) return false; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
        return !quit_ && gen_ == gen;
    };
    while (!quit_) {
        const int gen = gen_;
        const SpotFeedConfig c = config();
        bool on = false;
        std::string host;
        int port = 0;
        if (which == 0) { on = c.cluster && !c.clusterHost.empty(); host = c.clusterHost; port = c.clusterPort; }
        else if (which == 1) { on = c.rbn; host = "telnet.reversebeacon.net"; port = 7000; }
        else { on = c.rbnDigital; host = "telnet.reversebeacon.net"; port = 7001; }
        if (!on || c.myCall.empty()) { nap(1000, gen); continue; }
        std::string err;
        const sock_t s = tcpConnect(host, port, 8000, err);
        if (s == kBadSock) { setStatus(src, "error: " + err); nap(30000, gen); continue; }
        setStatus(src, "connected to " + host);
        TelnetLines tl;
        bool sentCall = false;
        const auto t0 = std::chrono::steady_clock::now();
        int spots = 0;
        bool alive = true;
        while (alive && !quit_ && gen_ == gen) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{0, 250000};
            const int n = select((int)s + 1, &rd, nullptr, nullptr, &tv);
            if (n < 0) { alive = false; break; }
            if (n > 0) {
                char buf[4096];
                const long long k = sockRecv(s, buf, sizeof buf);
                if (k <= 0) { alive = false; break; }
                std::vector<ExtSpot> got;
                const double now = nowUtc();
                for (const auto& line : tl.feed(buf, (size_t)k)) {
                    ExtSpot sp;
                    if (parseClusterLine(line, now, sp)) got.push_back(sp);
                    else if (!sentCall && telnetAsksCall(line)) { const std::string l = c.myCall + "\r\n"; sentCall = sockSendAll(s, l.data(), l.size()); }
                }
                if (!sentCall && telnetAsksCall(tl.partial())) { const std::string l = c.myCall + "\r\n"; sentCall = sockSendAll(s, l.data(), l.size()); }
                if (!got.empty()) {
                    spots += (int)got.size();
                    push(got);
                    setStatus(src, std::to_string(spots) + " spots, last " + hhmm(now));
                }
            }
            if (!sentCall && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {   // no prompt seen: log in anyway
                const std::string l = c.myCall + "\r\n";
                sentCall = sockSendAll(s, l.data(), l.size());
            }
        }
        sockShutdown(s);
        sockClose(s);
        if (quit_ || gen_ != gen) continue;
        setStatus(src, "disconnected from " + host + ", trying again");
        nap(15000, gen);
    }
}

}  // namespace dect2::ham
