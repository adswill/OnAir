#include "dect2/net/aprs_gate.h"
#include "dect2/net/feeds.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dect2 {

const char* aprsGateVerdictText(AprsGateVerdict v) {
    static const char* const t[(int)AprsGateVerdict::kCount] = {"gated", "no callsign", "callsign not valid", "not a UI frame", "empty", "third-party packet", "query", "NOGATE / RFONLY / TCPIP in the path",
                                                                "came from the Internet", "own packet or already passed this iGate", "source is not a valid callsign", "too long", "control characters",
                                                                "duplicate", "rate limit", "per-station rate limit"};
    return (int)v >= 0 && (int)v < (int)AprsGateVerdict::kCount ? t[(int)v] : "";
}

bool aprsValidCallsign(const std::string& c) {
    if (c.empty() || c.size() > 9) return false;
    std::string base = c;
    const size_t d = c.find('-');
    if (d != std::string::npos) {
        base = c.substr(0, d);
        const std::string ssid = c.substr(d + 1);
        if (ssid.empty() || ssid.size() > 2) return false;
        for (char ch : ssid) if (!isdigit((unsigned char)ch)) return false;
        if (atoi(ssid.c_str()) > 15) return false;
    }
    if (base.size() < 3 || base.size() > 6) return false;
    bool dig = false, let = false;
    for (char ch : base) {
        if (isdigit((unsigned char)ch)) dig = true;
        else if (isalpha((unsigned char)ch) && ch >= 0) let = true;
        else return false;
    }
    return dig && let;
}

std::string aprsIsLoginRedacted(const std::string& call) {
    std::string u = call;
    for (char& ch : u) ch = (char)toupper((unsigned char)ch);
    return "user " + u + " pass ****";
}

void AprsGate::configure(const AprsGateConfig& c) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    cfg_.maxPerMin = std::max(0, std::min(cfg_.maxPerMin, 6000));
    cfg_.perStationPerMin = std::max(0, std::min(cfg_.perStationPerMin, 600));
    cfg_.dedupSec = std::max(0, std::min(cfg_.dedupSec, 600));
}

void AprsGate::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    seen_.clear(); all_.clear(); perStation_.clear();
}

AprsGateStats AprsGate::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

std::string AprsGate::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    char b[160];
    snprintf(b, sizeof b, "gated %llu, duplicates %llu, rate-limited %llu, not gateable %llu", (unsigned long long)st_.gated, (unsigned long long)st_.duplicates,
             (unsigned long long)(st_.rateLimited + st_.stationLimited), (unsigned long long)st_.notGateable);
    return b;
}

static std::string up(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }

AprsGateVerdict AprsGate::decide(const std::string& from, const std::string& to, const std::string& path, const std::string& info, const std::string& myCall,
                                 bool uiF0, double now, std::string* line) {
    std::lock_guard<std::mutex> lk(mu_);
    auto fin = [&](AprsGateVerdict v) {
        st_.byVerdict[(int)v]++;
        if (v == AprsGateVerdict::Gate) st_.gated++;
        else if (v == AprsGateVerdict::Duplicate) st_.duplicates++;
        else if (v == AprsGateVerdict::RateLimited) st_.rateLimited++;
        else if (v == AprsGateVerdict::StationLimited) st_.stationLimited++;
        else st_.notGateable++;
        return v;
    };
    if (line) line->clear();
    if (myCall.empty()) return fin(AprsGateVerdict::NoCall);
    const std::string me = up(myCall);
    if (!aprsValidCallsign(me)) return fin(AprsGateVerdict::BadCall);
    if (!uiF0) return fin(AprsGateVerdict::NotUi);
    if (info.find('\0') != std::string::npos) return fin(AprsGateVerdict::BadChars);
    std::string body = info;
    const size_t cut = body.find_first_of("\r\n");
    if (cut != std::string::npos) body.resize(cut);
    if (body.empty()) return fin(AprsGateVerdict::Empty);
    if (body[0] == '}') return fin(AprsGateVerdict::ThirdParty);
    if (body[0] == '?') return fin(AprsGateVerdict::Query);
    const std::string src = up(from);
    if (!aprsValidCallsign(src)) return fin(AprsGateVerdict::BadSource);
    if (src == me) return fin(AprsGateVerdict::OwnPacket);
    // the path: a q construct or NOGATE and friends keep it off the Internet; our own call in it means it already went through us
    {
        size_t s = 0;
        while (!path.empty() && s <= path.size()) {
            size_t e = path.find(',', s);
            if (e == std::string::npos) e = path.size();
            std::string tok = up(path.substr(s, e - s));
            if (!tok.empty() && tok.back() == '*') tok.pop_back();
            if (tok == "TCPIP" || tok == "TCPXX" || tok == "NOGATE" || tok == "RFONLY" || tok.rfind("TCPIP", 0) == 0) return fin(AprsGateVerdict::NoGatePath);
            if (tok.size() == 3 && tok[0] == 'Q' && tok[1] == 'A') return fin(AprsGateVerdict::FromInternet);
            if (tok == me) return fin(AprsGateVerdict::OwnPacket);
            s = e + 1;
            if (e == path.size()) break;
        }
    }
    const std::string l = feedfmt::aprsIsLine(from, to, path, info, myCall, uiF0);
    if (l.empty()) return fin(AprsGateVerdict::NotUi);
    if (l.size() > 512) return fin(AprsGateVerdict::TooLong);
    // the same packet heard again (a digipeater repeating it, or two receivers): once per window
    const std::string key = src + ">" + up(to) + ":" + body;
    if (now - lastPrune_ > 30) {
        lastPrune_ = now;
        for (auto it = seen_.begin(); it != seen_.end();) { if (now - it->second > std::max(1, cfg_.dedupSec) || now < it->second) it = seen_.erase(it); else ++it; }
        for (auto it = perStation_.begin(); it != perStation_.end();) {
            while (!it->second.empty() && now - it->second.front() >= 60) it->second.pop_front();
            if (it->second.empty()) it = perStation_.erase(it); else ++it;
        }
    }
    if (cfg_.dedupSec > 0) {
        auto it = seen_.find(key);
        if (it != seen_.end() && now >= it->second && now - it->second < cfg_.dedupSec) return fin(AprsGateVerdict::Duplicate);
    }
    while (!all_.empty() && now - all_.front() >= 60) all_.pop_front();
    if (cfg_.maxPerMin > 0 && (int)all_.size() >= cfg_.maxPerMin) return fin(AprsGateVerdict::RateLimited);
    if (cfg_.perStationPerMin > 0) {
        std::deque<double>& q = perStation_[src];
        while (!q.empty() && now - q.front() >= 60) q.pop_front();
        if ((int)q.size() >= cfg_.perStationPerMin) return fin(AprsGateVerdict::StationLimited);
        q.push_back(now);
    }
    all_.push_back(now);
    if (seen_.size() < 20000) seen_[key] = now;
    if (line) *line = l;
    return fin(AprsGateVerdict::Gate);
}

} // namespace dect2
