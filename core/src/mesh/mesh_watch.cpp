// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh node watch: see mesh_watch.h.
#include "dect2/mesh/mesh_watch.h"
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
double km(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}
std::string nameOf(const MeshNode& n) { return n.longName.empty() ? (n.shortName.empty() ? n.id : n.shortName) : n.longName; }
}  // namespace

std::vector<automation::Event> MeshWatch::update(const std::vector<MeshNode>& nodes, const MeshWatchConfig& c, double now) {
    std::vector<automation::Event> out;
    auto ev = [&](const char* type, const MeshNode* nd, const std::string& id) {
        automation::Event e(type);
        e.at(now, "Mesh", 0);
        e.str("id", id).str("name", nd ? nameOf(*nd) : id);
        if (nd && nd->hasPosition) e.num("lat", nd->lat).num("lon", nd->lon);
        return e;
    };
    for (const MeshNode& nd : nodes) {
        if (nd.id.empty()) continue;
        // new nodes (not on the first look: those were already there)
        if (seen_.insert(nd.id).second && !first_ && c.newWithinKm > 0 && c.hasHome && nd.hasPosition) {
            const double d = km(c.homeLat, c.homeLon, nd.lat, nd.lon);
            if (d <= c.newWithinKm) { automation::Event e = ev("lora.node_new", &nd, nd.id); e.num("distance_km", d).str("hw", nd.hwModel); out.push_back(std::move(e)); }
        }
        if (!c.watched.count(nd.id)) continue;
        N& w = n_[nd.id];
        if (nd.lastHeard != w.lastHeardSig) {   // heard again
            w.lastHeardSig = nd.lastHeard;
            w.lastWall = now;
            if (w.silentSent) { automation::Event e = ev("lora.node_silent", &nd, nd.id); e.flag("back", true).num("hours", 0); out.push_back(std::move(e)); }
            w.silentSent = false;
        }
        if (c.batteryPct > 0 && nd.batteryPct >= 0 && nd.batteryPct <= 100) {
            if (nd.batteryPct < c.batteryPct && !w.batterySent) { automation::Event e = ev("lora.node_battery", &nd, nd.id); e.num("battery_pct", nd.batteryPct).num("voltage", nd.voltage); out.push_back(std::move(e)); w.batterySent = true; }
            else if (nd.batteryPct >= c.batteryPct + 5) w.batterySent = false;   // 5 % above the limit before it can warn again
        }
    }
    // silence: also for watched nodes no longer in the table (they left it, or OnAir restarted)
    if (c.silentHours > 0)
        for (auto& kv : n_) {
            if (!c.watched.count(kv.first) || kv.second.lastWall <= 0 || kv.second.silentSent) continue;
            const double h = (now - kv.second.lastWall) / 3600.0;
            if (h >= c.silentHours) {
                const MeshNode* nd = nullptr;
                for (const MeshNode& x : nodes) if (x.id == kv.first) nd = &x;
                automation::Event e = ev("lora.node_silent", nd, kv.first);
                e.flag("back", false).num("hours", std::floor(h * 10) / 10);
                out.push_back(std::move(e));
                kv.second.silentSent = true;
            }
        }
    first_ = false;
    return out;
}

std::string MeshWatch::serialize() const {
    std::ostringstream o;
    for (const auto& kv : n_) if (kv.second.lastWall > 0) o << kv.first << '\t' << (long long)kv.second.lastWall << '\t' << (kv.second.silentSent ? 1 : 0) << '\n';
    return o.str();
}

void MeshWatch::parse(const std::string& text) {
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        const size_t a = l.find('\t');
        if (a == std::string::npos || a == 0) continue;
        const size_t b = l.find('\t', a + 1);
        N& w = n_[l.substr(0, a)];
        w.lastWall = std::atof(l.c_str() + a + 1);
        w.silentSent = b != std::string::npos && l[b + 1] == '1';
    }
}

}  // namespace dect2
