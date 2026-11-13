// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM / DAB played list: see played_log.h.
#include "dect2/dab/played_log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string iso(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
int64_t fromIso(const std::string& s) {
    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    Y -= M <= 2;
    const int64_t era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400);
    const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + (int64_t)doe - 719468) * 86400 + h * 3600 + m * 60 + sec;
}
std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c == '\n' ? ' ' : c; }
    return o + "\"";
}
std::vector<std::string> csvSplit(const std::string& l) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < l.size(); i++) {
        const char c = l[i];
        if (q) { if (c == '"') { if (i + 1 < l.size() && l[i + 1] == '"') { cur += '"'; i++; } else q = false; } else cur += c; }
        else if (c == '"') q = true;
        else if (c == ',') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}
}  // namespace

bool PlayedLog::note(const std::string& source, const std::string& station0, double freqMhz, const std::string& artist0, const std::string& title0, int64_t utc) {
    const std::string artist = trim(artist0), title = trim(title0), station = trim(station0);
    if (artist.empty() && title.empty()) return false;
    // the newest entry of this station
    PlayedItem* last = nullptr;
    for (auto it = items_.rbegin(); it != items_.rend() && it - items_.rbegin() < 50; ++it)
        if (it->source == source && it->station == station) { last = &*it; break; }
    if (last) {
        const bool same = lower(last->artist) == lower(artist) && lower(last->title) == lower(title);
        if (same && utc - last->lastUtc <= kSameSongSec) { last->lastUtc = std::max(last->lastUtc, utc); return false; }
        // a tag that arrives after the other one, within a minute: the same song, completed
        const bool fills = utc - last->utc <= 60 &&
                           ((last->artist.empty() && lower(last->title) == lower(title) && !artist.empty()) || (last->title.empty() && lower(last->artist) == lower(artist) && !title.empty()));
        if (fills) { last->artist = artist; last->title = title; last->lastUtc = utc; return false; }
        // an older song again later is a new entry; a one-field entry the station replaced at once is overwritten
        if (utc - last->utc <= 10 && (last->artist.empty() || last->title.empty())) { last->artist = artist; last->title = title; last->lastUtc = utc; return false; }
    }
    PlayedItem p;
    p.utc = p.lastUtc = utc; p.source = source; p.station = station; p.freqMhz = freqMhz; p.artist = artist; p.title = title;
    add(p);
    return true;
}

void PlayedLog::add(const PlayedItem& p) {
    items_.push_back(p);
    if (items_.size() > kKeep) items_.erase(items_.begin(), items_.begin() + (long)(items_.size() - kKeep));
}

std::vector<PlayedItem> PlayedLog::find(const std::string& query, size_t max) const {
    std::vector<std::string> words;
    std::stringstream ss(lower(query));
    std::string w;
    while (ss >> w) words.push_back(w);
    std::vector<PlayedItem> out;
    for (auto it = items_.rbegin(); it != items_.rend() && out.size() < max; ++it) {
        const std::string h = lower(it->station + "\n" + it->artist + "\n" + it->title);
        bool ok = true;
        for (const auto& x : words) if (h.find(x) == std::string::npos) ok = false;
        if (ok) out.push_back(*it);
    }
    return out;
}

std::string playedCsv(const std::vector<PlayedItem>& v) {
    std::string o = "utc,source,station,freq_mhz,artist,title\n";
    char f[24];
    for (const auto& p : v) { std::snprintf(f, sizeof f, "%.3f", p.freqMhz); o += iso(p.utc) + "," + csvField(p.source) + "," + csvField(p.station) + "," + f + "," + csvField(p.artist) + "," + csvField(p.title) + "\n"; }
    return o;
}

std::vector<PlayedItem> playedFromCsv(const std::string& text) {
    std::vector<PlayedItem> v;
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const auto f = csvSplit(l);
        if (f.size() < 6) continue;
        PlayedItem p;
        p.utc = p.lastUtc = fromIso(f[0]);
        if (!p.utc) continue;
        p.source = f[1]; p.station = f[2]; p.freqMhz = std::atof(f[3].c_str()); p.artist = f[4]; p.title = f[5];
        v.push_back(p);
    }
    return v;
}

}  // namespace dect2
