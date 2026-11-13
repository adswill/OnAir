// 4.0 public safety, aviation, rail, LoRa and science helpers, group E (pure, no GUI). Tests: tests/d40/test_radio_e40.cpp
//  1. TgAlias / TgFollower   talkgroup alias import (RadioReference-style CSV or "id,name") and an auto-follow policy: priority groups pre-empt, equal ones wait for a hold time,
//                            encrypted calls are skipped.
//  2. pickControlChannel     from scan hits (valid frame rate, errors, level) the most likely trunking control channel, with "none" when nothing looks like one.
//  3. RangeMap               ADS-B / AIS reception range per 10 degree sector (max and 95th percentile): shows blind sectors and antenna gain pattern.
//  4. loraGuess, loraSyncWord  spreading factor from the measured symbol time and bandwidth; sync word byte from the two sync symbols, with its usual meaning.
//  5. PacketStats            counts per packet type with share and rate; Meshtastic port numbers get names.
//  6. PingLogger             meteor-scatter ping detector on a power envelope: start, duration, peak, decay time, under- or over-dense.
//  7. gsmrArfcnToHz          GSM-R (R-GSM) and E-GSM / P-GSM channel number to downlink / uplink frequency.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::d40 {

// 1.
struct TgInfo { int id = 0; std::string name; int priority = 0; };   // priority 0 = normal, larger wins
class TgAlias {
public:
    // lines "dec,hex,mode,alpha,desc,..." (a header line is skipped) or "dec,alpha". Quoted fields with commas are handled. Returns the number of groups read.
    int importCsv(const std::string& text) {
        std::istringstream in(text); std::string line; int n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto f = split(line); if (f.size() < 2) continue;
            char* end = nullptr; const long id = std::strtol(f[0].c_str(), &end, 10); if (end == f[0].c_str() || *end) continue;
            std::string name = f.size() >= 4 ? f[3] : f[1]; if (name.empty() && f.size() >= 5) name = f[4];
            m_[(int)id].id = (int)id; m_[(int)id].name = name; n++;
        }
        return n;
    }
    void setPriority(int id, int p) { m_[id].id = id; m_[id].priority = p; }
    std::string name(int id) const { auto i = m_.find(id); return i == m_.end() || i->second.name.empty() ? "TG " + std::to_string(id) : i->second.name; }
    int priority(int id) const { auto i = m_.find(id); return i == m_.end() ? 0 : i->second.priority; }
    size_t size() const { return m_.size(); }
private:
    static std::vector<std::string> split(const std::string& l) {
        std::vector<std::string> o; std::string c; bool q = false;
        for (char ch : l) { if (ch == '"') q = !q; else if (ch == ',' && !q) { o.push_back(c); c.clear(); } else c += ch; }
        o.push_back(c); return o;
    }
    std::map<int, TgInfo> m_;
};
class TgFollower {
public:
    TgFollower(const TgAlias& a, double holdSec = 3.0, bool listenSet = false) : a_(a), hold_(holdSec), onlyListed_(listenSet) {}
    void listen(int tg) { listen_.push_back(tg); }
    // a call starts or continues; returns true when the receiver should be on this talkgroup now
    bool onCall(int tg, double t, bool encrypted) {
        if (encrypted) return cur_ == tg && t - last_ <= hold_;
        if (onlyListed_ && std::find(listen_.begin(), listen_.end(), tg) == listen_.end()) return cur_ == tg && t - last_ <= hold_;
        const bool free = cur_ < 0 || t - last_ > hold_;
        if (tg == cur_) { last_ = t; return true; }
        if (free || a_.priority(tg) > a_.priority(cur_)) { cur_ = tg; last_ = t; return true; }
        return false;
    }
    int current() const { return cur_; }
private: const TgAlias& a_; double hold_; bool onlyListed_; std::vector<int> listen_; int cur_ = -1; double last_ = -1e9;
};

// 2.
struct CcHit { double freqHz = 0; int validFrames = 0, badFrames = 0; double levelDb = -120; double periodicity = 0; };   // periodicity 0..1: fixed frame cadence seen
inline int pickControlChannel(const std::vector<CcHit>& h, int minFrames = 8, double minValidShare = 0.6) {
    int best = -1; double bs = 0;
    for (size_t i = 0; i < h.size(); i++) {
        const int tot = h[i].validFrames + h[i].badFrames; if (h[i].validFrames < minFrames || tot == 0 || (double)h[i].validFrames / tot < minValidShare) continue;
        const double s = h[i].validFrames * (0.5 + 0.5 * h[i].periodicity) * ((double)h[i].validFrames / tot) + 0.02 * h[i].levelDb;
        if (s > bs) { bs = s; best = (int)i; }
    }
    return best;
}

// 3.
class RangeMap {
public:
    // own position and a contact, degrees; distance by haversine, bearing true
    void add(double lat0, double lon0, double lat, double lon) {
        const double r = M_PI / 180, p1 = lat0 * r, p2 = lat * r, dl = (lon - lon0) * r;
        const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
        const double km = 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
        double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r; if (b < 0) b += 360;
        v_[(int)(b / 10) % 36].push_back(km);
    }
    double maxKm(int sector) const { double m = 0; for (double d : v_[sector % 36]) m = std::max(m, d); return m; }
    double p95Km(int sector) const { auto s = v_[sector % 36]; if (s.empty()) return 0; std::sort(s.begin(), s.end()); return s[(size_t)(0.95 * (s.size() - 1))]; }
    int count(int sector) const { return (int)v_[sector % 36].size(); }
    // sectors whose p95 is under frac of the best sector's p95 (blocked by terrain or a poor antenna side)
    std::vector<int> blindSectors(double frac = 0.4, int minAll = 50) const {
        std::vector<int> o; int tot = 0; double best = 0; for (int i = 0; i < 36; i++) { tot += count(i); best = std::max(best, p95Km(i)); }
        if (tot < minAll) return o; for (int i = 0; i < 36; i++) if (p95Km(i) < frac * best) o.push_back(i); return o;
    }
private: std::vector<double> v_[36];
};

// 4. symbol time in seconds = 2^SF / BW. bw candidates are the standard ones; the nearest to the occupied bandwidth is taken.
struct LoraGuess { int sf = 0; double bwHz = 0; double confidence = 0; };
inline LoraGuess loraGuess(double symbolSec, double occupiedBwHz) {
    static const double bws[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
    LoraGuess g; double bd = 1e18;
    for (double b : bws) { const double d = std::fabs(std::log(b / occupiedBwHz)); if (d < bd) { bd = d; g.bwHz = b; } }
    const double x = std::log2(symbolSec * g.bwHz); g.sf = (int)std::lround(x);
    if (g.sf < 5 || g.sf > 12) { g.sf = 0; return g; }
    g.confidence = std::max(0.0, 1.0 - 4 * std::fabs(x - g.sf)) * std::max(0.0, 1.0 - bd);
    return g;
}
// the two sync symbols carry the high and low nibble of the sync word, each multiplied by 8; sf >= 7 needed for the *8 to fit
inline int loraSyncWord(int sym1, int sym2, int sf) {
    const int N = 1 << sf; auto nib = [&](int s) { const int n = (int)std::lround(s / 8.0); return n < 0 || n > 15 || std::abs(s - 8 * n) > 2 || s >= N ? -1 : n; };
    const int a = nib(sym1), b = nib(sym2); return a < 0 || b < 0 ? -1 : (a << 4) | b;
}
inline const char* loraSyncName(int w) { return w == 0x12 ? "private network (default)" : w == 0x34 ? "LoRaWAN public" : w == 0x2B ? "Meshtastic" : "unknown"; }

// 5.
inline const char* meshPortName(int p) {
    switch (p) { case 1: return "TEXT_MESSAGE"; case 3: return "POSITION"; case 4: return "NODEINFO"; case 5: return "ROUTING"; case 6: return "ADMIN"; case 67: return "TELEMETRY"; case 70: return "TRACEROUTE"; case 71: return "NEIGHBORINFO"; case 73: return "MAP_REPORT"; default: return nullptr; }
}
class PacketStats {
public:
    void add(const std::string& type, double t) { c_[type]++; total_++; if (first_ < 0) first_ = t; last_ = t; }
    void addMeshPort(int port, double t) { const char* n = meshPortName(port); add(n ? n : "PORT_" + std::to_string(port), t); }
    int count(const std::string& t) const { auto i = c_.find(t); return i == c_.end() ? 0 : i->second; }
    double share(const std::string& t) const { return total_ ? (double)count(t) / total_ : 0; }
    double perMinute(const std::string& t) const { return last_ > first_ ? count(t) * 60.0 / (last_ - first_) : 0; }
    std::vector<std::pair<std::string, int>> top(size_t n) const { std::vector<std::pair<std::string, int>> v(c_.begin(), c_.end()); std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); }); if (v.size() > n) v.resize(n); return v; }
private: std::map<std::string, int> c_; int total_ = 0; double first_ = -1, last_ = -1;
};

// 6. env: power in dB at fsHz. Floor = median; a ping starts above floor + onDb and ends when back under floor + offDb (hysteresis); shorter than minSec is ignored.
struct Ping { double startSec = 0, durSec = 0, peakDb = 0, decayTauSec = 0; bool underdense = false; };
inline std::vector<Ping> findPings(const std::vector<float>& env, double fsHz, double onDb = 8, double offDb = 3, double minSec = 0.05) {
    std::vector<Ping> out; const size_t n = env.size(); if (n < 16) return out;
    std::vector<float> s = env; std::nth_element(s.begin(), s.begin() + n / 2, s.end()); const double fl = s[n / 2];
    size_t i = 0;
    while (i < n) {
        if (env[i] < fl + onDb) { i++; continue; }
        size_t j = i; size_t pk = i; while (j < n && env[j] > fl + offDb) { if (env[j] > env[pk]) pk = j; j++; }
        const double dur = (double)(j - i) / fsHz;
        if (dur >= minSec) {
            Ping p; p.startSec = (double)i / fsHz; p.durSec = dur; p.peakDb = env[pk] - fl;
            // decay: least squares of dB against time from the peak to the end; underdense = steady exponential (dB falls linearly) after a short rise
            const size_t m = j - pk;
            if (m >= 4) {
                double st = 0, sy = 0, stt = 0, sty = 0, syy = 0; for (size_t k = 0; k < m; k++) { const double t = (double)k / fsHz, y = env[pk + k]; st += t; sy += y; stt += t * t; sty += t * y; syy += y * y; }
                const double den = m * stt - st * st, slope = (m * sty - st * sy) / den;
                const double vy = m * syy - sy * sy, r2 = vy > 0 ? (m * sty - st * sy) * (m * sty - st * sy) / (den * vy) : 0;
                if (slope < 0) p.decayTauSec = 10.0 / std::log(10.0) / -slope * 1.0;   // power in dB: tau = 10 log10(e) / |slope| seconds for e-fold in power
                size_t plateauN = 0; for (size_t k = i; k < j; k++) plateauN += env[k] > env[pk] - 2.0f;   // time within 2 dB of the peak: a plateau means overdense
                p.underdense = slope < 0 && r2 > 0.9 && (double)(pk - i) / fsHz < 0.3 * dur && (double)plateauN / (double)(j - i) < 0.2;
            }
            out.push_back(p);
        }
        i = j + 1;
    }
    return out;
}

// 7. returns false for a number outside P-GSM (1-124), E-GSM (975-1023, 0) and R-GSM (955-974)
inline bool gsmrArfcnToHz(int n, double* downHz, double* upHz = nullptr) {
    double up;
    if (n >= 1 && n <= 124) up = 890.0 + 0.2 * n;
    else if (n == 0) up = 890.0;
    else if (n >= 955 && n <= 1023) up = 890.0 + 0.2 * (n - 1024);
    else return false;
    if (downHz) *downHz = (up + 45.0) * 1e6; if (upHz) *upHz = up * 1e6; return true;
}
inline bool isGsmrBand(int n) { return n >= 955 && n <= 973; }   // 876-880 MHz up, 921-925 MHz down

}  // namespace dect2::d40
