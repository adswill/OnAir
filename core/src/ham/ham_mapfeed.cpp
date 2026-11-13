// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ham/ham_mapfeed.h"
#include "dect2/ham/ham.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace dect2::ham::mapfeed {

static std::string up(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }

static std::string normMode(const std::string& m) {
    std::string o;
    for (char c : m) if (std::isalnum((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
    if (o == "FT8" || o == "FT4" || o == "WSPR" || o == "JS8" || o == "QSO") return o;
    return "";
}

bool roundedCentre(const std::string& grid, int chars, double& lat, double& lon) {
    if (!hamGridValid(grid)) return false;
    const size_t n = (chars == 6 && grid.size() >= 6) ? 6 : 4;
    return fromMaidenhead(grid.substr(0, n), lat, lon);
}

std::string extraJson(const std::string& mode, double freqHz, const std::string& to, bool hasPos, double tlat, double tlon) {
    char b[96];
    std::string j = "{\"m\":\"" + mode + "\"";
    if (freqHz > 0) {
        snprintf(b, sizeof b, ",\"f\":%.0f", freqHz); j += b;
        const std::string band = bandNameFor(freqHz);
        if (!band.empty()) j += ",\"b\":\"" + band + "\"";
    }
    if (!to.empty()) j += ",\"to\":\"" + to + "\"";
    if (hasPos) { snprintf(b, sizeof b, ",\"tlat\":%.3f,\"tlon\":%.3f", tlat, tlon); j += b; }
    return j + "}";
}

// hamCallValid() checks only the longest part between slashes; the other parts (prefix / suffix) come off the air and are free text, and `to` is put
// into the extra JSON as it is. Only letters, digits and '/' (at most 12 characters) go to the map.
static bool callChars(const std::string& s) {
    if (s.empty() || s.size() > 12) return false;
    for (char c : s) if (!(std::isupper((unsigned char)c) || std::isdigit((unsigned char)c) || c == '/')) return false;
    return true;
}

bool buildTrack(const Contact& c, int gridChars, double nowUtc, Track& out) {
    const std::string mode = normMode(c.mode);
    const std::string from = up(c.from), to = up(c.to);
    if (mode.empty() || !callChars(from) || !callChars(to) || !hamCallValid(from) || !hamCallValid(to) || from == to) return false;
    double lat, lon;
    if (!roundedCentre(c.fromGrid, gridChars, lat, lon)) return false;
    double tlat = 0, tlon = 0;
    const bool hasPos = roundedCentre(c.toGrid, gridChars, tlat, tlon);
    out = Track();
    out.id = from; out.label = from; out.sub = "ham " + mode;
    out.lat = lat; out.lon = lon;
    out.ageSec = std::max(0.0, nowUtc - c.tUtc);
    out.extra = extraJson(mode, c.freqHz, to, hasPos, tlat, tlon);
    return true;
}

std::string dedupeKey(const Contact& c) {
    return up(c.from) + ">" + up(c.to) + "|" + bandNameFor(c.freqHz) + "|" + normMode(c.mode);
}

bool Feed::offer(const Switches& sw, const Contact& c, double nowUtc) {
    if (!allowed(sw)) return false;
    Track t;
    if (!buildTrack(c, l_.gridChars, nowUtc, t)) return false;
    const std::string k = dedupeKey(c);
    for (auto i = seen_.begin(); i != seen_.end();) i = (nowUtc - i->second > l_.dedupeSec) ? seen_.erase(i) : std::next(i);
    auto it = seen_.find(k);
    if (it != seen_.end()) return false;
    seen_[k] = nowUtc;
    q_.push_back({t, c.tUtc});
    while (q_.size() > l_.maxQueued) q_.pop_front();
    return true;
}

size_t Feed::flush(const Switches& sw, Sink& sink, double nowUtc) {
    if (!allowed(sw)) { clear(); return 0; }   // switched off: nothing is kept or sent
    while (!q_.empty() && nowUtc - q_.front().tUtc > l_.maxQueuedAgeSec) q_.pop_front();
    const double cap = std::max(1.0, l_.maxPerMinute);
    if (tokens_ < 0) { tokens_ = cap; lastTok_ = nowUtc; }
    tokens_ = std::min(cap, tokens_ + std::max(0.0, nowUtc - lastTok_) * cap / 60.0);
    lastTok_ = nowUtc;
    const size_t n = std::min({q_.size(), l_.maxPerPost, (size_t)std::floor(tokens_)});
    if (!n) return 0;
    std::vector<Track> out;
    for (size_t i = 0; i < n; i++) { Track t = q_[i].t; t.ageSec = std::max(0.0, nowUtc - q_[i].tUtc); out.push_back(std::move(t)); }
    if (!sink.post(out)) return 0;   // offline: stays queued, no tokens spent
    q_.erase(q_.begin(), q_.begin() + (long)n);
    tokens_ -= (double)n;
    return n;
}

}  // namespace dect2::ham::mapfeed
