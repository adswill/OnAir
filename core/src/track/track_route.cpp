// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/track/track_route.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2::track {

const Airport* findAirport(const std::string& icao) {
    size_t n; const Airport* t = airportTable(n);
    if (icao.size() != 4) return nullptr;
    const Airport* e = std::lower_bound(t, t + n, icao, [](const Airport& a, const std::string& k) { return std::strncmp(a.icao, k.c_str(), 4) < 0; });
    return e != t + n && std::strncmp(e->icao, icao.c_str(), 4) == 0 ? e : nullptr;
}

namespace {
bool isUp(char c) { return c >= 'A' && c <= 'Z'; }
bool isDig(char c) { return c >= '0' && c <= '9'; }
bool icaoAt(const std::string& s, size_t i) {   // four capital letters standing alone
    if (i + 4 > s.size()) return false;
    for (size_t k = 0; k < 4; k++) if (!isUp(s[i + k])) return false;
    if (i > 0 && isUp(s[i - 1])) return false;
    if (i + 4 < s.size() && isUp(s[i + 4])) return false;
    return true;
}
int hhmm(const std::string& s, size_t i) {   // 4 digits, valid time -> seconds, else -1
    if (i + 4 > s.size()) return -1;
    for (size_t k = 0; k < 4; k++) if (!isDig(s[i + k])) return -1;
    const int h = (s[i] - '0') * 10 + (s[i + 1] - '0'), m = (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
    return h < 24 && m < 60 ? h * 3600 + m * 60 : -1;
}
size_t skipSep(const std::string& s, size_t i) { while (i < s.size() && (s[i] == ' ' || s[i] == '/' || s[i] == ':' || s[i] == '=')) i++; return i; }
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
}  // namespace

bool parseRouteText(const std::string& label, const std::string& textIn, RouteInfo& o) {
    const std::string text = upper(textIn);
    bool any = false;
    // Q labels: the layouts of acarsDecodeText: origin/destination 4 letters, times 4 digits
    if (label.size() == 2 && label[0] == 'Q') {
        struct F { char kind; int off; };
        struct L { char b; F f[6]; };
        static const L kQ[] = {
            {'1', {{'o', 0}, {'O', 4}, {'F', 8}, {'N', 12}, {'I', 16}, {'d', 24}}}, {'2', {{'o', 0}, {'E', 4}}}, {'A', {{'o', 0}, {'O', 4}}},
            {'B', {{'o', 0}, {'F', 4}}}, {'C', {{'o', 0}, {'N', 4}}}, {'D', {{'o', 0}, {'I', 4}}}, {'E', {{'o', 0}, {'O', 4}, {'d', 8}}},
            {'F', {{'o', 0}, {'F', 4}, {'d', 8}}}, {'G', {{'o', 0}, {'O', 4}, {'I', 8}}}, {'H', {{'o', 0}, {'O', 4}}}, {'K', {{'o', 0}, {'N', 4}, {'d', 8}}},
            {'L', {{'d', 0}, {'I', 8}, {'o', 13}}}, {'M', {{'d', 0}, {'o', 8}}}, {'N', {{'d', 4}, {'E', 8}}}, {'P', {{'o', 0}, {'d', 4}, {'O', 8}}},
            {'Q', {{'o', 0}, {'d', 4}, {'F', 8}}}, {'R', {{'o', 0}, {'d', 4}, {'N', 8}}}, {'S', {{'o', 0}, {'d', 4}, {'I', 8}}}, {'T', {{'o', 0}, {'d', 4}, {'O', 8}, {'I', 12}}}};
        for (const auto& q : kQ) {
            if (q.b != label[1]) continue;
            RouteInfo r; bool ok = true;
            for (const auto& f : q.f) {
                if (!f.kind) break;
                const size_t off = (size_t)f.off;
                if (f.kind == 'o' || f.kind == 'd') {
                    if (off + 4 > text.size()) { ok = false; break; }
                    std::string a = text.substr(off, 4);
                    for (char c : a) if (!isUp(c)) ok = false;
                    (f.kind == 'o' ? r.origin : r.dest) = a;
                } else {
                    const int t = hhmm(text, off);
                    if (t < 0) { ok = false; break; }
                    (f.kind == 'O' ? r.outSec : f.kind == 'F' ? r.offSec : f.kind == 'N' ? r.onSec : f.kind == 'I' ? r.inSec : r.etaSec) = t;
                }
            }
            if (ok && r.any()) { o = r; return true; }
            break;
        }
    }
    // ARINC 702A style flight plan: "FPN/RI:DA:KLAX:AA:KSFO:CR:..."
    for (size_t p = text.find("DA:"); p != std::string::npos; p = text.find("DA:", p + 1)) {
        if (p > 0 && isUp(text[p - 1])) continue;
        if (icaoAt(text, p + 3)) { o.origin = text.substr(p + 3, 4); any = true; break; }
    }
    for (size_t p = text.find("AA:"); p != std::string::npos; p = text.find("AA:", p + 1)) {
        if (p > 0 && isUp(text[p - 1])) continue;
        if (icaoAt(text, p + 3)) { o.dest = text.substr(p + 3, 4); any = true; break; }
    }
    // ICAO flight plan "(FPL-ABC123-IS-B77W/H-...-OMDB1030-N0480F360 ...-EGLL0710 EGKK-...": departure and destination fields are 4 letters + 4 digits
    if (text.find("FPL-") != std::string::npos || text.find("-FPL") != std::string::npos) {
        std::vector<std::pair<std::string, int>> hits;
        for (size_t i = 0; i + 8 <= text.size(); i++) {
            if (text[i] != '-' || !icaoAt(text, i + 1) || hhmm(text, i + 5) < 0) continue;
            hits.push_back({text.substr(i + 1, 4), hhmm(text, i + 5)});
        }
        if (!hits.empty()) { o.origin = hits[0].first; any = true; }
        if (hits.size() >= 2) { o.dest = hits[1].first; any = true; }
    }
    // keyword forms
    static const char* kDest[] = {"DEST", "DST", "/DS", "DES ", "ARR", "TO "};
    if (o.dest.empty()) {
        for (const char* k : kDest) {
            for (size_t p = text.find(k); p != std::string::npos; p = text.find(k, p + 1)) {
                if (p > 0 && isUp(text[p - 1])) continue;
                size_t q = skipSep(text, p + std::strlen(k));
                if (icaoAt(text, q) && findAirport(text.substr(q, 4))) { o.dest = text.substr(q, 4); any = true; break; }
            }
            if (!o.dest.empty()) break;
        }
    }
    for (size_t p = text.find("ETA"); p != std::string::npos; p = text.find("ETA", p + 1)) {
        if (p > 0 && isUp(text[p - 1])) continue;
        size_t q = skipSep(text, p + 3);
        const int t = hhmm(text, q);
        if (t >= 0) { o.etaSec = t; any = true; break; }
    }
    for (const char* k : {"NXT WPT", "NEXT WPT", "NXT WP", "NEXT WP"}) {
        size_t p = text.find(k);
        if (p == std::string::npos) continue;
        size_t q = skipSep(text, p + std::strlen(k));
        size_t e = q; while (e < text.size() && (isUp(text[e]) || isDig(text[e])) && e - q < 6) e++;
        if (e - q >= 2) { o.nextWpt = text.substr(q, e - q); any = true; break; }
    }
    if (any && label.size() == 2 && label[0] == 'Q') return true;
    return any;
}

TextPosition parseTextPosition(const std::string& hint, const std::string& textIn) {
    TextPosition r;
    const std::string s = upper(textIn);
    for (size_t i = 0; i < s.size(); i++) {
        if ((s[i] != 'N' && s[i] != 'S') || (i > 0 && isDig(s[i - 1]))) continue;
        size_t j = i + 1;
        while (j < s.size() && s[j] == ' ') j++;
        if (j >= s.size() || !isDig(s[j])) continue;
        // collect the latitude token: digits, '.', and an internal space for "51 28.3"
        auto num = [&](size_t& k, std::string& tok, bool allowSpace) {
            tok.clear();
            while (k < s.size() && (isDig(s[k]) || s[k] == '.' || (allowSpace && s[k] == ' ' && !tok.empty() && k + 1 < s.size() && isDig(s[k + 1]) && tok.find(' ') == std::string::npos && tok.find('.') == std::string::npos))) tok += s[k++];
        };
        std::string la; num(j, la, true);
        if (la.empty()) continue;
        while (j < s.size() && (s[j] == ' ' || s[j] == ',' || s[j] == '/')) j++;
        if (j >= s.size() || (s[j] != 'E' && s[j] != 'W')) continue;
        const char ew = s[j]; j++;
        while (j < s.size() && s[j] == ' ') j++;
        std::string lo; num(j, lo, true);
        if (lo.empty()) continue;
        double latv = 0, lonv = 0, res = 0;
        auto digitsOnly = [](const std::string& t) { for (char c : t) if (!isDig(c)) return false; return true; };
        if (la.find('.') != std::string::npos || la.find(' ') != std::string::npos) {
            const size_t sp = la.find(' ');
            if (sp != std::string::npos) {             // degrees, minutes with decimals
                const double d = std::atof(la.substr(0, sp).c_str()), m = std::atof(la.substr(sp + 1).c_str());
                latv = d + m / 60.0;
                const size_t dot = la.find('.', sp); const int dec = dot == std::string::npos ? 0 : (int)(la.size() - dot - 1);
                res = (dec == 0 ? 1.0 : 1.0 / std::pow(10.0, dec)) / 60.0;
                const size_t sp2 = lo.find(' ');
                if (sp2 == std::string::npos) continue;
                lonv = std::atof(lo.substr(0, sp2).c_str()) + std::atof(lo.substr(sp2 + 1).c_str()) / 60.0;
            } else {                                   // decimal degrees
                latv = std::atof(la.c_str()); lonv = std::atof(lo.c_str());
                const size_t dot = la.find('.'); res = 1.0 / std::pow(10.0, (double)(la.size() - dot - 1));
            }
        } else if (digitsOnly(la) && digitsOnly(lo) && la.size() == 5 && lo.size() == 6) {
            const bool thousandths = hint == "20";
            if (thousandths) { latv = std::atoi(la.substr(0, 2).c_str()) + std::atoi(la.substr(2).c_str()) / 1000.0; lonv = std::atoi(lo.substr(0, 3).c_str()) + std::atoi(lo.substr(3).c_str()) / 1000.0; res = 0.001; }
            else {
                latv = std::atoi(la.substr(0, 2).c_str()) + (std::atoi(la.substr(2, 2).c_str()) + std::atoi(la.substr(4).c_str()) / 10.0) / 60.0;
                lonv = std::atoi(lo.substr(0, 3).c_str()) + (std::atoi(lo.substr(3, 2).c_str()) + std::atoi(lo.substr(5).c_str()) / 10.0) / 60.0;
                res = 0.1 / 60.0;
            }
        } else if (digitsOnly(la) && digitsOnly(lo) && la.size() == 4 && lo.size() == 5) {   // DDMM DDDMM: whole minutes
            latv = std::atoi(la.substr(0, 2).c_str()) + std::atoi(la.substr(2).c_str()) / 60.0;
            lonv = std::atoi(lo.substr(0, 3).c_str()) + std::atoi(lo.substr(3).c_str()) / 60.0; res = 1.0 / 60.0;
        } else continue;
        if (s[i] == 'S') latv = -latv;
        if (ew == 'W') lonv = -lonv;
        if (std::fabs(latv) > 90 || std::fabs(lonv) > 180) continue;
        r.ok = true; r.lat = latv; r.lon = lonv; r.resolutionDeg = res;
        return r;
    }
    return r;
}

double losRangeNm(double altFt, double antFt) { return 1.23 * (std::sqrt(std::max(0.0, altFt)) + std::sqrt(std::max(0.0, antFt))); }

void WindField::add(double t, double lat, double lon, double alt, double e, double n) {
    if (!std::isfinite(e + n)) return;
    const S s{t, lat, lon, alt, e, n};
    if (s_.size() < 2048) s_.push_back(s); else { s_[head_] = s; head_ = (head_ + 1) % s_.size(); }
}

bool WindField::at(double t, double lat, double lon, double alt, double& e, double& n, double& sigma) const {
    double sw = 0, se = 0, sn = 0; int cnt = 0;
    for (const S& s : s_) {
        if (t - s.t > maxAgeSec || s.t - t > 60 || std::fabs(s.alt - alt) > maxDzM) continue;
        const double d = gcDistM(lat, lon, s.lat, s.lon) / 1000.0;
        if (d > maxKm) continue;
        const double w = 1.0 / (10.0 + d * d / 100.0) * std::exp(-(t - s.t) / 900.0);
        sw += w; se += w * s.e; sn += w * s.n; cnt++;
    }
    if (cnt == 0 || sw <= 0) return false;
    e = se / sw; n = sn / sw;
    sigma = 6.0 + 10.0 / std::sqrt((double)cnt);          // m/s: spatial decorrelation of winds is large; this is a rule of thumb, see the doc
    return true;
}

}  // namespace dect2::track
