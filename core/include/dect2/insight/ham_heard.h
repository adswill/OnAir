// Ham radio insights from your own decodes (docs/SMART5-ham-3.9.md):
//  WhoHearsMe     - stations that call or answer you, the report they gave you, distance, bearing, link balance
//  AntennaPattern - SNR against distance fitted over all located decodes, the rest averaged per 30-degree sector
//  BustedCalls    - a call heard once that is one character / one Morse element from a call heard often, or is not a valid call
// Offline: only what this receiver decoded. Inputs are FT8 / FT4 / JS8 style messages ("LY1ABC DL1XYZ -12").
#pragma once
#include "dect2/insight/insight.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::insight {

// the words of a message; <LY1ABC/P> hashed calls lose their brackets
inline std::vector<std::string> words(const std::string& msg) {
    std::vector<std::string> w;
    std::istringstream is(msg);
    std::string t;
    while (is >> t) {
        if (t.size() > 2 && t.front() == '<' && t.back() == '>') t = t.substr(1, t.size() - 2);
        w.push_back(upper(t));
    }
    return w;
}

// "-12", "+05", "R-12", "R+05": the report in dB; false for anything else
inline bool parseReport(const std::string& s, int& db, bool& roger) {
    std::string t = s;
    roger = false;
    if (t.size() >= 2 && t[0] == 'R' && (t[1] == '-' || t[1] == '+')) { roger = true; t = t.substr(1); }
    if (t.size() < 2 || t.size() > 3 || (t[0] != '-' && t[0] != '+')) return false;
    for (size_t i = 1; i < t.size(); i++) if (!std::isdigit((unsigned char)t[i])) return false;
    db = std::atoi(t.c_str());
    return db >= -50 && db <= 49;
}

// The structure of an amateur call sign: a prefix with a digit, a suffix of 1 .. 4 letters; "/P", "/M", "DL/" parts allowed
inline bool validCall(const std::string& in) {
    std::string c = upper(in);
    // keep the longest part of a call with slashes (the home call)
    if (c.find('/') != std::string::npos) {
        std::string best, part;
        std::istringstream is(c);
        while (std::getline(is, part, '/')) if (part.size() > best.size()) best = part;
        c = best;
    }
    if (c.size() < 3 || c.size() > 7) return false;
    for (char ch : c) if (!std::isalnum((unsigned char)ch)) return false;
    // the last digit splits prefix and suffix
    size_t d = std::string::npos;
    for (size_t i = 0; i < c.size(); i++) if (std::isdigit((unsigned char)c[i])) d = i;
    if (d == std::string::npos || d == 0 || d > 3 || d + 1 >= c.size()) return false;
    for (size_t i = d + 1; i < c.size(); i++) if (!std::isalpha((unsigned char)c[i])) return false;
    if (c.size() - d - 1 > 4) return false;
    bool letter = false;
    for (size_t i = 0; i < d; i++) if (std::isalpha((unsigned char)c[i])) letter = true;
    return letter;
}

inline bool isGridWord(const std::string& w) { double a, b; return w.size() == 4 && gridToLatLon(w, a, b); }

// ------------------------------------------------------------------------------------------------------------------
struct HeardMe {
    std::string call, grid, band;
    int theirReport = 0;          // their SNR of you, dB (the report in their message)
    float mySnr = 0;              // your SNR of them
    double distanceKm = -1, bearing = 0;
    double lastTime = 0;
    int times = 0;
    bool hasReport = false;
    int balance() const { return theirReport - (int)std::lround(mySnr); }   // > 0: they hear you better than you hear them
};

class WhoHearsMe {
public:
    void setStation(const std::string& myCall, const std::string& myGrid) {
        call_ = upper(myCall);
        haveHome_ = gridToLatLon(myGrid, homeLat_, homeLon_);
    }
    // every decode of the band (also the ones not for you: they teach the locators). Returns true when it was a station hearing you.
    bool add(double time, const std::string& band, const std::string& msg, float snr) {
        const auto w = words(msg);
        if (w.size() >= 2) {
            // a locator in any message: "CQ DL1XYZ JO62", "LY1ABC DL1XYZ JO62", "CQ DX DL1XYZ JO62"
            const std::string& last = w.back();
            if (w.size() >= 3 && isGridWord(last)) grids_[w[w.size() - 2]] = last;
        }
        if (call_.empty() || w.size() < 3 || w[0] != call_) return false;
        const std::string& from = w[1];
        if (!validCall(from)) return false;
        HeardMe& h = heard_[from + "|" + band];
        h.call = from; h.band = band; h.lastTime = time; h.times++;
        h.mySnr = snr;
        int db; bool r;
        if (parseReport(w[2], db, r)) { h.theirReport = db; h.hasReport = true; }
        auto g = grids_.find(from);
        if (g != grids_.end()) h.grid = g->second;
        if (!h.grid.empty() && haveHome_) {
            double la, lo;
            if (gridToLatLon(h.grid, la, lo)) { h.distanceKm = distanceKm(homeLat_, homeLon_, la, lo); h.bearing = bearingDeg(homeLat_, homeLon_, la, lo); }
        }
        return true;
    }
    using Row = HeardMe;
    // newest first
    std::vector<Row> list() const {
        std::vector<Row> v;
        for (const auto& kv : heard_) v.push_back(kv.second);
        std::sort(v.begin(), v.end(), [](const Row& a, const Row& b) { return a.lastTime > b.lastTime; });
        return v;
    }
    // the farthest station that heard you, per band
    std::map<std::string, Row> bestPerBand() const {
        std::map<std::string, Row> m;
        for (const auto& r : list()) if (r.distanceKm >= 0 && (!m.count(r.band) || r.distanceKm > m[r.band].distanceKm)) m[r.band] = r;
        return m;
    }
    const std::map<std::string, std::string>& grids() const { return grids_; }
    bool home(double& lat, double& lon) const { lat = homeLat_; lon = homeLon_; return haveHome_; }
    void clear() { heard_.clear(); }

private:
    std::string call_;
    bool haveHome_ = false;
    double homeLat_ = 0, homeLon_ = 0;
    std::map<std::string, std::string> grids_;   // call -> locator, from any decode
    std::map<std::string, HeardMe> heard_;
};

// ------------------------------------------------------------------------------------------------------------------
struct PatternSector { int fromDeg = 0; int count = 0; double meanDb = 0; double stdErrDb = 0; };

class AntennaPattern {
public:
    static constexpr int kSectors = 12;
    void add(double bearing, double km, float snr) {
        if (km < 50 || km > 20000 || !std::isfinite(snr)) return;   // near stations are ground wave
        pts_.push_back({bearing, std::log10(km), snr});
        if (pts_.size() > 20000) pts_.erase(pts_.begin(), pts_.begin() + 5000);
    }
    size_t size() const { return pts_.size(); }
    // fit snr = a + b * log10(km); the rest per sector. Sectors with fewer than minCount points have count only.
    std::vector<PatternSector> sectors(int minCount = 5) const {
        std::vector<PatternSector> out(kSectors);
        for (int i = 0; i < kSectors; i++) out[i].fromDeg = i * 30;
        if (pts_.size() < 10) return out;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const auto& p : pts_) { sx += p.lk; sy += p.snr; sxx += p.lk * p.lk; sxy += p.lk * p.snr; }
        const double n = (double)pts_.size(), den = n * sxx - sx * sx;
        const double b = std::fabs(den) < 1e-9 ? 0 : (n * sxy - sx * sy) / den, a = (sy - b * sx) / n;
        // with the distance taken out, a sector's mean shows the direction alone; the overall mean of the residuals is 0
        std::vector<double> s1(kSectors, 0), s2(kSectors, 0);
        for (const auto& p : pts_) {
            const int k = (int)std::floor(std::fmod(p.brg + 360.0, 360.0) / 30.0) % kSectors;
            const double r = p.snr - (a + b * p.lk);
            out[k].count++; s1[k] += r; s2[k] += r * r;
        }
        for (int k = 0; k < kSectors; k++) {
            if (out[k].count < minCount) continue;
            const double m = s1[k] / out[k].count, var = std::max(0.0, s2[k] / out[k].count - m * m);
            out[k].meanDb = m; out[k].stdErrDb = std::sqrt(var / out[k].count);
        }
        return out;
    }
    // the sector the antenna favours most (-1 when not enough points)
    int bestSector(int minCount = 5) const {
        auto s = sectors(minCount);
        int best = -1;
        for (int k = 0; k < kSectors; k++) if (s[k].count >= minCount && (best < 0 || s[k].meanDb > s[best].meanDb)) best = k;
        return best;
    }
    void clear() { pts_.clear(); }

private:
    struct P { double brg, lk; float snr; };
    std::vector<P> pts_;
};

// ------------------------------------------------------------------------------------------------------------------
// Morse code of A-Z 0-9, for "one element away" (a dot read as a dash, a dot lost or added)
inline const char* morseOf(char c) {
    static const char* const L[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                                      "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
    static const char* const D[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    c = (char)std::toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return L[c - 'A'];
    if (c >= '0' && c <= '9') return D[c - '0'];
    return "";
}

inline int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// two calls whose Morse differs by one element (dot / dash swapped, lost or added), letter gaps kept
inline bool oneMorseElement(const std::string& a, const std::string& b) {
    auto code = [](const std::string& s) { std::string m; for (char c : s) { m += morseOf(c); m += ' '; } return m; };
    return editDistance(code(a), code(b)) == 1;
}

struct BustVerdict {
    bool busted = false;
    std::string likely;      // the call it probably is
    std::string why;         // "one character from LY1ABC (heard 12 times)", "not a valid call sign"
};

class BustedCalls {
public:
    void add(const std::string& call) { if (!call.empty()) counts_[upper(call)]++; }
    int count(const std::string& call) const { auto it = counts_.find(upper(call)); return it == counts_.end() ? 0 : it->second; }
    // cw: also look for one Morse element (a skimmer of CW); otherwise one character (substitution, lost or extra)
    BustVerdict check(const std::string& in, bool cw = false, int often = 3) const {
        BustVerdict v;
        const std::string c = upper(in);
        if (!validCall(c)) { v.busted = true; v.why = "not a valid call sign"; }
        if (count(c) > 1) return v;   // heard more than once: trusted (an invalid one stays flagged)
        int best = 0;
        for (const auto& kv : counts_) {
            if (kv.second < often || kv.first == c) continue;
            const bool isNear = editDistance(kv.first, c) == 1 || (cw && oneMorseElement(kv.first, c));
            if (isNear && kv.second > best) { best = kv.second; v.likely = kv.first; }
        }
        if (!v.likely.empty()) {
            v.busted = true;
            v.why = (cw && editDistance(v.likely, c) != 1 ? "one Morse element from " : "one character from ") + v.likely + " (heard " + std::to_string(best) + " times)";
        }
        return v;
    }
    void clear() { counts_.clear(); }

private:
    std::map<std::string, int> counts_;
};

}  // namespace dect2::insight
