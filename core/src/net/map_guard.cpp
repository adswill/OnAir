// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The map guard: see map_guard.h.
#include "dect2/net/map_guard.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

namespace dect2::mapguard {

namespace {
std::mutex gMu;
int gActive = 0;
double gLastEnd = -1e18;
std::map<std::string, double> gSent;   // normalised identity -> last time it was transmitted

double wall() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string baseOf(const std::string& n) { const size_t d = n.find('-'); return d == std::string::npos ? n : n.substr(0, d); }
void expire(double now) {
    for (auto it = gSent.begin(); it != gSent.end();) it = now - it->second > kIdMemorySec ? gSent.erase(it) : std::next(it);
}
}  // namespace

std::string normalizeId(const std::string& id) {
    std::string o;
    for (unsigned char c : id) if (std::isalnum(c) || c == '-') o += (char)std::toupper(c);
    while (!o.empty() && o.front() == '-') o.erase(o.begin());
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
}

std::vector<std::string> identitiesOf(const std::string& callsign, const std::string& dest, const std::string& name, unsigned address, const std::string& text) {
    std::vector<std::string> v;
    for (const std::string* s : {&callsign, &dest, &name}) if (!s->empty()) v.push_back(*s);
    if (address) {
        v.push_back(std::to_string(address));
        char b[16];
        snprintf(b, sizeof b, "%06X", address & 0xFFFFFF);
        v.push_back(b);
    }
    std::string w;
    for (size_t i = 0; i <= text.size(); i++) {
        const unsigned char c = i < text.size() ? (unsigned char)text[i] : ' ';
        if (std::isalnum(c) || c == '-') w += (char)c;
        else { if (w.size() >= 3) v.push_back(w); w.clear(); }
    }
    return v;
}

void txBegin(double) { std::lock_guard<std::mutex> l(gMu); gActive++; }
void txEnd(double now) {
    std::lock_guard<std::mutex> l(gMu);
    if (gActive > 0) gActive--;
    gLastEnd = std::max(gLastEnd, now);
}

void noteSent(const std::string& id, double now) {
    const std::string n = normalizeId(id);
    if (n.size() < 3) return;   // too short to mean anything
    std::lock_guard<std::mutex> l(gMu);
    gSent[n] = now;
    const std::string b = baseOf(n);
    if (b != n && b.size() >= 3) gSent[b] = now;
    expire(now);
}
void noteSentAll(const std::vector<std::string>& ids, double now) { for (const auto& i : ids) noteSent(i, now); }

bool transmitting() { std::lock_guard<std::mutex> l(gMu); return gActive > 0; }
bool holding(double now) { std::lock_guard<std::mutex> l(gMu); return gActive > 0 || now - gLastEnd < kHoldAfterSec; }
double holdLeftSec(double now) {
    std::lock_guard<std::mutex> l(gMu);
    if (gActive > 0) return 1e9;
    const double left = kHoldAfterSec - (now - gLastEnd);
    return left > 0 ? left : 0;
}

bool idBlocked(const std::string& id, double now) {
    const std::string n = normalizeId(id);
    if (n.empty()) return false;
    std::lock_guard<std::mutex> l(gMu);
    if (gSent.empty()) return false;
    auto hit = [&](const std::string& k) { const auto it = gSent.find(k); return it != gSent.end() && now - it->second <= kIdMemorySec; };
    if (hit(n)) return true;
    const std::string b = baseOf(n);
    return b != n && b.size() >= 3 && hit(b);
}

void reset() { std::lock_guard<std::mutex> l(gMu); gActive = 0; gLastEnd = -1e18; gSent.clear(); }

void txBegin() { txBegin(wall()); }
void txEnd() { txEnd(wall()); }
void noteSent(const std::string& id) { noteSent(id, wall()); }
bool holdingNow() { return holding(wall()); }
bool idBlockedNow(const std::string& id) { return idBlocked(id, wall()); }

}  // namespace dect2::mapguard
