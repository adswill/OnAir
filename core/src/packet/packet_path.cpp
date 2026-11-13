// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / packet path graph (see packet_path.h).
#include "dect2/packet/packet_path.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace dect2 {

namespace {
constexpr size_t kMaxNodes = 400, kMaxEdges = 1500, kMaxVia = 200, kMaxAgain = 3000;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double km(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double p1 = la1 * r, p2 = la2 * r, dp = p2 - p1, dl = (lo2 - lo1) * r;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 6371.0 * 2 * std::asin(std::min(1.0, std::sqrt(h)));
}

// monotone chain on (lon, lat) as planar x, y: fine for the few hundred km a digipeater covers
std::vector<std::pair<float, float>> hullOf(std::vector<std::pair<double, double>> p) {   // p: (lat, lon)
    std::sort(p.begin(), p.end(), [](auto& a, auto& b) { return a.second < b.second || (a.second == b.second && a.first < b.first); });
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3) { std::vector<std::pair<float, float>> o; for (auto& q : p) o.push_back({(float)q.first, (float)q.second}); return o; }
    auto cross = [](const std::pair<double, double>& o, const std::pair<double, double>& a, const std::pair<double, double>& b) {
        return (a.second - o.second) * (b.first - o.first) - (a.first - o.first) * (b.second - o.second);
    };
    std::vector<std::pair<double, double>> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); i++) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--; h[k++] = p[i]; }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) { while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) k--; h[k++] = p[i - 1]; }
    h.resize(k - 1);
    std::vector<std::pair<float, float>> o;
    const size_t step = std::max<size_t>(1, (h.size() + 23) / 24);
    for (size_t i = 0; i < h.size(); i += step) o.push_back({(float)h[i].first, (float)h[i].second});
    return o;
}
} // namespace

bool PacketPathGraph::isAlias(const std::string& c) {
    static const char* const names[] = {"WIDE", "RELAY", "TRACE", "TCPIP", "TCPXX", "NOGATE", "RFONLY", "ECHO", "GATE", "HFPAD", "CITY", "SAR", "SGATE", "NO"};
    for (const char* n : names) {
        const size_t l = std::char_traits<char>::length(n);
        if (c.compare(0, l, n) == 0 && c.size() >= l) {
            bool rest = true;
            for (size_t i = l; i < c.size(); i++) rest = rest && std::isdigit((unsigned char)c[i]);
            if (rest) return true;
        }
    }
    return false;
}

void PacketPathGraph::observe(const ax25::Frame& f, double t, bool hasPos, double lat, double lon, bool copy) {
    const std::string src = f.from.str();
    std::string prev = src;
    std::string lastReal;
    bool tcpip = false;
    for (const ax25::Address& a : f.path) if (a.call.compare(0, 5, "TCPIP") == 0 || a.call.compare(0, 5, "TCPXX") == 0) tcpip = true;
    // a call twice in the path is a loop (a digipeater that repeats what it repeated)
    std::vector<std::string> seen;
    std::vector<std::string> looped;
    for (const ax25::Address& a : f.path) {
        if (isAlias(a.call)) continue;
        const std::string n = a.str();
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) looped.push_back(n); else seen.push_back(n);
    }
    bool any = false;
    for (const ax25::Address& a : f.path) {
        if (!a.h) continue;
        any = true;
        if (isAlias(a.call)) continue;                  // an alias used up as a hop: the digipeater behind it is not named
        const std::string n = a.str();
        if (nodes_.size() >= kMaxNodes && !nodes_.count(n)) continue;
        Node& d = nodes_[n];
        if (!d.hops && !d.heard) d.first = t;
        d.hops++;
        d.last = std::max(d.last, t);
        if (edges_.size() < kMaxEdges || edges_.count({prev, n})) { Edge& e = edges_[{prev, n}]; e.count++; e.last = t; }
        prev = n;
        lastReal = n;
    }
    if (!any) direct_++;
    if (lastReal.empty()) return;
    Node& d = nodes_[lastReal];
    d.heard++;
    d.last = t;
    if (copy) d.copies++;
    if (tcpip) d.igate = true;
    for (const std::string& n : looped) { auto it = nodes_.find(n); if (it != nodes_.end()) it->second.loops++; }
    // the same packet again and again through the same digipeater
    {
        const std::string key = src + ">" + f.to.str() + ":" + f.info + "@" + lastReal;
        if (again_.size() > kMaxAgain) for (auto it = again_.begin(); it != again_.end();) it = t - it->second.first > 600 ? again_.erase(it) : std::next(it);
        Seen& s = again_[key];
        if (s.n == 0 || t - s.first > 300) { s.n = 0; s.first = t; }
        s.n++;
        if (s.n >= 3) d.loops++;
    }
    d.frames++;
    if (d.via.size() < kMaxVia || d.via.count(src)) {
        auto it = d.via.find(src);
        if (it == d.via.end()) it = d.via.emplace(src, std::make_pair(kNaN, kNaN)).first;
        if (hasPos) it->second = {lat, lon};
    }
}

void PacketPathGraph::snapshot(double now, const std::function<bool(const std::string&, double&, double&)>& posOf, std::vector<PacketDigi>& digis, std::vector<PacketPathEdge>& edges) const {
    digis.clear(); edges.clear();
    for (const auto& kv : nodes_) {
        const Node& n = kv.second;
        PacketDigi d;
        d.call = kv.first; d.heard = n.heard; d.hops = n.hops; d.copies = n.copies; d.loops = n.loops;
        d.firstSec = n.first; d.lastSec = n.last; d.igate = n.igate;
        d.stations = (uint32_t)n.via.size();
        if (n.heard >= 4) {
            const double interval = (n.last - n.first) / (double)(n.heard - 1);
            d.silent = now - n.last > std::max(1800.0, 6.0 * interval);
        }
        d.looping = n.loops >= 3 && n.loops * 10 > n.heard;
        if (posOf) { double la, lo; if (posOf(kv.first, la, lo)) { d.hasPos = true; d.lat = la; d.lon = lo; } }
        std::vector<std::pair<double, double>> pts;
        for (const auto& s : n.via) if (!std::isnan(s.second.first)) pts.push_back(s.second);
        if (!pts.empty()) {
            d.hull = hullOf(pts);
            double cla = 0, clo = 0;
            if (d.hasPos) { cla = d.lat; clo = d.lon; }
            else { for (auto& p : pts) { cla += p.first; clo += p.second; } cla /= (double)pts.size(); clo /= (double)pts.size(); }
            for (auto& p : pts) d.rangeKm = std::max(d.rangeKm, km(cla, clo, p.first, p.second));
        }
        digis.push_back(std::move(d));
    }
    std::sort(digis.begin(), digis.end(), [](const PacketDigi& a, const PacketDigi& b) { return a.heard != b.heard ? a.heard > b.heard : a.call < b.call; });
    for (const auto& kv : edges_) { PacketPathEdge e; e.from = kv.first.first; e.to = kv.first.second; e.count = kv.second.count; e.lastSec = kv.second.last; edges.push_back(std::move(e)); }
    std::sort(edges.begin(), edges.end(), [](const PacketPathEdge& a, const PacketPathEdge& b) { return a.count > b.count; });
}

void PacketPathGraph::shiftTimes(double dt) {
    for (auto& kv : nodes_) { kv.second.first += dt; kv.second.last += dt; }
    for (auto& kv : edges_) kv.second.last += dt;
    again_.clear();
}

void PacketPathGraph::clear() { nodes_.clear(); edges_.clear(); again_.clear(); direct_ = 0; }

} // namespace dect2
