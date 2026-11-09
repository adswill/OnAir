#include "dect2/fm/fm_dx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace dect2 {

// ECC E0..E4, country nibble 1..F (IEC 62106 Annex D, Region 1; "" where the standard leaves the place empty)
static const char* const kCc[5][15] = {
    {"DE","DZ","AD","IL","IT","BE","RU","PS","AL","AT","HU","MT","DE","","EG"},
    {"GR","CY","SM","CH","JO","FI","LU","BG","DK","GI","IQ","GB","LY","RO","FR"},
    {"MA","CZ","PL","VA","SK","SY","TN","","LI","IS","MC","LT","RS","ES","NO"},
    {"ME","IE","TR","MK","","","","NL","LV","LB","AZ","HR","KZ","SE","BY"},
    {"MD","EE","KG","","","UA","XK","PT","SI","AM","","GE","","","BA"},
};

std::string rdsCountry(int pi, int ecc) {
    const int n = (pi >> 12) & 0xF;
    if (n == 0 || ecc < 0xE0 || ecc > 0xE4) return "";
    return kCc[ecc - 0xE0][n - 1];
}

std::vector<std::string> rdsCountriesLikely(int pi) {
    std::vector<std::string> v;
    const int n = (pi >> 12) & 0xF;
    if (n == 0) return v;
    for (auto& row : kCc) if (row[n - 1][0] && std::find(v.begin(), v.end(), row[n - 1]) == v.end()) v.push_back(row[n - 1]);
    return v;
}

static uint64_t key(double mhz, int pi) { return ((uint64_t)std::llround(mhz * 10) << 16) | (uint64_t)(pi & 0xFFFF); }

std::string FmDxLog::homeCountry() const {
    if (!home_.empty()) return home_;
    std::map<std::string, uint32_t> n;
    for (const auto& kv : heard_) { const auto c = rdsCountry(kv.second.pi, kv.second.ecc); if (!c.empty()) n[c] += kv.second.count; }
    std::string best; uint32_t top = 0;
    for (const auto& kv : n) if (kv.second > top) { top = kv.second; best = kv.first; }
    return best;
}

FmDxAlert FmDxLog::observe(int64_t now, double mhz, int pi, int ecc, const std::string& ps, float aboveDb) {
    FmDxAlert a;
    if (pi == 0) return a;
    if (t0_ == 0) t0_ = now;
    auto it = heard_.find(key(mhz, pi));
    const bool isNew = it == heard_.end();
    FmDxHeard& h = heard_[key(mhz, pi)];
    if (isNew) { h.mhz = mhz; h.pi = pi; h.first = now; }
    h.last = now; h.count++;
    if (ecc >= 0) h.ecc = ecc;
    if (!ps.empty()) h.ps = ps;
    h.bestDb = std::max(h.bestDb, aboveDb);
    if (!isNew || !learned(now) || aboveDb < minAbove_) return a;
    const std::string home = homeCountry();
    const std::string country = rdsCountry(pi, h.ecc);
    std::string like;
    bool foreign;
    if (!country.empty()) foreign = !home.empty() && country != home;
    else {
        const auto v = rdsCountriesLikely(pi);
        for (const auto& c : v) like += (like.empty() ? "" : "/") + c;
        foreign = !v.empty() && !home.empty() && std::find(v.begin(), v.end(), home) == v.end();   // not even possibly the home country
    }
    if (!foreign) return a;
    a.fire = true; a.mhz = mhz; a.pi = pi; a.ps = ps; a.country = country; a.likely = like; a.certain = !country.empty(); a.levelDb = aboveDb; a.at = now;
    alerts_.push_back(a);
    return a;
}

std::vector<FmDxHeard> FmDxLog::heard() const {
    std::vector<FmDxHeard> v;
    for (const auto& kv : heard_) v.push_back(kv.second);
    return v;
}

std::string FmDxLog::toCsv() const {
    std::string s = "utc_seconds,mhz,pi,ps,country,proved_by_ecc,above_noise_db\n";
    char b[160];
    for (const auto& a : alerts_) {
        std::string ps = a.ps; for (auto& c : ps) if (c == ',' || c == '"') c = ' ';
        snprintf(b, sizeof b, "%lld,%.1f,%04X,%s,%s,%d,%.1f\n", (long long)a.at, a.mhz, a.pi, ps.c_str(), (a.certain ? a.country : a.likely).c_str(), a.certain ? 1 : 0, a.levelDb);
        s += b;
    }
    return s;
}

std::string FmDxLog::serialize() const {
    std::string s; char b[160];
    if (t0_) { snprintf(b, sizeof b, "t0\t%lld\n", (long long)t0_); s += b; }
    for (const auto& kv : heard_) {
        const auto& h = kv.second;
        std::string ps = h.ps; for (auto& c : ps) if (c == '\t' || c == '\n') c = ' ';
        snprintf(b, sizeof b, "s\t%.1f\t%04X\t%d\t%lld\t%lld\t%u\t%.1f\t%s\n", h.mhz, h.pi, h.ecc, (long long)h.first, (long long)h.last, h.count, h.bestDb, ps.c_str());
        s += b;
    }
    return s;
}

void FmDxLog::load(const std::string& text) {
    heard_.clear(); t0_ = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("t0\t", 0) == 0) { t0_ = std::atoll(line.c_str() + 3); continue; }
        if (line.rfind("s\t", 0) != 0) continue;
        FmDxHeard h; int pi = 0; long long a = 0, b = 0; unsigned c = 0; char ps[64] = "";
        const int n = sscanf(line.c_str() + 2, "%lf\t%x\t%d\t%lld\t%lld\t%u\t%f\t%63[^\n]", &h.mhz, &pi, &h.ecc, &a, &b, &c, &h.bestDb, ps);
        if (n < 7) continue;
        h.pi = pi; h.first = a; h.last = b; h.count = c; h.ps = ps;
        heard_[key(h.mhz, h.pi)] = h;
    }
}

} // namespace dect2
