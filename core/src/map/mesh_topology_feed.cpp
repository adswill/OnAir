// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol front ends of the shared mesh topology (mesh_topology_feed.h).
#include "dect2/map/mesh_topology_feed.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace {

constexpr size_t kPacketKeys = 3000;
const char* const kMeshtastic = "Meshtastic";
const char* const kMeshCore = "MeshCore";

int lastByteOf(const std::string& id) {   // "!1a2b3c4d" -> 0x4d, -1 when it is not a node number
    if (id.size() != 9 || id[0] != '!') return -1;
    char* end = nullptr;
    const long v = std::strtol(id.c_str() + 7, &end, 16);
    return *end ? -1 : (int)v;
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

void observer(MeshTopology& t, const std::string& group, double now) {
    TopoNode& o = t.node(kTopoObserver, group, now);
    o.observer = true; o.label = "this receiver";
}

} // namespace

std::string topoGroupName(const std::string& g) {
    if (g.size() == 4 && g.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos) return "PAN " + g;
    if (g.size() == 8 && g.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos) return "home " + g;
    return g;
}

void topoNoteMeshNode(MeshTopology& t, const std::string& id, const std::string& name, const std::string& role, bool hasPos, double lat, double lon, double now, int protocol) {
    if (id.empty()) return;
    TopoNode& n = t.node(id, protocol == 2 ? kMeshCore : kMeshtastic, now);
    n.placeholder = false;
    if (!name.empty()) n.label = name;
    if (!role.empty()) n.role = role;
    if (hasPos) { n.hasPos = true; n.lat = lat; n.lon = lon; }
}

void topoFeedLora(MeshTopology& t, LoraTopoState& st, const MeshPacket& p, double now) {
    if (!p.crcOk || !p.parsed || p.from.empty()) return;
    if (p.protocol == 1) {
        const std::string group = kMeshtastic;
        observer(t, group, now);
        t.node(p.from, group, now);
        const int hops = (p.hopStart >= 0 && p.hopLimit >= 0 && p.hopStart >= p.hopLimit) ? p.hopStart - p.hopLimit : -1;
        const std::string pkey = p.packetId ? [&] { char b[40]; snprintf(b, sizeof b, "%08x/", p.packetId); return std::string(b) + p.from; }() : std::string();
        std::vector<LoraTopoState::Seen>* seen = nullptr;
        bool first = true;
        if (!pkey.empty()) {
            t.floodCopy(group, pkey);
            auto it = st.perPacket.find(pkey);
            first = it == st.perPacket.end();
            if (first) {
                it = st.perPacket.emplace(pkey, std::vector<LoraTopoState::Seen>()).first;
                st.order.push_back(pkey);
                if (st.order.size() > kPacketKeys) { st.perPacket.erase(st.order.front()); st.order.pop_front(); }
            }
            seen = &it->second;
        }
        if (first) t.originated(p.from, now);
        if (p.viaMqtt) return;   // a gateway put it on the air: the relay byte says nothing about the radio path
        const bool direct = hops == 0 || (hops < 0 && p.relayNode >= 0 && p.relayNode == lastByteOf(p.from));
        if (direct) { t.heardDirect(p.from, kTopoObserver, now, p.snrDb); return; }
        if (p.relayNode < 0) return;   // an older firmware: no relay byte
        // who sent this copy on
        std::vector<std::pair<std::string, double>> cand = t.relayCandidates(group, p.relayNode, p.from);
        bool placeholder = false;
        if (cand.empty()) {
            char b[16]; snprintf(b, sizeof b, "?%02x", p.relayNode);
            TopoNode& ph = t.node(std::string(group) + "/" + b, group, now);
            ph.placeholder = true; ph.label = std::string("relay ") + b;
            cand.emplace_back(ph.id, 1.0);
            placeholder = true;
        }
        const bool single = cand.size() == 1;
        for (const auto& c : cand) {
            t.link(c.first, kTopoObserver, TopoEvidence::Heard, now, true, p.snrDb, c.second);
            if (single) { TopoNode& n = t.node(c.first, group, now); n.heard++; n.snrSum += p.snrDb; n.snrN++; }
            t.relayedBy(c.first, now, c.second);
            if (hops == 1) t.link(p.from, c.first, TopoEvidence::Link, now, true, p.snrDb, c.second);
        }
        (void)placeholder;
        if (single && hops >= 2 && seen) {
            // a deeper copy that followed exactly one shallower relay of the same packet: weak guess that the shallower relay fed it
            std::string upstream; int n = 0;
            for (const auto& s : *seen) if (s.depth == hops - 1 && s.relay != cand[0].first) { if (upstream != s.relay) { upstream = s.relay; n++; } }
            if (n == 1) t.link(upstream, cand[0].first, TopoEvidence::Inferred, now);
        }
        if (single && seen && seen->size() < 16) seen->push_back({hops, cand[0].first});
    } else if (p.protocol == 2) {
        const std::string group = kMeshCore;
        observer(t, group, now);
        t.node(p.from, group, now);
        t.originated(p.from, now);
        // the path: repeaters in the order the flood went through them, as hashes (hex) of public keys
        std::vector<std::string> hashes;
        size_t a = 0;
        while (a <= p.path.size()) {
            size_t b = p.path.find(',', a);
            if (b == std::string::npos) b = p.path.size();
            if (b > a) hashes.push_back(lower(p.path.substr(a, b - a)));
            a = b + 1;
        }
        if (hashes.empty()) { t.heardDirect(p.from, kTopoObserver, now, p.snrDb); return; }
        std::vector<std::string> ids;
        for (const std::string& h : hashes) {
            std::string found; int hits = 0;
            for (const auto& kv : t.nodes()) {
                const TopoNode& n = kv.second;
                if (n.group != group || n.observer || n.placeholder || n.id == p.from) continue;
                if (lower(n.id).compare(0, h.size(), h) == 0) { found = n.id; hits++; }
            }
            if (hits != 1) {   // unknown or ambiguous: a placeholder named by the hash (it merges with a real node later only by the same hash)
                found = group + "/?" + h;
                TopoNode& ph = t.node(found, group, now);
                ph.placeholder = true; ph.label = "repeater " + h;
            }
            ids.push_back(found);
        }
        std::string prev = p.from;
        for (size_t i = 0; i < ids.size(); i++) {
            t.link(prev, ids[i], i == 0 ? TopoEvidence::Link : TopoEvidence::Route, now);
            t.relayedBy(ids[i], now);
            prev = ids[i];
        }
        t.link(prev, kTopoObserver, TopoEvidence::Heard, now, true, p.snrDb);
        TopoNode& last = t.node(prev, group, now);
        last.heard++; last.snrSum += p.snrDb; last.snrN++;
    }
}

void topoFeedZwave(MeshTopology& t, const zwave::Frame& f, float snrDb, double now) {
    const std::string home = zwave::homeText(f.homeId);
    auto nid = [&](int n) { return home + "/" + std::to_string(n); };
    auto touch = [&](int n) -> TopoNode& {
        const bool isNew = !t.find(nid(n));
        TopoNode& nd = t.node(nid(n), home, now);
        if (isNew) { nd.label = "node " + std::to_string(n); if (n == 1) nd.role = "controller"; }
        return nd;
    };
    observer(t, home, now);
    if (f.src < 1 || f.src > 232) return;
    touch(f.src);
    zwave::Route r;
    if (f.routed && zwave::parseRoute(f.payload, r)) {
        const std::vector<int> path = zwave::routePath(f, r);
        if (path.size() >= 2 && f.dst >= 1 && f.dst <= 232) {
            for (int n : path) if (n >= 1 && n <= 232) touch(n);
            for (int x : r.repeaters) { TopoNode& nd = touch(x); if (nd.role.empty() || nd.role == "node") nd.role = "router"; }
            for (size_t i = 0; i + 1 < path.size(); i++) t.link(nid(path[i]), nid(path[i + 1]), TopoEvidence::Route, now);
            // this copy is on the air from path[hop]: that hop is proven
            const size_t h = (size_t)r.hop;
            if (h + 1 < path.size()) {
                t.link(nid(path[h]), nid(path[h + 1]), TopoEvidence::Link, now, true, snrDb);
                t.heardDirect(nid(path[h]), kTopoObserver, now, snrDb);
                if (h == 0) t.originated(nid(f.src), now); else t.relayedBy(nid(path[h]), now);
            }
        }
        return;
    }
    if (f.routed) return;   // a route header that does not parse: nothing to say about the path
    t.heardDirect(nid(f.src), kTopoObserver, now, snrDb);
    if (f.headerType == zwave::kHdrMulticast || f.dst < 1 || f.dst > 232) { t.originated(nid(f.src), now); return; }
    touch(f.dst);
    // an ack proves the link in both directions; a plain frame to a node only that the sender expects it to be in range
    t.link(nid(f.src), nid(f.dst), f.headerType == zwave::kHdrAck ? TopoEvidence::Link : TopoEvidence::Route, now, true, snrDb);
    if (f.headerType != zwave::kHdrAck) t.originated(nid(f.src), now);
}

void topoFeedIeee(MeshTopology& t, const zigbee::Frame& f, const thread::Frame* th, uint16_t pan, float snrDb, double now) {
    if (!f.ok || (f.type != zigbee::Data && f.type != zigbee::Command && f.type != zigbee::Beacon)) return;
    char gb[8]; snprintf(gb, sizeof gb, "%04X", pan);
    const std::string group = gb;
    observer(t, group, now);
    auto shortId = [&](uint16_t a) { return group + "/" + zigbee::shortText(a); };
    auto extId = [&](uint64_t e) { return group + "/" + zigbee::addrText(e); };
    auto touch = [&](const std::string& id) -> TopoNode& {   // a short address is "PAN/0xXXXX", an extended one "PAN/00:12:..."
        const bool isNew = !t.find(id);
        TopoNode& n = t.node(id, group, now);
        if (isNew) {
            const size_t sl = id.find('/'), x = id.find("0x");
            n.label = id.substr(sl + 1);
            if (x != std::string::npos && std::strtol(id.c_str() + x + 2, nullptr, 16) == 0) n.role = "coordinator";
        }
        return n;
    };
    std::string tx, rx;
    if (f.srcMode == 2) { tx = shortId(f.srcShort); touch(tx); }
    else if (f.srcMode == 3) { tx = extId(f.srcExt); touch(tx); }
    if (f.destMode == 2 && f.destShort < 0xFFF8) { rx = shortId(f.destShort); touch(rx); }
    else if (f.destMode == 3) { rx = extId(f.destExt); touch(rx); }
    if (tx.empty()) return;
    t.heardDirect(tx, kTopoObserver, now, snrDb);
    if (f.type == zigbee::Beacon) {
        TopoNode& n = t.node(tx, group, now);
        if (n.role.empty() || n.role == "device") n.role = (f.panCoordinator || (f.srcMode == 2 && f.srcShort == 0)) ? "coordinator" : "router";
        return;
    }
    if (!rx.empty()) t.link(tx, rx, TopoEvidence::Link, now, true, snrDb);
    // who started it: the NWK originator (Zigbee) or the mesh header's originator (Thread); a different transmitter is a relay
    std::string origin;
    if (f.hasNwk) origin = shortId(f.nwkSrc);
    else if (th && th->mesh && !th->meshOrig.empty()) origin = th->meshOrig.size() == 6 ? shortId((uint16_t)std::strtol(th->meshOrig.c_str() + 2, nullptr, 16)) : group + "/" + th->meshOrig;
    if (!origin.empty()) {
        if (origin != tx) {
            touch(origin);
            t.relayedBy(tx, now);
            TopoNode& n = t.node(tx, group, now);
            if (n.role.empty() || n.role == "device") n.role = "router";
        } else t.originated(tx, now);
    }
    if (f.hasNwk) {
        if (f.nwkSrc == 0) t.setRole(shortId(0), "coordinator");
        if (f.nwkDest >= 0xFFF8) { char kb[24]; snprintf(kb, sizeof kb, "%04X/%u", f.nwkSrc, (unsigned)f.nwkSeq); t.floodCopy(group, kb); }
        if (f.nwkSourceRoute && !f.nwkRelays.empty()) {
            std::vector<std::string> path{shortId(f.nwkSrc)};
            for (size_t i = f.nwkRelays.size(); i-- > 0;) path.push_back(shortId(f.nwkRelays[i]));
            path.push_back(shortId(f.nwkDest));
            for (const std::string& id : path) touch(id);
            for (size_t i = 1; i + 1 < path.size(); i++) { TopoNode& n = t.node(path[i], group, now); if (n.role.empty() || n.role == "device") n.role = "router"; }
            for (size_t i = 0; i + 1 < path.size(); i++) t.link(path[i], path[i + 1], TopoEvidence::Route, now);
        }
    } else if (th && th->mesh && !th->meshOrig.empty() && !th->meshFinal.empty() && th->meshOrig.size() == 6 && th->meshFinal.size() == 6) {
        // the mesh header names the two ends: not a link, but the nodes exist
        touch(shortId((uint16_t)std::strtol(th->meshFinal.c_str() + 2, nullptr, 16)));
    }
}

} // namespace dect2
