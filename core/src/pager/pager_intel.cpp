// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers: text repair, positions, learned fields and incidents (see pager_intel.h).
#include "dect2/pager/pager_intel.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace pager {

// ================================================================ positions

namespace {
constexpr double kPi = 3.14159265358979323846;

double deg2rad(double d) { return d * kPi / 180.0; }

bool latLonOk(double lat, double lon) { return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && !(lat == 0 && lon == 0); }

std::string lowerOf(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
bool isAlnum(int c) { return std::isalnum(c & 0xFF) != 0 && c < 128; }
} // namespace

bool utmToLatLon(int zone, bool south, double easting, double northing, double& lat, double& lon) {
    if (zone < 1 || zone > 60 || easting < 100000 || easting > 900000 || northing < 0 || northing > 10000000) return false;
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f), k0 = 0.9996;
    const double x = easting - 500000.0, y = south ? northing - 10000000.0 : northing;
    const double m = y / k0;
    const double e4 = e2 * e2, e6 = e4 * e2;
    const double mu = m / (a * (1 - e2 / 4 - 3 * e4 / 64 - 5 * e6 / 256));
    const double s = std::sqrt(1 - e2), e1 = (1 - s) / (1 + s);
    const double phi1 = mu + (3 * e1 / 2 - 27 * e1 * e1 * e1 / 32) * std::sin(2 * mu) + (21 * e1 * e1 / 16 - 55 * e1 * e1 * e1 * e1 / 32) * std::sin(4 * mu) +
                        (151 * e1 * e1 * e1 / 96) * std::sin(6 * mu) + (1097 * e1 * e1 * e1 * e1 / 512) * std::sin(8 * mu);
    const double ep2 = e2 / (1 - e2), sp = std::sin(phi1), cp = std::cos(phi1), tp = std::tan(phi1);
    const double c1 = ep2 * cp * cp, t1 = tp * tp;
    const double n1 = a / std::sqrt(1 - e2 * sp * sp), r1 = a * (1 - e2) / std::pow(1 - e2 * sp * sp, 1.5);
    const double d = x / (n1 * k0);
    const double d2 = d * d, d3 = d2 * d, d4 = d2 * d2, d5 = d4 * d, d6 = d4 * d2;
    const double la = phi1 - (n1 * tp / r1) * (d2 / 2 - (5 + 3 * t1 + 10 * c1 - 4 * c1 * c1 - 9 * ep2) * d4 / 24 + (61 + 90 * t1 + 298 * c1 + 45 * t1 * t1 - 252 * ep2 - 3 * c1 * c1) * d6 / 720);
    const double lo = (d - (1 + 2 * t1 + c1) * d3 / 6 + (5 - 2 * c1 + 28 * t1 - 3 * c1 * c1 + 8 * ep2 + 24 * t1 * t1) * d5 / 120) / cp;
    lat = la * 180.0 / kPi;
    lon = (zone - 1) * 6 - 180 + 3 + lo * 180.0 / kPi;
    return lat >= -84.5 && lat <= 84.5;
}

bool mgrsToLatLon(int zone, char band, char colLetter, char rowLetter, double e, double n, double& lat, double& lon) {
    static const char* const cols[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
    static const char kRows[] = "ABCDEFGHJKLMNPQRSTUV";
    static const char kBands[] = "CDEFGHJKLMNPQRSTUVWX";
    if (zone < 1 || zone > 60) return false;
    const char* cb = std::strchr(kBands, band);
    const char* cc = std::strchr(cols[(zone - 1) % 3], colLetter);
    const char* cr = std::strchr(kRows, rowLetter);
    if (!band || !cb || !colLetter || !cc || !rowLetter || !cr) return false;
    const int bandIdx = (int)(cb - kBands);
    int row = (int)(cr - kRows);
    if (zone % 2 == 0) row = (row + 15) % 20;           // even zones start the rows at F
    const double east = ((int)(cc - cols[(zone - 1) % 3]) + 1) * 100000.0 + e;
    const double latMin = -80.0 + 8.0 * bandIdx, latMax = bandIdx == 19 ? 84.0 : latMin + 8.0;
    const bool south = bandIdx < 10;
    for (int k = 0; k < 5; k++) {
        const double north = row * 100000.0 + k * 2000000.0 + n;
        double la, lo;
        if (!utmToLatLon(zone, south, east, north, la, lo)) continue;
        if (la >= latMin - 0.01 && la < latMax + 0.01) { lat = la; lon = lo; return true; }
    }
    return false;
}

double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = deg2rad(lat1), p2 = deg2rad(lat2), dp = p2 - p1, dl = deg2rad(lon2 - lon1);
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 6371.0 * 2 * std::asin(std::min(1.0, std::sqrt(h)));
}

std::vector<GeoHit> findCoordinates(const std::string& text) {
    static const std::regex reMgrs(R"(\b(\d{1,2})([C-HJ-NP-X])\s?([A-HJ-NP-Z])([A-HJ-NP-V])\s?(\d{2,10})(?:\s+(\d{2,5}))?\b)");
    static const std::regex reUtm(R"(\b(\d{1,2})\s?([C-HJ-NP-X])\s+(\d{6})\s*m?E?\s+(\d{6,7})\b)");
    static const std::regex reDm(R"(([NS])\s?(\d{1,2})[°\s]\s?(\d{1,2}(?:\.\d+)?)'?\s*[,;]?\s*([EW])\s?(\d{1,3})[°\s]\s?(\d{1,2}(?:\.\d+)?)'?)");
    static const std::regex reDec(R"(([-+]?\d{1,2}\.\d{3,})\s?([NS])?\s*[,;/ ]\s*([-+]?\d{1,3}\.\d{3,})\s?([EW])?)");
    std::vector<GeoHit> hits;
    std::vector<std::pair<size_t, size_t>> used;
    auto free = [&](size_t a, size_t b) {
        for (auto& u : used) if (a < u.second && u.first < b) return false;
        return true;
    };
    auto add = [&](size_t a, size_t b, double lat, double lon, const char* kind) {
        if (!latLonOk(lat, lon) || !free(a, b)) return;
        used.push_back({a, b});
        GeoHit h; h.lat = lat; h.lon = lon; h.kind = kind; h.pos = a;
        hits.push_back(h);
    };
    for (std::sregex_iterator it(text.begin(), text.end(), reMgrs), end; it != end; ++it) {
        const std::smatch& m = *it;
        std::string d1 = m[5].str(), d2 = m[6].str();
        if (d2.empty()) { if (d1.size() % 2) continue; d2 = d1.substr(d1.size() / 2); d1 = d1.substr(0, d1.size() / 2); }
        if (d1.size() != d2.size()) continue;
        const double scale = std::pow(10.0, 5 - (int)d1.size());
        double lat, lon;
        if (mgrsToLatLon(std::atoi(m[1].str().c_str()), m[2].str()[0], m[3].str()[0], m[4].str()[0], std::atof(d1.c_str()) * scale, std::atof(d2.c_str()) * scale, lat, lon))
            add((size_t)m.position(0), (size_t)(m.position(0) + m.length(0)), lat, lon, "MGRS");
    }
    for (std::sregex_iterator it(text.begin(), text.end(), reUtm), end; it != end; ++it) {
        const std::smatch& m = *it;
        const char b = m[2].str()[0];
        double lat, lon;
        if (utmToLatLon(std::atoi(m[1].str().c_str()), b < 'N', std::atof(m[3].str().c_str()), std::atof(m[4].str().c_str()), lat, lon))
            add((size_t)m.position(0), (size_t)(m.position(0) + m.length(0)), lat, lon, "UTM");
    }
    for (std::sregex_iterator it(text.begin(), text.end(), reDm), end; it != end; ++it) {
        const std::smatch& m = *it;
        double la = std::atof(m[2].str().c_str()) + std::atof(m[3].str().c_str()) / 60.0;
        double lo = std::atof(m[5].str().c_str()) + std::atof(m[6].str().c_str()) / 60.0;
        if (m[1].str() == "S") la = -la;
        if (m[4].str() == "W") lo = -lo;
        add((size_t)m.position(0), (size_t)(m.position(0) + m.length(0)), la, lo, "coordinates");
    }
    for (std::sregex_iterator it(text.begin(), text.end(), reDec), end; it != end; ++it) {
        const std::smatch& m = *it;
        double la = std::atof(m[1].str().c_str()), lo = std::atof(m[3].str().c_str());
        if (m[2].str() == "S") la = -std::fabs(la);
        if (m[4].str() == "W") lo = -std::fabs(lo);
        add((size_t)m.position(0), (size_t)(m.position(0) + m.length(0)), la, lo, "coordinates");
    }
    std::sort(hits.begin(), hits.end(), [](const GeoHit& a, const GeoHit& b) { return a.pos < b.pos; });
    return hits;
}

// ================================================================ the brain

namespace {

constexpr size_t kMaxProfiles = 400, kMaxWordsProfile = 3000, kMaxWordsGlobal = 20000, kMaxRecent = 240, kMaxIncidents = 300;

uint64_t profKey(const PagerMessage& m) { return ((uint64_t)(m.flex ? 1 : 0) << 40) | m.address; }

// the alphanumeric words of a text: runs of letters and digits, at least two characters, with at least one letter
std::vector<std::string> wordsOf(const std::string& t) {
    std::vector<std::string> w;
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 2) {
            bool letter = false;
            for (char c : cur) letter = letter || std::isalpha((unsigned char)c);
            if (letter) w.push_back(cur);
        }
        cur.clear();
    };
    for (char c : t) {
        if (isAlnum((unsigned char)c)) cur += c; else flush();
    }
    flush();
    return w;
}

void pruneWords(std::unordered_map<std::string, uint32_t>& m, size_t limit) {
    if (m.size() <= limit) return;
    std::vector<uint32_t> counts;
    counts.reserve(m.size());
    for (auto& kv : m) counts.push_back(kv.second);
    std::nth_element(counts.begin(), counts.begin() + (ptrdiff_t)counts.size() / 2, counts.end());
    const uint32_t cut = counts[counts.size() / 2];
    for (auto it = m.begin(); it != m.end();) it = it->second <= cut ? m.erase(it) : std::next(it);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool looksAddress(const std::string& t) {
    static const std::regex re(R"(\b\d{1,5}[A-Za-z]?\s+(?:[A-Za-z']+\s+){0,3}(?:st|street|rd|road|ave|avenue|dr|drive|ln|lane|ct|court|pl|place|hwy|highway|blvd|way|cres|crescent|tce|terrace|cl|close|gatan|straat|laan)\b|\b[A-Za-z]+(?:stra(?:ss|ß)e|str\.|gasse|weg|platz|gatan|straat|laan)\s*\d+)", std::regex::icase);
    return std::regex_search(t, re);
}

double digitShare(const std::string& s) {
    if (s.empty()) return 0;
    size_t d = 0;
    for (char c : s) d += std::isdigit((unsigned char)c) ? 1 : 0;
    return (double)d / (double)s.size();
}

std::vector<std::string> jobKeys(const std::string& t) {
    static const std::regex re1(R"((?:job|inc(?:ident)?|ref|nr|no|id)\s*[:#.]?\s*([A-Z0-9-]{4,}))", std::regex::icase);
    static const std::regex re2(R"(\b([A-Z]{1,4}-?\d{6,})\b)");
    static const std::regex re3(R"(#(\d{4,}))");
    std::vector<std::string> k;
    for (const std::regex* re : {&re1, &re2, &re3})
        for (std::sregex_iterator it(t.begin(), t.end(), *re), end; it != end; ++it) {
            std::string v = lowerOf((*it)[1].str());
            bool digit = false;
            for (char c : v) digit = digit || std::isdigit((unsigned char)c);
            if (digit && std::find(k.begin(), k.end(), v) == k.end()) k.push_back(v);
        }
    return k;
}

} // namespace

bool PagerBrain::looksMedical(const std::string& text) {
    static const char* const kw[] = {"ambulance", "cardiac", "chest pain", "unconscious", "overdose", "resuscitat", "cpr", "stroke", "seizure", "patient", "bleeding",
                                     "fracture", "diabet", "medical", "paramedic", "breathing", "suicid", "maternity", "unwell", "collapsed", "injur", "casualty", "p1 ", "als "};
    const std::string l = lowerOf(text);
    for (const char* k : kw) {
        const char* f = std::strstr(l.c_str(), k);
        if (!f) continue;
        // a whole word (or the start of one) so that "scprs" does not match "cpr"
        const size_t at = (size_t)(f - l.c_str());
        if (at == 0 || !std::isalnum((unsigned char)l[at - 1])) return true;
    }
    return false;
}

int PagerBrain::loadPlaces(const std::string& text) {
    int n = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = trim(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == '#') continue;
        const char sep = line.find('\t') != std::string::npos ? '\t' : ',';
        const size_t c2 = line.rfind(sep), c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : line.rfind(sep, c2 - 1);
        if (c1 == std::string::npos) continue;
        Place p;
        p.name = trim(line.substr(0, c1));
        p.lat = std::atof(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        p.lon = std::atof(line.substr(c2 + 1).c_str());
        p.lower = lowerOf(p.name);
        if (p.lower.size() < 4 || !latLonOk(p.lat, p.lon)) continue;
        places_.push_back(std::move(p));
        n++;
    }
    return n;
}
void PagerBrain::clearPlaces() { places_.clear(); }

void PagerBrain::reset() {
    prof_.clear(); global_.clear(); recent_.clear(); inc_.clear();
    nextIncident_ = 1;
    stats = BrainStats();
}

// ---------------------------------------------------------------- learning

void PagerBrain::learn(const std::string& text, Profile& pr) {
    for (const std::string& w : wordsOf(text)) {
        // a number-like word (all digits apart from one letter) is not worth keeping: it cannot be guessed
        if (digitShare(w) > 0.5) continue;
        pr.words[w]++;
        global_[w]++;
    }
    {
        int run = 0;
        for (size_t i = 0; i <= text.size(); i++) {
            if (i < text.size() && std::isdigit((unsigned char)text[i])) run++;
            else { if (run > 0 && run < 12) { pr.nums[run]++; globalNum_[run]++; } run = 0; }
        }
    }
    pruneWords(pr.words, kMaxWordsProfile);
    pruneWords(global_, kMaxWordsGlobal);
    pr.n++;
}

// ---------------------------------------------------------------- repair (#13)

namespace {

struct Cell { double sc = -1e30; int prev = -1, prevK = 0; int len = 0; const std::string* w = nullptr; };

} // namespace

void PagerBrain::repairText(PagerMessage& m, Profile& pr) {
    const std::string& raw = m.alphaRaw;
    const std::string& kn = m.alphaKnown;
    size_t n = std::min(raw.size(), kn.size());
    for (size_t i = 0; i < n; i++)
        if ((kn[i] & 0x7F) == 0x7F && (raw[i] & 0x7F) == 4) { n = i; break; }        // end of text
    while (n > 0 && (kn[n - 1] & 0x7F) == 0x7F && (raw[n - 1] & 0x7F) == 0) n--;      // padding
    // a lost word at the end: how long the text really was is unknown, so the lost tail is cut and one '?' stands for it
    bool tail = m.alphaTailLost > 0;
    while (n > 0 && (kn[n - 1] & 0x7F) != 0x7F) { n--; tail = true; }
    bool any = tail;
    for (size_t i = 0; i < n; i++) any = any || (kn[i] & 0x7F) != 0x7F;
    if (!any) return;
    auto known = [&](size_t i) { return (kn[i] & 0x7F) == 0x7F; };
    auto ch = [&](size_t i) { return (int)(raw[i] & 0x7F); };
    auto fits = [&](size_t i, int c) { return (((c ^ raw[i]) & kn[i]) & 0x7F) == 0; };

    // the vocabulary: this capcode's words first, then the channel's. Only words seen at least twice (counting the capcode's three times)
    struct V { const std::string* w; uint32_t cap, glob; double sc; };
    std::vector<V> pool;
    for (auto& kv : pr.words) {
        const auto g = global_.find(kv.first);
        const uint32_t glob = g == global_.end() ? 0 : g->second;
        if (3 * kv.second + glob >= 2) pool.push_back({&kv.first, kv.second, glob, 3.0 * kv.second + glob});
    }
    for (auto& kv : global_)
        if (kv.second >= 2 && pr.words.find(kv.first) == pr.words.end()) pool.push_back({&kv.first, 0, kv.second, (double)kv.second});
    // the word probabilities (a rough unigram model): a path with more words pays for each, so a long rare word beats two short common ones
    // only when its letters fit; a number is a competitor of any word that fits digits, at the rate this capcode sends numbers of that length
    double total = 1;
    for (const V& v : pool) total += v.sc;
    for (auto& kv : pr.nums) total += 3.0 * kv.second;
    for (auto& kv : globalNum_) total += kv.second;
    for (V& v : pool) v.sc = std::log((v.sc + 0.5) / total);
    static const std::string kNumTag = "#";
    std::vector<std::vector<const V*>> byLen(48);
    for (const V& v : pool) if (v.w->size() < byLen.size()) byLen[v.w->size()].push_back(&v);

    std::string out, mask;
    size_t guessed = 0, lost = 0;
    auto emitKnown = [&](size_t i) {
        const int c = ch(i);
        if (c == 0) return;
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)(c < 32 || c == 127 ? '?' : c);
        mask += (char)0;
    };
    size_t i = 0;
    while (i < n) {
        if (known(i) && !isAlnum(ch(i))) { emitKnown(i); i++; continue; }
        size_t j = i;
        bool unk = false;
        while (j < n && (!known(j) || isAlnum(ch(j)))) { unk = unk || !known(j); j++; }
        if (!unk) { for (size_t k = i; k < j; k++) emitKnown(k); i = j; continue; }
        // a stretch with lost characters: split it into words from the vocabulary (a lost character may also have been a space)
        const size_t L = j - i;
        std::vector<std::array<Cell, 2>> dp(L + 2);
        std::vector<int> cnt(L + 2, 0);
        dp[0][0].sc = 0; cnt[0] = 1;
        auto spaceOk = [&](size_t k) { return !known(i + k) && fits(i + k, ' '); };
        for (size_t s = 0; s < L; s++) {
            if (!cnt[s]) continue;
            for (size_t e = s + 1; e <= L; e++) {
                if (e < L && !(e + 1 < L && spaceOk(e))) continue;
                const size_t wl = e - s;
                bool allKnown = true;
                for (size_t k = s; k < e; k++) allKnown = allKnown && known(i + k);
                std::vector<std::pair<const std::string*, double>> opts;
                if (wl < byLen.size())
                    for (const V* v : byLen[wl]) {
                        bool ok = true;
                        for (size_t k = 0; k < wl && ok; k++) {
                            const int c = (unsigned char)(*v->w)[k];
                            ok = known(i + s + k) ? c == ch(i + s + k) : fits(i + s + k, c);
                        }
                        if (ok) opts.push_back({v->w, v->sc});
                    }
                if (allKnown && opts.empty()) opts.push_back({nullptr, std::log(0.5 / total) - 1.0});
                {
                    bool digitsOk = true;
                    for (size_t k = s; k < e && digitsOk; k++) {
                        if (known(i + k)) digitsOk = std::isdigit(ch(i + k)) != 0;
                        else { bool f = false; for (int c = '0'; c <= '9'; c++) f = f || fits(i + k, c); digitsOk = f; }
                    }
                    if (digitsOk && !allKnown) {
                        const auto a = pr.nums.find((int)wl); const auto g = globalNum_.find((int)wl);
                        const double ev = 3.0 * (a == pr.nums.end() ? 0 : a->second) + (g == globalNum_.end() ? 0 : g->second);
                        opts.push_back({&kNumTag, std::log((ev + 0.5) / total)});
                    }
                }
                const size_t to = e == L ? L + 1 : e + 1;
                for (auto& o : opts)
                    for (int k = 0; k < cnt[s]; k++) {
                        Cell c; c.sc = dp[s][(size_t)k].sc + o.second; c.prev = (int)s; c.prevK = k; c.len = (int)wl; c.w = o.first;
                        // keep the two best
                        if (cnt[to] < 2) { dp[to][(size_t)cnt[to]++] = c; if (cnt[to] == 2 && dp[to][1].sc > dp[to][0].sc) std::swap(dp[to][0], dp[to][1]); }
                        else if (c.sc > dp[to][1].sc) { dp[to][1] = c; if (dp[to][1].sc > dp[to][0].sc) std::swap(dp[to][0], dp[to][1]); }
                    }
            }
        }
        const bool solved = cnt[L + 1] >= 1 && (cnt[L + 1] == 1 || dp[L + 1][0].sc - dp[L + 1][1].sc >= 1.1);
        std::string seg(L, '?'), sm(L, (char)2);
        if (solved) {
            // walk back
            int pos = (int)L + 1, k = 0;
            while (pos != 0) {
                const Cell& c = dp[(size_t)pos][(size_t)k];
                const int s = c.prev, e = s + c.len;
                for (int t = 0; t < c.len; t++) {
                    const size_t q = (size_t)(s + t);
                    if (known(i + q)) { seg[q] = (char)ch(i + q); sm[q] = 0; }
                    else if (c.w == &kNumTag) {
                        int only = -1, cands = 0;
                        for (int d = '0'; d <= '9'; d++) if (fits(i + q, d)) { cands++; only = d; }
                        seg[q] = cands == 1 ? (char)only : '?'; sm[q] = cands == 1 ? (char)1 : (char)2;
                    }
                    else { seg[q] = c.w ? (*c.w)[(size_t)t] : '?'; sm[q] = c.w ? (char)1 : (char)2; }
                }
                if (e < (int)L) { seg[(size_t)e] = ' '; sm[(size_t)e] = 1; }
                pos = s; k = c.prevK;
            }
        } else {
            for (size_t q = 0; q < L; q++) {
                if (known(i + q)) { seg[q] = (char)ch(i + q); sm[q] = 0; continue; }
                int only = -1, cands = 0;
                for (int c = 32; c < 127; c++) if (fits(i + q, c)) { cands++; only = c; }
                if (cands == 1) { seg[q] = (char)only; sm[q] = 1; }
            }
        }
        for (size_t q = 0; q < L; q++) {
            out += seg[q]; mask += sm[q];
            if (sm[q] == 1) guessed++; else if (sm[q] == 2) lost++;
        }
        i = j;
    }
    if (tail) { out += '?'; mask += (char)2; lost++; }
    if (guessed + lost == 0) return;
    m.repairedText = out;
    m.guessMask = mask;
    if (guessed) stats.repairedPages++;
    stats.guessedChars += guessed;
    stats.lostChars += lost;
}

// ---------------------------------------------------------------- fields, positions (#12)

void PagerBrain::segments(PagerMessage& m, Profile& pr, const std::string& text) {
    static const char D[4] = {'|', ';', ',', '/'};
    int cnt[4] = {0, 0, 0, 0};
    for (char c : text) for (int d = 0; d < 4; d++) if (c == D[d]) cnt[d]++;
    int best = -1;
    for (int d = 0; d < 4; d++) if (cnt[d] >= 1 && (best < 0 || cnt[d] > cnt[best])) best = d;
    const uint32_t before = pr.n;       // messages learned so far (this one is counted already: learn() runs first)
    if (best >= 0) pr.delimVotes[best]++;
    if (!pr.delim) {
        for (int d = 0; d < 4; d++) if (pr.delimVotes[d] >= 3 && pr.delimVotes[d] * 10 >= before * 6) pr.delim = D[d];
    }
    if (pr.delim) {
        std::vector<std::string> seg;
        std::string cur;
        for (char c : text) { if (c == pr.delim) { seg.push_back(trim(cur)); cur.clear(); } else cur += c; }
        seg.push_back(trim(cur));
        if (pr.segs.size() < seg.size() && seg.size() <= 10) pr.segs.resize(seg.size());
        for (size_t k = 0; k < seg.size() && k < pr.segs.size(); k++) {
            Profile::Seg& s = pr.segs[k];
            // the role learned so far gives the field its name
            if (s.n >= 4 && !seg[k].empty()) {
                const char* key = nullptr;
                if (s.vals.size() <= std::max<size_t>(2, s.n / 3)) key = "Type";
                else if (s.digits * 10 >= s.n * 8) key = "Number";
                else if (s.addr * 10 >= s.n * 6) key = "Address";
                else if (s.pos * 10 >= s.n * 6) key = "Position";
                if (key) m.fields.push_back({key, seg[k]});
            }
            s.n++;
            if (digitShare(seg[k]) >= 0.6) s.digits++;
            if (looksAddress(seg[k])) s.addr++;
            if (!findCoordinates(seg[k]).empty()) s.pos++;
            if (s.vals.size() < 64 || s.vals.count(seg[k])) s.vals[seg[k]]++;
            else s.vals[std::string("\x01") + std::to_string(s.n)] = 1;   // too many different values: not a type field
        }
    }
}

void PagerBrain::analyse(PagerMessage& m, Profile& pr, int64_t wall) {
    const std::string& text = m.text;
    // explicit "key: value" pairs
    {
        static const std::regex kv(R"(\b([A-Za-z][A-Za-z ]{1,15}?)\s*[:=]\s*([^,;|]{1,48}))");
        int k = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), kv), end; it != end && k < 6; ++it, k++)
            m.fields.push_back({trim((*it)[1].str()), trim((*it)[2].str())});
    }
    segments(m, pr, text);
    // the first word as the type, once this capcode has shown that it repeats
    {
        const std::vector<std::string> w = wordsOf(text);
        if (!w.empty() && text.size() >= w[0].size() && lowerOf(text.substr(0, w[0].size())) == lowerOf(w[0])) {
            const std::string f = lowerOf(w[0]);
            const uint32_t seen = pr.first.count(f) ? pr.first[f] : 0;
            if (pr.n >= 4 && seen >= 3 && pr.first.size() <= std::max<size_t>(2, pr.n / 3) && pr.delim == 0) m.fields.push_back({"Type", w[0]});
            if (pr.first.size() < 64 || pr.first.count(f)) pr.first[f]++;
        }
    }
    const std::vector<std::string> keys = jobKeys(text);
    for (const std::string& k : keys) m.fields.push_back({"Job", k});
    if (looksAddress(text) && std::none_of(m.fields.begin(), m.fields.end(), [](const PagerField& f) { return f.key == "Address"; })) {
        static const std::regex re(R"(\b\d{1,5}[A-Za-z]?\s+(?:[A-Za-z']+\s+){0,3}(?:st|street|rd|road|ave|avenue|dr|drive|ln|lane|ct|court|pl|place|hwy|highway|blvd|way|cres|crescent|tce|terrace|cl|close|gatan|straat|laan)\b|\b[A-Za-z]+(?:stra(?:ss|ß)e|str\.|gasse|weg|platz|gatan|straat|laan)\s*\d+)", std::regex::icase);
        std::smatch mt;
        if (std::regex_search(text, mt, re)) m.fields.push_back({"Address", mt.str()});
    }
    // position
    const std::vector<GeoHit> geo = findCoordinates(text);
    if (!geo.empty()) {
        m.hasPos = true; m.lat = geo[0].lat; m.lon = geo[0].lon; m.place = geo[0].kind;
        char b[48];
        snprintf(b, sizeof b, "%.5f, %.5f", m.lat, m.lon);
        m.fields.push_back({"Position", b});
    } else if (!places_.empty()) {
        const std::string l = lowerOf(text);
        const Place* best = nullptr;
        for (const Place& p : places_) {
            size_t at = l.find(p.lower);
            while (at != std::string::npos) {
                const bool lb = at == 0 || !isAlnum((unsigned char)l[at - 1]);
                const bool rb = at + p.lower.size() >= l.size() || !isAlnum((unsigned char)l[at + p.lower.size()]);
                if (lb && rb) { if (!best || p.lower.size() > best->lower.size()) best = &p; break; }
                at = l.find(p.lower, at + 1);
            }
        }
        if (best) { m.hasPos = true; m.lat = best->lat; m.lon = best->lon; m.place = best->name; m.fields.push_back({"Place", best->name}); }
    }
    if (m.hasPos && !m.posApprox) {
        pr.latSum += m.lat; pr.lonSum += m.lon; pr.posN++;
        pr.latMin = std::min(pr.latMin, m.lat); pr.latMax = std::max(pr.latMax, m.lat);
        pr.lonMin = std::min(pr.lonMin, m.lon); pr.lonMax = std::max(pr.lonMax, m.lon);
    } else if (!m.hasPos && pr.posN >= 3 && pr.latMax - pr.latMin < 0.5 && pr.lonMax - pr.lonMin < 0.7) {
        m.hasPos = true; m.posApprox = true; m.lat = pr.latSum / pr.posN; m.lon = pr.lonSum / pr.posN; m.place = "capcode area";
    }
    if (m.hasPos) stats.pagesWithPos++;
    assignIncident(m, wall);
}

void PagerBrain::assignIncident(PagerMessage& m, int64_t wall) {
    Recent r;
    r.wall = wall; r.cap = m.address; r.flex = m.flex;
    r.keys = jobKeys(m.text);
    {
        std::set<std::string> t;
        for (const std::string& w : wordsOf(m.text)) if (w.size() >= 3 && digitShare(w) < 0.5) t.insert(lowerOf(w));
        r.toks.assign(t.begin(), t.end());
    }
    r.hasPos = m.hasPos && !m.posApprox; r.lat = m.lat; r.lon = m.lon;
    while (!recent_.empty() && (wall - recent_.front().wall > 900 || wall < recent_.front().wall)) recent_.pop_front();
    uint32_t id = 0;
    for (size_t k = recent_.size(); k-- > 0 && !id;) {
        const Recent& o = recent_[k];
        bool same = false;
        for (const std::string& a : r.keys) if (std::find(o.keys.begin(), o.keys.end(), a) != o.keys.end()) same = true;
        if (!same && r.hasPos && o.hasPos && distanceKm(r.lat, r.lon, o.lat, o.lon) < 0.3) same = true;
        if (!same && r.toks.size() >= 4 && o.toks.size() >= 4) {
            size_t common = 0, i = 0, j = 0;
            while (i < r.toks.size() && j < o.toks.size()) {
                if (r.toks[i] == o.toks[j]) { common++; i++; j++; }
                else if (r.toks[i] < o.toks[j]) i++; else j++;
            }
            const double jac = (double)common / (double)(r.toks.size() + o.toks.size() - common);
            same = jac >= 0.6;
        }
        if (same) id = o.incident;
    }
    PagerIncident* inc = nullptr;
    if (id) for (PagerIncident& x : inc_) if (x.id == id) inc = &x;
    if (!inc) {
        id = nextIncident_++;
        PagerIncident n;
        n.id = id; n.firstWall = wall;
        n.title = m.text.substr(0, 48);
        for (const PagerField& f : m.fields) if (f.key == "Type") { n.title = f.value; break; }
        inc_.push_back(n);
        if (inc_.size() > kMaxIncidents) inc_.erase(inc_.begin());
        inc = &inc_.back();
    }
    inc->lastWall = wall;
    inc->count++;
    if (std::find(inc->capcodes.begin(), inc->capcodes.end(), m.address) == inc->capcodes.end()) inc->capcodes.push_back(m.address);
    if (m.hasPos && !m.posApprox && !inc->hasPos) { inc->hasPos = true; inc->lat = m.lat; inc->lon = m.lon; }
    r.incident = id;
    m.incident = id;
    recent_.push_back(std::move(r));
    if (recent_.size() > kMaxRecent) recent_.pop_front();
}

std::vector<PagerIncident> PagerBrain::incidents() const {
    std::vector<PagerIncident> v;
    for (const PagerIncident& x : inc_) if (x.count >= 2) v.push_back(x);
    std::sort(v.begin(), v.end(), [](const PagerIncident& a, const PagerIncident& b) { return a.lastWall > b.lastWall; });
    return v;
}

// ---------------------------------------------------------------- one message

void PagerBrain::process(PagerMessage& m, int64_t wall) {
    if (m.selcall || m.type == kPagerTone || m.type == kPagerSecure || m.type == kPagerBinary) return;
    if (!opt.repair && !opt.intel) return;
    if (m.type == kPagerNumeric && !opt.intel) return;
    const bool textual = m.type == kPagerAlpha;
    // privacy: medical-looking text is not touched
    if (looksMedical(m.text) || (!m.repairedText.empty() && looksMedical(m.repairedText))) {
        m.sensitive = true;
        if (!opt.analyseMedical) { stats.sensitivePages++; return; }
    }
    Profile& pr = prof_[profKey(m)];
    pr.last = wall;
    if (prof_.size() > kMaxProfiles) {
        auto old = prof_.begin();
        for (auto it = prof_.begin(); it != prof_.end(); ++it) if (it->second.last < old->second.last && &it->second != &pr) old = it;
        if (&old->second != &pr) prof_.erase(old);
    }
    if (opt.repair && textual && m.damaged && !m.flex && !m.alphaRaw.empty()) repairText(m, pr);
    if (textual && !m.damaged && m.text.size() >= 4 && opt.repair) {
        learn(m.text, pr);
    }
    if (opt.intel && !m.text.empty()) analyse(m, pr, wall);
}

} // namespace pager
} // namespace dect2
