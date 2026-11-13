// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/marine_navtex_geo.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>

namespace dect2 {
namespace marine {
namespace {

std::string upper(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}

// "°" and the minute marks come in as UTF-8: turn them into plain separators so the patterns stay simple
std::string plain(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xB0) { o += '-'; i++; }               // degree sign
        else if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0x99 || (unsigned char)s[i + 2] == 0xB2)) { o += ' '; i += 2; }   // right quote, prime
        else if (c == '\'') o += ' ';
        else if (c < 0x80) o += (char)c;
        else o += ' ';
    }
    return o;
}

double num(const std::string& s) {
    std::string t = s;
    for (auto& c : t) if (c == ',') c = '.';
    return std::atof(t.c_str());
}

bool build(const std::string& dStr, const std::string& mStr, char hemi, double maxDeg, double& out) {
    if (dStr.empty()) return false;
    const double d = num(dStr);
    const double m = mStr.empty() ? 0 : num(mStr);
    if (m >= 60.0 || d > maxDeg || d + m / 60.0 > maxDeg) return false;
    out = d + m / 60.0;
    if (hemi == 'S' || hemi == 'W') out = -out;
    return true;
}

struct Hit { size_t pos, len; double lat, lon; };

void scan(const std::string& t, const std::regex& re, int dLat, int mLat, int hLat, int dLon, int mLon, int hLon, bool decimal, std::vector<Hit>& out) {
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        double la, lo;
        const char hl = m[hLat].str()[0], ho = m[hLon].str()[0];
        if (decimal) {
            if (!build(m[dLat].str(), "", hl, 90, la) || !build(m[dLon].str(), "", ho, 180, lo)) continue;
        } else {
            if (!build(m[dLat].str(), m[mLat].str(), hl, 90, la) || !build(m[dLon].str(), m[mLon].str(), ho, 180, lo)) continue;
        }
        out.push_back({(size_t)m.position(0), (size_t)m.length(0), la, lo});
    }
}

} // namespace

std::vector<GeoRef> navtexPositions(const std::string& text) {
    const std::string t = plain(upper(text));
    // decimal degrees: 56.5N 3.25E
    static const std::regex dec(R"((?:^|[^\d.])(\d{1,2}[.,]\d+)\s?-?\s?([NS])[\s,/]*(\d{1,3}[.,]\d+)\s?-?\s?([EW]))");
    // degrees and minutes with the letter behind: 56-30N 003-15E, 5630.5N 00315.2E, 56 30 N 003 15 E
    static const std::regex suf(R"((?:^|[^\d.])(\d{1,2})(?:\s?-\s?|\s)?(\d{2}(?:[.,]\d+)?)?\s?([NS])[\s,/]*(?:LONG?[A-Z]*\.?[\s:]*)?(\d{1,3})(?:\s?-\s?|\s)?(\d{2}(?:[.,]\d+)?)?\s?([EW])(?![A-Z]))");
    // the letter in front: N56 30 E003 15, N 56-30 E 003-15
    static const std::regex pre(R"((?:^|[^A-Z])([NS])\s?(\d{1,2})(?:\s?-\s?|\s)?(\d{2}(?:[.,]\d+)?)?[\s,/]+([EW])\s?(\d{1,3})(?:\s?-\s?|\s)?(\d{2}(?:[.,]\d+)?)?(?![\d]))");
    std::vector<Hit> hits;
    {
        std::vector<Hit> a;
        scan(t, dec, 1, 0, 2, 3, 0, 4, true, a);
        for (auto& h : a) hits.push_back(h);
    }
    {
        std::vector<Hit> b;
        scan(t, suf, 1, 2, 3, 4, 5, 6, false, b);
        for (auto& h : b) hits.push_back(h);
    }
    {
        // the "prefix" pattern has its groups in another order: hemisphere, degrees, minutes, hemisphere, degrees, minutes
        for (auto it = std::sregex_iterator(t.begin(), t.end(), pre); it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            double la, lo;
            if (!build(m[2].str(), m[3].str(), m[1].str()[0], 90, la) || !build(m[5].str(), m[6].str(), m[4].str()[0], 180, lo)) continue;
            hits.push_back({(size_t)m.position(0), (size_t)m.length(0), la, lo});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    std::vector<GeoRef> out;
    size_t end = 0;
    for (const Hit& h : hits) {
        if (!out.empty() && h.pos < end) continue;               // a second pattern read the same words
        GeoRef g;
        g.lat = h.lat; g.lon = h.lon;
        size_t a = h.pos;
        while (a < t.size() && t[a] == ' ') a++;
        g.text = text.substr(std::min(a, text.size()), std::min(h.len, text.size() - std::min(a, text.size())));
        while (!g.text.empty() && (g.text.front() == ' ' || g.text.front() == '\n')) g.text.erase(g.text.begin());
        out.push_back(g);
        end = h.pos + h.len;
    }
    return out;
}

namespace {
struct AreaDef { const char* name; const char* aliases; double la0, la1, lo0, lo1; };
// Coarse boxes of the seas NAVTEX warnings name. Aliases are upper case, separated by '|'.
const AreaDef kAreas[] = {
    {"Gulf of Finland", "GULF OF FINLAND", 59.0, 60.8, 22.5, 30.3},
    {"Gulf of Bothnia", "GULF OF BOTHNIA|BOTHNIAN BAY|BOTHNIAN SEA|BAY OF BOTHNIA", 60.0, 66.0, 17.0, 26.0},
    {"Gulf of Riga", "GULF OF RIGA", 57.0, 58.5, 21.5, 24.5},
    {"Baltic Sea", "BALTIC", 53.5, 66.0, 9.5, 30.5},
    {"Skagerrak", "SKAGERRAK", 57.0, 59.5, 7.0, 11.5},
    {"Kattegat", "KATTEGAT", 55.5, 58.0, 10.5, 13.0},
    {"Oresund", "ORESUND|OeRESUND|OERESUND", 55.3, 56.2, 12.5, 13.0},
    {"Dover Strait", "DOVER STRAIT|STRAIT OF DOVER|PAS DE CALAIS", 50.5, 51.5, 0.8, 2.0},
    {"English Channel", "ENGLISH CHANNEL|THE CHANNEL|WESTERN CHANNEL|EASTERN CHANNEL", 48.5, 51.2, -6.0, 2.0},
    {"North Sea", "NORTH SEA|NORDSEE|NOORDZEE", 51.0, 61.0, -4.0, 9.0},
    {"Irish Sea", "IRISH SEA|NORTH CHANNEL", 52.0, 55.5, -6.5, -3.0},
    {"Celtic Sea", "CELTIC SEA", 49.0, 52.5, -10.0, -5.0},
    {"Hebrides", "HEBRIDES|MINCH", 55.0, 59.0, -8.5, -5.0},
    {"Bay of Biscay", "BAY OF BISCAY|BISCAY|GOLFE DE GASCOGNE", 43.0, 48.5, -10.0, -1.0},
    {"Strait of Gibraltar", "STRAIT OF GIBRALTAR|GIBRALTAR STRAIT|GIBRALTAR", 35.5, 36.5, -6.5, -5.0},
    {"Strait of Bonifacio", "BONIFACIO", 41.0, 41.6, 8.6, 9.8},
    {"Adriatic Sea", "ADRIATIC", 39.5, 46.0, 12.0, 20.0},
    {"Ionian Sea", "IONIAN", 35.0, 40.5, 15.0, 22.0},
    {"Aegean Sea", "AEGEAN", 35.0, 41.0, 22.5, 28.0},
    {"Sea of Marmara", "MARMARA", 40.3, 41.1, 26.5, 30.0},
    {"Sea of Azov", "SEA OF AZOV|AZOV", 45.3, 47.3, 34.8, 39.5},
    {"Black Sea", "BLACK SEA", 41.0, 47.0, 27.5, 41.8},
    {"Mediterranean", "MEDITERRANEAN", 30.0, 46.0, -6.0, 36.0},
    {"Red Sea", "RED SEA", 12.5, 30.0, 32.0, 44.0},
    {"Gulf of Aden", "GULF OF ADEN", 11.0, 15.0, 43.0, 52.0},
    {"Strait of Hormuz", "HORMUZ", 25.5, 27.0, 55.5, 57.0},
    {"Gulf of Oman", "GULF OF OMAN", 22.5, 26.5, 56.0, 61.0},
    {"Persian Gulf", "PERSIAN GULF|ARABIAN GULF", 24.0, 30.5, 48.0, 56.5},
    {"Arabian Sea", "ARABIAN SEA", 8.0, 25.0, 50.0, 75.0},
    {"Bay of Bengal", "BAY OF BENGAL", 5.0, 23.0, 80.0, 100.0},
    {"Strait of Malacca", "MALACCA", 1.0, 6.0, 95.0, 104.5},
    {"Singapore Strait", "SINGAPORE STRAIT", 1.0, 1.6, 103.4, 104.3},
    {"South China Sea", "SOUTH CHINA SEA", 0.0, 23.0, 99.0, 121.0},
    {"East China Sea", "EAST CHINA SEA", 23.0, 33.0, 117.0, 130.0},
    {"Yellow Sea", "YELLOW SEA", 32.0, 41.0, 119.0, 127.0},
    {"Sea of Japan", "SEA OF JAPAN|EAST SEA", 33.0, 52.0, 127.0, 142.0},
    {"Gulf of Mexico", "GULF OF MEXICO", 18.0, 30.5, -98.0, -80.5},
    {"Caribbean Sea", "CARIBBEAN", 9.0, 22.0, -88.0, -60.0},
    {"Norwegian Sea", "NORWEGIAN SEA", 62.0, 72.0, -5.0, 15.0},
    {"Barents Sea", "BARENTS", 66.0, 80.0, 15.0, 60.0},
    {"White Sea", "WHITE SEA", 63.5, 68.0, 32.0, 44.0},
    {"North Atlantic", "NORTH ATLANTIC", 0.0, 65.0, -60.0, -5.0},
    {"Gulf of Guinea", "GULF OF GUINEA", -2.0, 6.0, -10.0, 10.0},
    {"Mozambique Channel", "MOZAMBIQUE CHANNEL", -26.0, -10.0, 32.0, 46.0},
    {"Tasman Sea", "TASMAN SEA", -45.0, -30.0, 150.0, 175.0},
    {"Bass Strait", "BASS STRAIT", -40.5, -38.0, 144.0, 149.0},
};

bool wordAt(const std::string& t, size_t pos, size_t len) {
    const bool l = pos == 0 || !std::isalpha((unsigned char)t[pos - 1]);
    const bool r = pos + len >= t.size() || !std::isalpha((unsigned char)t[pos + len]);
    return l && r;
}
} // namespace

const std::vector<AreaRef>& navtexAreaTable() {
    static const std::vector<AreaRef> v = [] {
        std::vector<AreaRef> o;
        for (const AreaDef& a : kAreas) { AreaRef r; r.name = a.name; r.latMin = a.la0; r.latMax = a.la1; r.lonMin = a.lo0; r.lonMax = a.lo1; o.push_back(r); }
        return o;
    }();
    return v;
}

std::vector<AreaRef> navtexAreas(const std::string& text) {
    const std::string t = upper(text);
    std::vector<AreaRef> out;
    const auto& tab = navtexAreaTable();
    for (size_t i = 0; i < tab.size(); i++) {
        const std::string al = std::string(kAreas[i].aliases) + "|";
        size_t s = 0;
        bool hit = false;
        while (s < al.size() && !hit) {
            const size_t e = al.find('|', s);
            const std::string a = al.substr(s, e - s);
            s = e + 1;
            if (a.empty()) continue;
            for (size_t p = t.find(a); p != std::string::npos; p = t.find(a, p + 1)) if (wordAt(t, p, a.size())) { hit = true; break; }
        }
        if (hit) out.push_back(tab[i]);
    }
    return out;
}

} // namespace marine
} // namespace dect2
