// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde upload to APRS-IS aggregators (sonde_upload.h): formats, one thread, a socket per enabled target.
#include "dect2/net/sonde_upload.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "dect2/net/aprs_gate.h"
#include "tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2 {

const std::vector<SondeUploadPreset>& sondeUploadPresets() {
    static const std::vector<SondeUploadPreset> p = {
        {"wettersonde.net", "wettersonde.net", 14580, "APRS-IS port of wettersonde.net (as given by its users; edit if the site tells you another)."},
        {"radiosondy.info", "radiosondy.info", 14580, "Host and port are the usual APRS-IS ones and were not confirmed on the site: check them against radiosondy.info and edit if needed."},
        {"Custom APRS-IS server", "", 14580, "Any APRS-IS server that takes a client login."},
    };
    return p;
}

namespace sondeup {
namespace {
std::string upperStr(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
bool allDigits(const std::string& s) { if (s.empty()) return false; for (char c : s) if (c < '0' || c > '9') return false; return true; }
std::string pad9(std::string s) { if (s.size() > 9) s.resize(9); while (s.size() < 9) s += ' '; return s; }
std::string lastN(const std::string& s, size_t n) { return s.size() > n ? s.substr(s.size() - n) : s; }
std::string hexUp(unsigned long long v, size_t width) {
    char b[24]; snprintf(b, sizeof b, "%0*llX", (int)width, v);
    return lastN(b, width);
}
// "12-34" style pieces after the last '-' as an integer; false when not all digits
bool numAfterDash(const std::string& ser, unsigned long long& v) {
    const size_t d = ser.rfind('-');
    const std::string t = d == std::string::npos ? ser : ser.substr(d + 1);
    if (!allDigits(t) || t.size() > 18) return false;
    v = std::strtoull(t.c_str(), nullptr, 10);
    return true;
}
std::string hundredths(double deg, int degWidth) {
    const double a = std::min(std::fabs(deg), degWidth == 2 ? 90.0 : 180.0);
    const long long h = (long long)std::llround(a * 6000.0);
    char b[24]; snprintf(b, sizeof b, "%0*lld%02lld.%02lld", degWidth, h / 6000, (h % 6000) / 100, h % 100);
    return b;
}
}

std::string loginLine(const std::string& call, const std::string& pass) {
    return "user " + upperStr(call) + " pass " + (pass.empty() ? std::string("-1") : pass) + " vers OnAir " ONAIR_VERSION "\r\n";
}
std::string latText(double lat) { return hundredths(lat, 2) + (lat < 0 ? "S" : "N"); }
std::string lonText(double lon) { return hundredths(lon, 3) + (lon < 0 ? "W" : "E"); }

std::string beaconLine(const std::string& call, double lat, double lon, const std::string& comment) {
    if (!(std::fabs(lat) <= 90) || !(std::fabs(lon) <= 180)) return {};
    return upperStr(call) + ">APRS,TCPIP*:!" + latText(lat) + "/" + lonText(lon) + "&" + comment + "\r\n";
}

std::string objectName(const SondeInfo& s) {
    std::string ser = s.serial;
    while (!ser.empty() && ser.back() == ' ') ser.pop_back();
    if (ser.empty() || ser.find('?') != std::string::npos) return {};
    unsigned long long v = 0;
    switch (s.kind) {
    case SondeRs41: case SondeRs92:
        for (char c : ser) if (!std::isalnum((unsigned char)c)) return {};
        return pad9(upperStr(ser));
    case SondeDfm:
        if (!allDigits(ser) || ser.size() > 18) return {};
        return pad9("D" + std::to_string(std::strtoull(ser.c_str(), nullptr, 10)));
    case SondeLms6:
        if (!numAfterDash(ser, v)) return {};
        return pad9("LMS6" + hexUp(v, 5));
    case SondeMeisei:
        if (!numAfterDash(ser, v)) return {};
        return pad9("IMS" + hexUp(v, 6));
    case SondeMrz: {   // "MRZ-5667-39155": the two numbers joined, the low 6 hex digits
        const size_t a = ser.find('-'), b = a == std::string::npos ? a : ser.find('-', a + 1);
        if (b == std::string::npos) return {};
        const std::string x = ser.substr(a + 1, b - a - 1), y = ser.substr(b + 1);
        if (!allDigits(x) || !allDigits(y) || x.size() + y.size() > 18) return {};
        return pad9("MRZ" + hexUp(std::strtoull((x + y).c_str(), nullptr, 10), 6));
    }
    case SondeImet54:
        if (!numAfterDash(ser, v)) return {};
        return pad9("IMET" + lastN(std::to_string(v), 5));
    default: return {};   // M10, M20, iMet-1/4 and unknown types
    }
}

std::string typeText(const SondeInfo& s) {
    if (s.kind == SondeMeisei) return "IMS100";
    if ((s.kind == SondeRs41 || s.kind == SondeRs92) && s.subtype.compare(0, s.type.size(), s.type) == 0 && !s.subtype.empty()) return s.subtype;
    return s.type.empty() ? std::string(sondeKindName(s.kind)) : s.type;
}

std::string objectLine(const std::string& call, const SondeInfo& s, double wallUtc, std::string* why) {
    auto no = [&](const char* r) { if (why) *why = r; return std::string(); };
    if (!s.hasPos || !(std::fabs(s.lat) <= 90) || !(std::fabs(s.lon) <= 180) || (s.lat == 0 && s.lon == 0) || !(s.altM > -1000 && s.altM < 100000)) return no("no valid position");
    const std::string name = objectName(s);
    if (name.empty()) return no("no APRS object name for this sonde type");
    const double t = s.hasTime && s.unixTime > 0 ? s.unixTime : wallUtc;
    struct tm g; gmTime((time_t)std::floor(t), &g);
    char b[64];
    std::string o = upperStr(call) + ">APRS,TCPIP*:;" + name + "*";
    snprintf(b, sizeof b, "%02d%02d%02dh", g.tm_hour, g.tm_min, g.tm_sec); o += b;
    o += latText(s.lat) + "/" + lonText(s.lon) + "O";
    if (s.hasVel) {
        int crs = (int)std::lround(s.headingDeg); crs = ((crs % 360) + 360) % 360; if (crs == 0) crs = 360;   // APRS: 1 - 360, 0 means unknown
        const int kn = std::max(0, std::min(999, (int)std::lround(s.hSpeed * 1.9438445)));
        snprintf(b, sizeof b, "%03d/%03d", crs, kn); o += b;
    }
    snprintf(b, sizeof b, "/A=%06d", (int)std::lround(s.altM / 0.3048)); o += b;
    if (s.hasVel) { snprintf(b, sizeof b, " Clb=%.1fm/s", s.vSpeed); o += b; }
    if (s.hasTemp) { snprintf(b, sizeof b, " t=%.1fC", s.tempC); o += b; }
    if (s.hasHumidity) { snprintf(b, sizeof b, " h=%.1f%%", s.humidity); o += b; }
    if (s.hasPressure) { snprintf(b, sizeof b, " p=%.1fhPa", s.pressureHpa); o += b; }
    if (s.freqHz > 0) { snprintf(b, sizeof b, " %.3fMHz", s.freqHz / 1e6); o += b; }
    o += " Type=" + typeText(s) + "\r\n";
    return o;
}
} // namespace sondeup

namespace {
constexpr size_t kMaxQueue = 512, kMaxPending = 256;
}

struct SondeUpload::Impl {
    struct Item { SondeInfo s; double wall; };
    struct Live {
        sock_t s = kBadSock;
        std::deque<std::string> out; size_t off = 0;
        std::string rx;
        double retryAt = 0, wait = 2, nextBeacon = 0;
        bool answered = false, verified = false;
    };
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> q;
    std::vector<SondeUploadTarget> cfg, live;
    std::vector<SondeUploadStatus> pub, st;
    std::vector<Live> L;
    SondeUploadTiming tm, tl;                 // tm: set by callers under mu; tl: the thread's copy
    double cLat = 0, cLon = 0; bool cSet = false;   // the thread's copy of the station
    double stLat = 0, stLon = 0; bool stSet = false;
    std::map<std::string, double> lastSent;   // object name -> steady time
    bool dirty = false, quit = false;
    std::thread th;

    static bool same(const SondeUploadTarget& a, const SondeUploadTarget& b) {
        return a.enabled && b.enabled && a.host == b.host && a.port == b.port && a.call == b.call && a.pass == b.pass;
    }
    void closeOne(size_t i) {
        if (L[i].s != kBadSock) sockClose(L[i].s);
        L[i] = Live(); st[i].connected = false; st[i].verified = false;
    }
    void applyConfig() {
        std::vector<Live> nl(cfg.size());
        std::vector<SondeUploadStatus> ns(cfg.size());
        std::vector<bool> used(live.size(), false);
        for (size_t i = 0; i < cfg.size(); i++)
            for (size_t j = 0; j < live.size(); j++)
                if (!used[j] && same(live[j], cfg[i])) { nl[i] = L[j]; ns[i] = st[j]; used[j] = true; L[j].s = kBadSock; break; }
        for (Live& l : L) if (l.s != kBadSock) sockClose(l.s);
        L = std::move(nl); st = std::move(ns); live = cfg;
        for (size_t i = 0; i < live.size(); i++) if (!live[i].enabled) { st[i] = SondeUploadStatus(); st[i].state = "off"; }
    }
    void queueBeacon(size_t i, double t) {
        const SondeUploadTarget& tg = live[i];
        Live& l = L[i];
        if (!cSet) return;
        const std::string b = sondeup::beaconLine(tg.call, cLat, cLon);
        if (!b.empty()) { l.out.push_back(b); l.nextBeacon = t + (tl.beaconEveryS > 0 ? tl.beaconEveryS : std::max(1, std::min(120, tg.beaconMin)) * 60.0); }
    }
    void service(size_t i, double t) {
        const SondeUploadTarget& tg = live[i];
        Live& l = L[i]; SondeUploadStatus& s = st[i];
        if (!tg.enabled) return;
        if (tg.host.empty() || tg.port <= 0 || tg.port > 65535) { s.state = "needs a host and a port"; return; }
        if (!aprsValidCallsign(tg.call)) { s.state = "needs a valid callsign (like DG9BFC-10)"; return; }
        if (l.s == kBadSock) {
            if (t < l.retryAt) { s.state = "waiting to reconnect"; return; }
            s.state = "connecting to " + tg.host;
            std::string err;
            sock_t c = tcpConnect(tg.host, tg.port, 3000, err);
            if (c == kBadSock) { s.lastError = err; s.state = "waiting to reconnect"; l.retryAt = t + l.wait; l.wait = std::min(tl.reconnectMaxS, l.wait * 2); return; }
            sockSetNonBlocking(c, true);
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            l.s = c; l.wait = tl.reconnectMinS; l.answered = l.verified = false; l.rx.clear(); l.out.clear(); l.off = 0;
            s.connected = true; s.verified = false; s.lastError.clear();
            l.out.push_back(sondeup::loginLine(tg.call, tg.pass));
            l.nextBeacon = 0;
        }
        if (cSet && t >= l.nextBeacon) queueBeacon(i, t);
        s.state = "connected to " + tg.host + ":" + std::to_string(tg.port) + (l.verified ? ", verified" : l.answered ? ", unverified" : "") + (cSet ? "" : " (set your location for the station beacon)");
    }
    void parseRx(size_t i) {
        Live& l = L[i]; SondeUploadStatus& s = st[i];
        size_t p;
        while ((p = l.rx.find('\n')) != std::string::npos) {
            std::string ln = l.rx.substr(0, p); l.rx.erase(0, p + 1);
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            if (ln.compare(0, 9, "# logresp") != 0) continue;
            l.answered = true;
            if (ln.find(" unverified") != std::string::npos) {
                l.verified = false; s.verified = false;
                s.lastError = "the server did not accept the passcode (unverified): sonde lines are not sent";
            } else if (ln.find(" verified") != std::string::npos) { l.verified = true; s.verified = true; s.lastError.clear(); }
        }
    }
    void flush(size_t i, double t) {
        Live& l = L[i]; SondeUploadStatus& s = st[i];
        if (l.s == kBadSock) return;
        bool dead = false;
        while (!l.out.empty() && !dead) {
            const std::string& ln = l.out.front();
#if defined(MSG_NOSIGNAL)
            const int fl = MSG_NOSIGNAL;
#else
            const int fl = 0;
#endif
            const long long k = send(l.s, ln.data() + l.off, (int)(ln.size() - l.off), fl);
            if (k > 0) {
                l.off += (size_t)k;
                if (l.off >= ln.size()) {
                    std::string line = ln.substr(0, ln.size() >= 2 ? ln.size() - 2 : ln.size());
                    if (ln.compare(0, 5, "user ") == 0) line = "user " + live[i].call + " pass ****";   // never the passcode
                    else if (ln.find(":!") != std::string::npos) s.beacons++;
                    else s.sent++;
                    s.lastLine = line;
                    l.out.pop_front(); l.off = 0;
                }
            } else if (k < 0) {
#ifdef _WIN32
                const int e = WSAGetLastError(); dead = !(e == WSAEWOULDBLOCK || e == WSAEINTR);
#else
                dead = !(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
                break;
            } else break;
        }
        char b[512];
        for (int n = 0; n < 8 && !dead; n++) {
            const long long r = recv(l.s, b, sizeof b, 0);
            if (r == 0) dead = true;
            else if (r < 0) break;
            else { l.rx.append(b, (size_t)r); if (l.rx.size() > 8192) l.rx.erase(0, l.rx.size() - 4096); }
        }
        parseRx(i);
        if (dead) {
            closeOne(i); s.lastError = "the server closed the connection"; s.reconnects++;
            L[i].retryAt = t + tl.reconnectMinS; L[i].wait = std::min(tl.reconnectMaxS, tl.reconnectMinS * 2);
        }
    }
    void deliver(const Item& it) {
        for (size_t i = 0; i < live.size(); i++) {
            const SondeUploadTarget& tg = live[i];
            if (!tg.enabled) continue;
            Live& l = L[i]; SondeUploadStatus& s = st[i];
            const std::string line = sondeup::objectLine(tg.call, it.s, it.wall);
            if (line.empty()) continue;
            if (l.s == kBadSock || (l.answered && !l.verified) || l.out.size() >= kMaxPending) { s.dropped++; continue; }
            l.out.push_back(line);
        }
    }
    void loop() {
        for (;;) {
            std::deque<Item> batch;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(150), [&] { return quit || !q.empty() || dirty; });
                if (quit) break;
                batch.swap(q);
                tl = tm; cLat = stLat; cLon = stLon; cSet = stSet;
                if (dirty) { applyConfig(); dirty = false; }
            }
            const double t = steadyNowSec();
            for (size_t i = 0; i < live.size(); i++) service(i, t);
            for (const Item& it : batch) deliver(it);
            for (size_t i = 0; i < live.size(); i++) flush(i, t);
            std::lock_guard<std::mutex> lk(mu);
            pub = st;
        }
        for (size_t i = 0; i < L.size(); i++) closeOne(i);
        L.clear(); st.clear(); live.clear();
    }
};

SondeUpload& SondeUpload::get() { static SondeUpload* f = new SondeUpload(); return *f; }   // never destroyed: the receiver may still report at exit
SondeUpload::SondeUpload() : impl_(new Impl) {}

void SondeUpload::configure(const std::vector<SondeUploadTarget>& t) {
    netInit();
    Impl& I = *impl_;
    bool any = false;
    for (const SondeUploadTarget& x : t) any = any || x.enabled;
    want_.store(any);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.cfg = t; I.dirty = true; I.quit = false;
        I.pub.assign(t.size(), SondeUploadStatus());
        for (auto& p : I.pub) p.state = "starting";
    }
    if (any && !I.th.joinable()) I.th = namedThread("onair-sondeup", [&I] { I.loop(); });
    I.cv.notify_one();
}
std::vector<SondeUploadTarget> SondeUpload::targets() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->cfg; }
SondeUploadStatus SondeUpload::status(size_t i) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    SondeUploadStatus s;
    if (i >= impl_->cfg.size() || !impl_->cfg[i].enabled) { s.state = "off"; return s; }
    return i < impl_->pub.size() ? impl_->pub[i] : s;
}
void SondeUpload::setStation(double lat, double lon, bool set) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->stLat = lat; impl_->stLon = lon; impl_->stSet = set;
}
void SondeUpload::setTiming(const SondeUploadTiming& t) { std::lock_guard<std::mutex> lk(impl_->mu); impl_->tm = t; }

void SondeUpload::frame(const SondeInfo& s, double wallUtc) {
    if (!wanted() || !s.hasPos) return;
    Impl& I = *impl_;
    const std::string name = sondeup::objectName(s);
    if (name.empty()) return;
    const double t = steadyNowSec();
    {
        std::lock_guard<std::mutex> lk(I.mu);
        auto it = I.lastSent.find(name);
        if (it != I.lastSent.end() && t - it->second < I.tm.objectGapS) return;
        if (I.q.size() >= kMaxQueue) return;
        if (I.lastSent.size() > 4096) I.lastSent.clear();
        I.lastSent[name] = t;
        I.q.push_back({s, wallUtc});
    }
    I.cv.notify_one();
}

void SondeUpload::stop() {
    Impl& I = *impl_;
    want_.store(false);
    { std::lock_guard<std::mutex> lk(I.mu); I.quit = true; I.q.clear(); I.cfg.clear(); I.pub.clear(); }
    I.cv.notify_one();
    if (I.th.joinable()) I.th.join();
    I.dirty = false;
}

} // namespace dect2
