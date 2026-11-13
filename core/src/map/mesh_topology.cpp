// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passive mesh topology model shared by the Meshtastic, Zigbee / Thread and Z-Wave monitors (mesh_topology.h).
#include "dect2/map/mesh_topology.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <set>

namespace dect2 {
namespace {

constexpr size_t kFloodKeys = 4000;       // distinct packet keys remembered per group
constexpr double kUnheardWeight = 0.15;   // a relay candidate the receiver never heard directly is much less likely to be in range of it

std::pair<std::string, std::string> key(const std::string& x, const std::string& y) { return x < y ? std::make_pair(x, y) : std::make_pair(y, x); }

} // namespace

void MeshTopology::clear() { nodes_.clear(); edges_.clear(); flood_.clear(); }

TopoNode& MeshTopology::node(const std::string& id, const std::string& group, double t) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        TopoNode n;
        n.id = id; n.label = id; n.group = group; n.first = n.last = t;
        it = nodes_.emplace(id, std::move(n)).first;
        if (nodes_.size() > maxNodes_) { trim(); it = nodes_.find(id); if (it == nodes_.end()) { TopoNode m; m.id = id; m.label = id; m.group = group; it = nodes_.emplace(id, std::move(m)).first; } }
    }
    TopoNode& n = it->second;
    if (n.group.empty() && !group.empty()) n.group = group;
    if (t > n.last) n.last = t;
    return n;
}

void MeshTopology::setLabel(const std::string& id, const std::string& label) { auto it = nodes_.find(id); if (it != nodes_.end() && !label.empty()) it->second.label = label; }
void MeshTopology::setRole(const std::string& id, const std::string& role) { auto it = nodes_.find(id); if (it != nodes_.end() && !role.empty()) it->second.role = role; }
void MeshTopology::setPosition(const std::string& id, double lat, double lon) { auto it = nodes_.find(id); if (it != nodes_.end()) { it->second.hasPos = true; it->second.lat = lat; it->second.lon = lon; } }
void MeshTopology::markObserver(const std::string& id) { auto it = nodes_.find(id); if (it != nodes_.end()) it->second.observer = true; }
const TopoNode* MeshTopology::find(const std::string& id) const { auto it = nodes_.find(id); return it == nodes_.end() ? nullptr : &it->second; }

void MeshTopology::link(const std::string& from, const std::string& to, TopoEvidence ev, double t, bool hasSnr, float snr, double weight) {
    if (from == to || from.empty() || to.empty() || weight <= 0) return;
    const std::string grp = [&] { auto i = nodes_.find(from); if (i != nodes_.end() && !i->second.group.empty()) return i->second.group; i = nodes_.find(to); return i != nodes_.end() ? i->second.group : std::string(); }();
    node(from, grp, t); node(to, grp, t);
    auto k = key(from, to);
    auto it = edges_.find(k);
    if (it == edges_.end()) {
        TopoEdge e; e.a = k.first; e.b = k.second; e.first = e.last = t;
        it = edges_.emplace(k, std::move(e)).first;
        if (edges_.size() > maxEdges_) { trim(); it = edges_.find(k); if (it == edges_.end()) { TopoEdge e2; e2.a = k.first; e2.b = k.second; e2.first = e2.last = t; it = edges_.emplace(k, std::move(e2)).first; } }
    }
    TopoEdge& e = it->second;
    switch (ev) {
    case TopoEvidence::Heard: e.heard += weight; break;
    case TopoEvidence::Link: e.link += weight; break;
    case TopoEvidence::Route: e.route += weight; break;
    case TopoEvidence::Inferred: e.inferred += weight; break;
    }
    if (weight < 1) e.ambiguous = true;
    if (hasSnr) { e.snrSum += snr * weight; e.snrN++; e.lastSnr = snr; }
    if (t > e.last) e.last = t;
}

void MeshTopology::heardDirect(const std::string& id, const std::string& observer, double t, float snr) {
    TopoNode& n = node(id, "", t);
    n.heard++; n.snrSum += snr; n.snrN++;
    TopoNode& o = node(observer, n.group, t);
    o.observer = true;
    link(id, observer, TopoEvidence::Heard, t, true, snr);
}

void MeshTopology::floodCopy(const std::string& group, const std::string& packetKey) {
    Flood& f = flood_[group];
    auto it = f.seen.find(packetKey);
    f.copies++;
    if (it == f.seen.end()) {
        f.packets++;
        f.seen[packetKey] = 1;
        f.order.push_back(packetKey);
        if (f.order.size() > kFloodKeys) { f.seen.erase(f.order.front()); f.order.pop_front(); }
    } else if (it->second < 65535) {
        it->second++;
    }
}

std::vector<std::pair<std::string, double>> MeshTopology::relayCandidates(const std::string& group, int byte, const std::string& exclude) const {
    std::vector<std::pair<std::string, double>> out;
    if (byte < 0 || byte > 255) return out;
    double sum = 0;
    for (const auto& kv : nodes_) {
        const TopoNode& n = kv.second;
        if (n.observer || n.placeholder || n.group != group || n.id == exclude || n.id.size() != 9 || n.id[0] != '!') continue;
        char* end = nullptr;
        const long last = std::strtol(n.id.c_str() + 7, &end, 16);
        if (*end || last != byte) continue;
        double w = n.heard > 0 ? 1.0 : kUnheardWeight;
        w *= 1.0 + std::sqrt(n.relayed);                     // a node that has been relaying is likely to relay again
        if (!exclude.empty()) {                              // the origin and this node are known to be in range of each other
            auto e = edges_.find(exclude < n.id ? std::make_pair(exclude, n.id) : std::make_pair(n.id, exclude));
            if (e != edges_.end() && e->second.link > 0) w *= 1.0 + std::min(e->second.link, 3.0);
        }
        out.emplace_back(n.id, w);
        sum += w;
    }
    if (sum > 0) for (auto& c : out) c.second /= sum;
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
    return out;
}

void MeshTopology::trim() {
    while (nodes_.size() > maxNodes_) {
        auto worst = nodes_.end();
        for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
            if (it->second.observer) continue;
            if (worst == nodes_.end() || it->second.last < worst->second.last) worst = it;
        }
        if (worst == nodes_.end()) break;
        const std::string id = worst->first;
        nodes_.erase(worst);
        for (auto it = edges_.begin(); it != edges_.end();) { if (it->first.first == id || it->first.second == id) it = edges_.erase(it); else ++it; }
    }
    while (edges_.size() > maxEdges_) {
        auto worst = edges_.begin();
        for (auto it = edges_.begin(); it != edges_.end(); ++it) if (it->second.last < worst->second.last) worst = it;
        edges_.erase(worst);
    }
}

TopoReport MeshTopology::analyse(const std::string& group, const std::string& rootIn, bool withInferred) const {
    TopoReport r;
    r.group = group;
    std::vector<const TopoNode*> ns;
    std::unordered_map<std::string, int> idx;
    for (const auto& kv : nodes_) if (kv.second.group == group && !kv.second.observer) ns.push_back(&kv.second);
    std::sort(ns.begin(), ns.end(), [](const TopoNode* x, const TopoNode* y) { return x->id < y->id; });
    for (size_t i = 0; i < ns.size(); i++) idx[ns[i]->id] = (int)i;
    r.nodes = (int)ns.size();
    const int N = (int)ns.size();
    std::vector<std::vector<int>> adj(N);
    for (const auto& kv : edges_) {
        const TopoEdge& e = kv.second;
        const double w = e.link + e.route + (withInferred ? e.inferred : 0);
        if (w <= 0) continue;
        auto a = idx.find(e.a), b = idx.find(e.b);
        if (a == idx.end() || b == idx.end()) continue;
        adj[a->second].push_back(b->second); adj[b->second].push_back(a->second);
        r.links++;
    }
    // components among nodes that have a link
    {
        std::vector<char> seen(N, 0);
        for (int s = 0; s < N; s++) {
            if (seen[s] || adj[s].empty()) continue;
            r.components++;
            std::vector<int> st{s}; seen[s] = 1;
            while (!st.empty()) { const int u = st.back(); st.pop_back(); for (int v : adj[u]) if (!seen[v]) { seen[v] = 1; st.push_back(v); } }
        }
    }
    // the root: the named node, else a coordinator, else the best connected node
    int root = -1;
    if (!rootIn.empty()) { auto it = idx.find(rootIn); if (it != idx.end()) root = it->second; }
    if (root < 0) {
        for (int i = 0; i < N; i++) {
            if (adj[i].empty()) continue;
            if (root < 0) { root = i; continue; }
            auto isRoot = [](const std::string& r) { return r == "coordinator" || r == "controller"; };
            const bool ci = isRoot(ns[i]->role), cr = isRoot(ns[root]->role);
            if (ci != cr) { if (ci) root = i; continue; }
            if (adj[i].size() > adj[root].size()) root = i;
        }
    }
    if (root >= 0) r.root = ns[root]->id;
    auto label = [&](int i) { return ns[i]->label.empty() ? ns[i]->id : ns[i]->label; };

    // relay ranking
    double total = 0;
    for (const TopoNode* n : ns) total += n->relayed;
    for (const TopoNode* n : ns) if (n->relayed > 0) r.relays.push_back({n->id, n->relayed, total > 0 ? n->relayed / total : 0});
    std::sort(r.relays.begin(), r.relays.end(), [](const TopoRelayRank& x, const TopoRelayRank& y) { return x.relayed != y.relayed ? x.relayed > y.relayed : x.id < y.id; });
    if (r.relays.size() > 8) r.relays.resize(8);

    // single points of failure: remove one node, count what the root can no longer reach
    if (root >= 0) {
        auto reach = [&](int skip) {
            std::vector<char> seen(N, 0);
            std::vector<int> st{root}; seen[root] = 1;
            while (!st.empty()) { const int u = st.back(); st.pop_back(); for (int v : adj[u]) if (v != skip && !seen[v]) { seen[v] = 1; st.push_back(v); } }
            return seen;
        };
        const std::vector<char> base = reach(-1);
        for (int v = 0; v < N; v++) {
            if (v == root || !base[v]) continue;
            const std::vector<char> after = reach(v);
            TopoCut c; c.id = ns[v]->id;
            for (int u = 0; u < N; u++) if (u != v && base[u] && !after[u]) { c.orphans++; if (c.cutOff.size() < 12) c.cutOff.push_back(ns[u]->id); }
            if (c.orphans > 0) r.cuts.push_back(std::move(c));
        }
        std::sort(r.cuts.begin(), r.cuts.end(), [](const TopoCut& x, const TopoCut& y) { return x.orphans != y.orphans ? x.orphans > y.orphans : x.id < y.id; });
        if (r.cuts.size() > 12) r.cuts.resize(12);
    }

    auto fl = flood_.find(group);
    if (fl != flood_.end() && fl->second.packets) { r.floodPackets = fl->second.packets; r.floodCopies = fl->second.copies; r.floodFactor = (double)fl->second.copies / fl->second.packets; }

    // one line of advice
    char b[240];
    if (!r.cuts.empty()) {
        const int i = idx[r.cuts[0].id];
        snprintf(b, sizeof b, "%s is a single point of failure: %d node%s would lose the way to %s", label(i).c_str(), r.cuts[0].orphans, r.cuts[0].orphans == 1 ? "" : "s", label(root).c_str());
        r.advice = b;
    } else if (!r.relays.empty() && r.relays[0].share >= 0.5 && total >= 10) {
        const int i = idx[r.relays[0].id];
        snprintf(b, sizeof b, "%s sends on %d %% of the relayed traffic: the network leans on it", label(i).c_str(), (int)(r.relays[0].share * 100 + 0.5));
        r.advice = b;
    }
    return r;
}

TopoSnapshot MeshTopology::snapshot(bool withInferred) const {
    TopoSnapshot s;
    std::set<std::string> groups;
    for (const auto& kv : nodes_) { s.nodes.push_back(kv.second); if (!kv.second.group.empty()) groups.insert(kv.second.group); }
    std::sort(s.nodes.begin(), s.nodes.end(), [](const TopoNode& x, const TopoNode& y) { return x.id < y.id; });
    for (const auto& kv : edges_) s.edges.push_back(kv.second);
    for (const std::string& g : groups) s.reports.push_back(analyse(g, std::string(), withInferred));
    return s;
}

} // namespace dect2
