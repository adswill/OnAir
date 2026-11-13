// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ham/ham_upload.h"
#include "dect2/common/thread_name.h"
#include "dect2/net/wsprnet.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace dect2::ham {
namespace {
std::string enc(const std::string& s) { return wsprnet::urlEncodeForm(s); }
std::string pair(const char* k, const std::string& v) { return std::string(k) + "=" + enc(v); }
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
std::string adifField(const char* n, const std::string& v) { return std::string("<") + n + ":" + std::to_string(v.size()) + ">" + v; }
bool isHttps(const std::string& u) { return u.compare(0, 8, "https://") == 0; }
bool isLoopback(const std::string& u) { return u.compare(0, 17, "http://127.0.0.1:") == 0; }
std::string oneLine(std::string a) { a = clean(a); while (!a.empty() && a.back() == ' ') a.pop_back(); return a; }
}  // namespace

const char* upServiceName(UpService s) { return s == UpService::Qrz ? "QRZ Logbook" : s == UpService::ClubLog ? "Club Log" : "eQSL"; }

bool UpConfig::ready(UpService s) const {
    if (!on[(int)s]) return false;
    switch (s) {
        case UpService::Qrz: return !qrzKey.empty();
        case UpService::ClubLog: return !clubEmail.empty() && !clubPassword.empty() && !clubApiKey.empty();
        case UpService::Eqsl: return !eqslUser.empty() && !eqslPassword.empty();
    }
    return false;
}

UpRequest buildUpRequest(UpService s, const UpConfig& c, const QsoRecord& q) {
    const std::string adif = adifRecord(q);
    UpRequest r;
    if (s == UpService::Qrz) {   // QRZ Logbook API: POST KEY, ACTION=INSERT, ADIF
        r.url = "https://logbook.qrz.com/api";
        r.body = pair("KEY", c.qrzKey) + "&" + pair("ACTION", "INSERT") + "&" + pair("ADIF", adif);
    } else if (s == UpService::ClubLog) {   // Club Log realtime API: POST email, password, callsign, api, adif
        r.url = "https://clublog.org/realtime.php";
        r.body = pair("email", c.clubEmail) + "&" + pair("password", c.clubPassword) + "&" + pair("callsign", q.myCall) + "&" + pair("api", c.clubApiKey) + "&" + pair("adif", adif);
    } else {   // eQSL ImportADIF: the account goes into the ADIF header as EQSL_USER / EQSL_PSWD
        r.url = "https://www.eqsl.cc/qslcard/ImportADIF.cfm";
        r.body = pair("ADIFData", "OnAir upload\n" + adifField("EQSL_USER", c.eqslUser) + adifField("EQSL_PSWD", c.eqslPassword) + "<EOH>\n" + adif);
    }
    return r;
}

UpOutcome parseUpResponse(UpService s, const UpResponse& r) {
    UpOutcome o;
    if (r.status == 0) { o.reason = r.err.empty() ? "no answer" : r.err; return o; }   // transport failure: retry
    const std::string b = lower(r.body);
    if (s == UpService::Qrz) {
        if (b.find("result=ok") != std::string::npos || b.find("result=replace") != std::string::npos) { o.ok = true; return o; }
        const size_t p = b.find("reason=");
        o.reason = p != std::string::npos ? oneLine(r.body.substr(p + 7, 80)) : "HTTP " + std::to_string(r.status);
        o.permanent = r.status < 500;
        return o;
    }
    if (s == UpService::ClubLog) {
        if (r.status == 200) { o.ok = true; return o; }
        o.reason = "HTTP " + std::to_string(r.status) + (r.body.empty() ? "" : ": " + oneLine(r.body.substr(0, 80)));
        o.permanent = r.status >= 400 && r.status < 500;
        return o;
    }
    if (r.status >= 200 && r.status < 300 && (b.find("result: 1 out of 1 records added") != std::string::npos || b.find("duplicate") != std::string::npos)) { o.ok = true; return o; }
    const size_t p = b.find("error:");
    o.reason = p != std::string::npos ? oneLine(r.body.substr(p, 80)) : (r.status >= 500 ? "HTTP " + std::to_string(r.status) : "rejected by eQSL");
    o.permanent = r.status < 500;
    return o;
}

// curl reads its whole request (URL, body) from stdin, so credentials never appear in the process list. https only, no redirects, certificates verified.
UpTransport curlTransport() {
    return [](const std::string& url, const std::string& body) {
        UpResponse r;
        if (!isHttps(url)) { r.err = "refused: not https"; return r; }
        const char* t = getenv("TMPDIR"); if (!t) t = getenv("TEMP"); if (!t) t = "/tmp";
        const std::string stem = std::string(t) + "/onair_up_" + std::to_string((long long)time(nullptr)) + "_" + std::to_string(rand());
        const std::string outF = stem + ".out", codeF = stem + ".code";
        std::string cfg = "silent\nshow-error\nproto = \"=https\"\nmax-time = 30\nconnect-timeout = 15\nmax-filesize = 65536\n";
        cfg += "url = \"" + url + "\"\nheader = \"Content-Type: application/x-www-form-urlencoded\"\nheader = \"User-Agent: OnAir-ham-upload\"\n";
        cfg += "data = \"" + body + "\"\noutput = \"" + outF + "\"\nwrite-out = \"%output{" + codeF + "}%{http_code}\"\n";
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
        r.status = atoi(code.c_str());
        std::ostringstream ss; ss << of.rdbuf(); r.body = ss.str();
        if (!r.status) r.err = "connection failed";
        std::remove(outF.c_str()); std::remove(codeF.c_str());
        return r;
    };
}

void HamUploader::setConfig(const UpConfig& c) { std::lock_guard<std::mutex> lk(mu_); cfg_ = c; }
UpConfig HamUploader::config() const { std::lock_guard<std::mutex> lk(mu_); return cfg_; }
std::vector<UpItem> HamUploader::items() const { std::lock_guard<std::mutex> lk(mu_); return q_; }
size_t HamUploader::pendingCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const UpItem& i : q_) for (int k = 0; k < kUpServices; k++) if (cfg_.ready((UpService)k) && i.s[k].st == 'p') { n++; break; }
    return n;
}

// persisted line: the ADIF record on one line, then per service TAB state:attempts:nextTry:reason
bool HamUploader::save() {
    if (path_.empty()) return true;
    {
        std::ofstream o(path_ + ".tmp", std::ios::trunc);
        for (const UpItem& i : q_) {
            o << clean(adifRecord(i.q));
            for (int k = 0; k < kUpServices; k++) o << '\t' << i.s[k].st << ':' << i.s[k].attempts << ':' << i.s[k].nextTry << ':' << clean(i.s[k].reason);
            o << '\n';
        }
        o.flush();
        if (!o) return false;
    }
    std::remove(path_.c_str());
    return std::rename((path_ + ".tmp").c_str(), path_.c_str()) == 0;
}
bool HamUploader::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    q_.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> parts;
        size_t a = 0;
        for (;;) { const size_t t = line.find('\t', a); parts.push_back(line.substr(a, t == std::string::npos ? t : t - a)); if (t == std::string::npos) break; a = t + 1; }
        const auto recs = adifParse(parts[0]);
        if (recs.empty()) continue;
        UpItem it; it.q = recs[0];
        for (int k = 0; k < kUpServices && k + 1 < (int)parts.size(); k++) {
            const std::string& p = parts[k + 1];
            if (p.size() < 2) continue;
            it.s[k].st = (p[0] == 'u' || p[0] == 'f' || p[0] == 'n') ? p[0] : 'p';
            const size_t c1 = p.find(':', 2), c2 = c1 == std::string::npos ? c1 : p.find(':', c1 + 1);
            it.s[k].attempts = atoi(p.c_str() + 2);
            if (c1 != std::string::npos) it.s[k].nextTry = atoll(p.c_str() + c1 + 1);
            if (c2 != std::string::npos) it.s[k].reason = p.substr(c2 + 1);
        }
        q_.push_back(it);
    }
    return true;
}

void HamUploader::enqueue(const QsoRecord& q, long long now) {
    std::lock_guard<std::mutex> lk(mu_);
    UpItem it; it.q = q;
    for (int k = 0; k < kUpServices; k++) { it.s[k].nextTry = now; if (!cfg_.on[k]) { it.s[k].st = 'n'; it.s[k].reason = "service off when logged"; } }
    q_.push_back(it);
    while (q_.size() > 2000) q_.erase(q_.begin());
    save();
}

void HamUploader::uploadNow(long long now) {
    std::lock_guard<std::mutex> lk(mu_);
    for (UpItem& i : q_) for (int k = 0; k < kUpServices; k++) {
        UpState& s = i.s[k];
        if (s.st == 'u' || !cfg_.ready((UpService)k)) continue;
        s.st = 'p'; s.nextTry = now; s.attempts = 0; s.force = true;
    }
    for (long long& l : lastReq_) l = 0;
    save();
}

int HamUploader::poll(long long now) {
    int made = 0;
    for (int k = 0; k < kUpServices; k++) {
        const UpService sv = (UpService)k;
        UpConfig c; QsoRecord rec; size_t idx = (size_t)-1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            c = cfg_;
            if (!c.ready(sv)) continue;                        // off or no credentials: nothing leaves the machine
            if (lastReq_[k] && now - lastReq_[k] < kMinGapSec) continue;
            for (size_t i = 0; i < q_.size(); i++) {
                const UpState& s = q_[i].s[k];
                if (s.st == 'p' && (c.autoUpload || s.force) && s.nextTry <= now && s.attempts < kMaxAttempts) { idx = i; rec = q_[i].q; break; }
            }
        }
        if (idx == (size_t)-1) continue;
        const UpRequest rq = buildUpRequest(sv, c, rec);
        UpResponse rs;
        if (isHttps(rq.url) || (loopback_ && isLoopback(rq.url))) rs = tr_(rq.url, rq.body); else rs.err = "refused: not https";
        const UpOutcome oc = parseUpResponse(sv, rs);
        made++;
        std::lock_guard<std::mutex> lk(mu_);
        lastReq_[k] = now;
        if (idx >= q_.size()) continue;
        UpState& s = q_[idx].s[k];
        s.attempts++;
        if (oc.ok) { s.st = 'u'; s.reason.clear(); s.force = false; }
        else {
            s.reason = oc.reason;
            if (oc.permanent || s.attempts >= kMaxAttempts) { s.st = 'f'; s.force = false; }
            else s.nextTry = now + 60LL * (1 << std::min(s.attempts, 6));
        }
        save();
    }
    return made;
}

void HamUploader::startWorker() {
    if (th_.joinable()) return;
    quit_ = false;
    th_ = dect2::namedThread("onair-hamup", [this] {
        while (!quit_) {
            poll((long long)time(nullptr));
            for (int i = 0; i < 10 && !quit_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });
}
void HamUploader::stopWorker() { quit_ = true; if (th_.joinable()) th_.join(); }

}  // namespace dect2::ham
