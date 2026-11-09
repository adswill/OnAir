// Shared mesh topology (map/mesh_topology.h, mesh_topology_feed.h): the graph model (relay ranking, single points of failure, flood factor),
// the Meshtastic relay-byte resolver on a synthetic flooding network with last-byte collisions, MeshCore paths, the Z-Wave route header
// (build / parse / graph), Zigbee source routes and Thread mesh headers, each with frame loss, duplicates and shuffling.
#include "dect2/map/mesh_topology.h"
#include "dect2/map/mesh_topology_feed.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string key(const std::string& a, const std::string& b) { return a < b ? a + "|" + b : b + "|" + a; }

// ---------------------------------------------------------------- the model
static void testModel() {
    MeshTopology t;
    // root 0 - a - b - c, a - d (leaf), c - e (leaf): a cuts {b, c, d, e}, b cuts {c, e}, c cuts {e}
    const char* ids[] = {"r", "a", "b", "c", "d", "e"};
    for (const char* i : ids) t.node(i, "g", 1);
    t.setRole("r", "coordinator");
    t.link("r", "a", TopoEvidence::Link, 1); t.link("a", "b", TopoEvidence::Link, 1); t.link("b", "c", TopoEvidence::Route, 1);
    t.link("a", "d", TopoEvidence::Link, 1); t.link("c", "e", TopoEvidence::Link, 1);
    t.link("d", "e", TopoEvidence::Inferred, 1);   // a guess must not hide the single points
    for (int i = 0; i < 30; i++) t.relayedBy("a", 2);
    for (int i = 0; i < 10; i++) t.relayedBy("b", 2);
    t.floodCopy("g", "p1"); t.floodCopy("g", "p1"); t.floodCopy("g", "p1"); t.floodCopy("g", "p2");
    TopoReport r = t.analyse("g");
    CHECK(r.root == "r" && r.nodes == 6 && r.links == 5 && r.components == 1, "basic %s %d %d %d", r.root.c_str(), r.nodes, r.links, r.components);
    CHECK(r.cuts.size() == 3 && r.cuts[0].id == "a" && r.cuts[0].orphans == 4 && r.cuts[1].id == "b" && r.cuts[1].orphans == 2 && r.cuts[2].id == "c" && r.cuts[2].orphans == 1,
          "cuts: %zu first %s %d", r.cuts.size(), r.cuts.empty() ? "" : r.cuts[0].id.c_str(), r.cuts.empty() ? 0 : r.cuts[0].orphans);
    CHECK(!r.relays.empty() && r.relays[0].id == "a" && std::fabs(r.relays[0].share - 0.75) < 1e-9, "relay share");
    CHECK(std::fabs(r.floodFactor - 2.0) < 1e-9 && r.floodPackets == 2 && r.floodCopies == 4, "flood factor %.2f", r.floodFactor);
    CHECK(!r.advice.empty(), "advice");
    TopoReport r2 = t.analyse("g", "", true);   // with the guess d-e the leaf e has a second way: c no longer orphans e... but b still cuts c? (c-e-d-a) no
    bool cOrph = false; for (const auto& c : r2.cuts) if (c.id == "c") cOrph = true;
    CHECK(!cOrph, "inferred link opens a way round c");
    // a ring has no single point
    MeshTopology ring;
    for (int i = 0; i < 6; i++) ring.link("n" + std::to_string(i), "n" + std::to_string((i + 1) % 6), TopoEvidence::Link, 1);
    for (const auto& kv : ring.nodes()) (void)kv;
    for (int i = 0; i < 6; i++) ring.node("n" + std::to_string(i), "g", 1);
    CHECK(ring.analyse("g").cuts.empty(), "ring has no cut");
    // the observer is not part of the analysis
    MeshTopology o;
    o.heardDirect("a", "rx", 1, -5); o.heardDirect("b", "rx", 1, -5);
    o.node("a", "g", 1); o.node("b", "g", 1);
    CHECK(o.analyse("g").links == 0, "heard edges are not graph links");
    // trimming keeps the observer and bounds memory
    MeshTopology small(10, 20);
    small.node("rx", "g", 0); small.markObserver("rx");
    for (int i = 0; i < 100; i++) small.link("rx", "n" + std::to_string(i), TopoEvidence::Heard, i);
    CHECK(small.nodeCount() <= 10 && small.find("rx"), "trim %zu", small.nodeCount());
}

// ------------------------------------------------- Meshtastic on a flooding network
struct SimNet {
    int n;
    std::vector<std::string> id;
    std::vector<std::vector<int>> adj;   // radio neighbours
    std::vector<char> rxHears;           // the receiver is in range of this node
};

static std::string nodeId(unsigned hi, int lastByte) { char b[16]; snprintf(b, sizeof b, "!%06x%02x", hi & 0xFFFFFF, lastByte); return b; }

static SimNet makeNet(std::mt19937& g, int n, int distinctBytes, double range, double rxRange) {
    SimNet s; s.n = n;
    std::vector<double> x(n), y(n);
    std::uniform_real_distribution<double> u(0, 1);
    for (int i = 0; i < n; i++) { x[i] = u(g); y[i] = u(g); s.id.push_back(nodeId(0xA10000 + (unsigned)i * 7, (int)(g() % (unsigned)distinctBytes) * 3 + 1)); }
    s.adj.assign(n, {});
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (std::hypot(x[i] - x[j], y[i] - y[j]) < range) { s.adj[i].push_back(j); s.adj[j].push_back(i); }
    s.rxHears.assign(n, 0);
    for (int i = 0; i < n; i++) s.rxHears[i] = std::hypot(x[i] - 0.5, y[i] - 0.5) < rxRange;
    return s;
}

static int lastByte(const std::string& id) { return (int)strtol(id.c_str() + 7, nullptr, 16); }

struct FloodResult { double ourAcc = 0, uniformAcc = 0, linkPrec = 0, linkRecall = 0; int copies = 0; };

// loss: chance the receiver misses a copy it is in range of
static FloodResult runFlood(unsigned seed, int n, int distinctBytes, double loss, int packets) {
    std::mt19937 g(seed);
    SimNet net = makeNet(g, n, distinctBytes, 0.28, 0.32);
    MeshTopology topo; LoraTopoState st;
    for (int i = 0; i < n; i++) topoNoteMeshNode(topo, net.id[i], "", "", false, 0, 0, 0);
    std::map<std::string, double> trueRelayed;      // ground truth: copies each node sent on that the receiver heard
    std::set<std::string> trueEdges;                // RF edges
    for (int i = 0; i < n; i++) for (int j : net.adj[i]) trueEdges.insert(key(net.id[i], net.id[j]));
    // the uniform-weight baseline: every candidate with the byte gets the same share
    double baseOk = 0, baseAll = 0;
    int copies = 0;
    std::normal_distribution<double> snrN(5, 4);
    for (int k = 0; k < packets; k++) {
        const int origin = (int)(g() % (unsigned)n);
        const unsigned pid = 0x10000000u + (unsigned)k * 977;
        std::vector<int> depth(n, -1);
        std::vector<int> order{origin}; depth[origin] = 0;
        for (size_t q = 0; q < order.size(); q++) { const int u = order[q]; if (depth[u] >= 3) continue; for (int v : net.adj[u]) if (depth[v] < 0) { depth[v] = depth[u] + 1; order.push_back(v); } }
        // a node transmits once when it has the packet and (depth <= 3): the origin at depth 0, relays at depth d send with hop_limit 3 - d
        std::vector<std::string> sent;
        for (int u : order) {
            if (depth[u] > 3 || !net.rxHears[u]) continue;
            if (std::uniform_real_distribution<double>(0, 1)(g) < loss) continue;
            MeshPacket p;
            p.protocol = 1; p.crcOk = true; p.parsed = true; p.from = net.id[origin]; p.packetId = pid;
            p.hopStart = 3; p.hopLimit = 3 - depth[u]; p.relayNode = lastByte(net.id[u]); p.snrDb = (float)snrN(g);
            topoFeedLora(topo, st, p, 100.0 + k);
            copies++;
            if (depth[u] >= 1) {
                trueRelayed[net.id[u]] += 1;
                // baseline: uniform over the candidates (all nodes with that last byte, except the origin)
                int cnt = 0; bool inSet = false;
                for (int i = 0; i < n; i++) if (i != origin && lastByte(net.id[i]) == p.relayNode) { cnt++; if (i == u) inSet = true; }
                baseAll += 1; if (inSet && cnt) baseOk += 1.0 / cnt;
            }
        }
    }
    FloodResult r; r.copies = copies;
    double ok = 0, all = 0;
    for (const auto& kv : trueRelayed) {
        all += kv.second;
        const TopoNode* nd = topo.find(kv.first);
        ok += std::min(kv.second, nd ? nd->relayed : 0.0);
    }
    r.ourAcc = all > 0 ? ok / all : 1;
    r.uniformAcc = baseAll > 0 ? baseOk / baseAll : 1;
    double linkOk = 0, linkAll = 0; std::set<std::string> found;
    for (const auto& kv : topo.edges()) {
        const TopoEdge& e = kv.second;
        if (e.link <= 0) continue;
        linkAll += e.link;
        if (trueEdges.count(key(e.a, e.b))) { linkOk += e.link; found.insert(key(e.a, e.b)); }
    }
    r.linkPrec = linkAll > 0 ? linkOk / linkAll : 1;
    r.linkRecall = trueEdges.empty() ? 1 : (double)found.size() / trueEdges.size();
    return r;
}

static void testMeshtastic() {
    // simple cases
    {
        MeshTopology t; LoraTopoState st;
        topoNoteMeshNode(t, "!11111101", "A", "CLIENT", false, 0, 0, 0);
        topoNoteMeshNode(t, "!22222202", "B", "ROUTER", false, 0, 0, 0);
        topoNoteMeshNode(t, "!33333303", "C", "CLIENT", false, 0, 0, 0);
        MeshPacket p; p.protocol = 1; p.crcOk = p.parsed = true; p.from = "!11111101"; p.packetId = 5; p.hopStart = 3; p.hopLimit = 3; p.relayNode = 0x01; p.snrDb = 6;
        topoFeedLora(t, st, p, 1);   // direct from A
        p.hopLimit = 2; p.relayNode = 0x02; p.snrDb = 2;
        topoFeedLora(t, st, p, 2);   // relayed once by B: A-B is a proven link
        p.packetId = 6; p.from = "!33333303"; p.hopLimit = 1; p.relayNode = 0x02;   // C reached us after two hops, last by B
        topoFeedLora(t, st, p, 3);
        const TopoNode* b = t.find("!22222202");
        CHECK(b && b->relayed == 2, "B relayed twice");
        auto it = t.edges().find({"!11111101", "!22222202"});
        CHECK(it != t.edges().end() && it->second.link == 1 && !it->second.ambiguous, "A-B link");
        CHECK(t.find("!11111101")->heard == 1, "A heard directly");
        // viaMqtt packets are ignored for the path
        p.viaMqtt = true; p.packetId = 7; p.relayNode = 0x02; p.hopLimit = 2;
        const double before = t.find("!22222202")->relayed;
        topoFeedLora(t, st, p, 4);
        CHECK(t.find("!22222202")->relayed == before, "mqtt packets do not count as relays");
        // an unknown relay byte makes a placeholder, shown as ambiguous nothing
        p.viaMqtt = false; p.packetId = 8; p.relayNode = 0x7e; p.hopLimit = 2;
        topoFeedLora(t, st, p, 5);
        bool ph = false; for (const auto& kv : t.nodes()) if (kv.second.placeholder) ph = true;
        CHECK(ph, "placeholder for an unknown relay byte");
    }
    // collisions: two nodes share the last byte, only one is heard directly
    {
        MeshTopology t; LoraTopoState st;
        topoNoteMeshNode(t, "!aaaaaa10", "", "", false, 0, 0, 0);
        topoNoteMeshNode(t, "!bbbbbb10", "", "", false, 0, 0, 0);   // same last byte
        topoNoteMeshNode(t, "!cccccc20", "", "", false, 0, 0, 0);
        MeshPacket p; p.protocol = 1; p.crcOk = p.parsed = true; p.from = "!aaaaaa10"; p.packetId = 1; p.hopStart = 3; p.hopLimit = 3; p.relayNode = 0x10; p.snrDb = 4;
        topoFeedLora(t, st, p, 1);   // aaaaaa10 heard directly
        p.from = "!cccccc20"; p.packetId = 2; p.hopLimit = 2;
        topoFeedLora(t, st, p, 2);   // relayed by 0x10: candidates aaaaaa10 (heard) and bbbbbb10 (never heard)
        const double a = t.find("!aaaaaa10")->relayed, b = t.find("!bbbbbb10")->relayed;
        CHECK(a > 0.8 && b > 0 && std::fabs(a + b - 1) < 1e-9, "weights %.3f %.3f", a, b);
        auto it = t.edges().find({"!aaaaaa10", "!cccccc20"});
        CHECK(it != t.edges().end() && it->second.ambiguous, "ambiguity is flagged");
    }
    // measured: relay attribution with the heard-directly prior against uniform guessing, over seeds and loss
    printf("meshtastic relay attribution (30 nodes, last bytes drawn from N values, 600 packets, averaged over 8 networks)\n");
    printf("  bytes  loss  copies  ours  uniform  linkPrec  linkRecall\n");
    for (int bytes : {30, 16, 10}) for (double loss : {0.0, 0.3}) {
        double a = 0, u = 0, lp = 0, lr = 0; int c = 0; const int S = 8;
        for (int s = 0; s < S; s++) { FloodResult r = runFlood(1000u + (unsigned)s * 31 + (unsigned)bytes, 30, bytes, loss, 600); a += r.ourAcc; u += r.uniformAcc; lp += r.linkPrec; lr += r.linkRecall; c += r.copies; }
        a /= S; u /= S; lp /= S; lr /= S; c /= S;
        printf("  %5d  %4.1f  %6d  %.3f  %.3f    %.3f     %.3f\n", bytes, loss, c, a, u, lp, lr);
        CHECK(a >= u - 0.005, "prior never worse than uniform (%d bytes, loss %.1f: %.3f vs %.3f)", bytes, loss, a, u);
        CHECK(a > u + 0.2, "the prior must beat uniform guessing with collisions (%.3f vs %.3f)", a, u);
        CHECK(a > (bytes >= 16 ? 0.85 : 0.8), "attribution %.3f", a);
        CHECK(lp > 0.75, "link precision %.3f", lp);
    }
}

static void testMeshCore() {
    MeshTopology t; LoraTopoState st;
    MeshPacket p; p.protocol = 2; p.crcOk = p.parsed = true; p.snrDb = 3;
    p.from = "ab01cd02"; p.path = "";
    topoFeedLora(t, st, p, 1);
    CHECK(t.find("ab01cd02")->heard == 1, "direct MeshCore advert");
    p.from = "ee11ff22"; p.path = "c7,5a";
    topoFeedLora(t, st, p, 2);   // unknown hashes: placeholders; chain ee11ff22 > c7 > 5a > rx
    CHECK(t.find("MeshCore/?c7") && t.find("MeshCore/?5a"), "placeholders");
    TopoReport r = t.analyse("MeshCore");
    CHECK(r.links == 2, "two links in the chain: %d", r.links);
    topoNoteMeshNode(t, "5a99aabb", "", "", false, 0, 0, 3);   // would be noted as Meshtastic: ignore, MeshCore nodes arrive through packets
}

// ----------------------------------------------------------------- Z-Wave
static void testZwave() {
    // header round trip
    for (int reps = 0; reps <= 4; reps++) for (int hop = 0; hop <= reps; hop++) for (int type = 0; type <= 2; type++) {
        std::vector<int> rv; for (int i = 0; i < reps; i++) rv.push_back(10 + i);
        zwave::Route r;
        std::vector<uint8_t> pl = zwave::buildRoute(type, hop, rv);
        pl.push_back(0x25); pl.push_back(0x01); pl.push_back(0xFF);
        CHECK(zwave::parseRoute(pl, r) && r.type == type && r.count == reps && r.hop == hop && r.repeaters == rv && r.size == 2 + (size_t)reps, "route round trip %d %d %d", reps, hop, type);
    }
    zwave::Route bad;
    CHECK(!zwave::parseRoute({0x00, 0x51, 1, 2, 3, 4, 5}, bad), "5 repeaters refused");
    CHECK(!zwave::parseRoute({0x00, 0x23, 1, 2}, bad), "hop above count refused");
    CHECK(!zwave::parseRoute({0x00, 0x20, 1}, bad), "short header refused");
    CHECK(!zwave::parseRoute({0x07, 0x00}, bad), "unknown type refused");

    // a network: 1 controller - 2 - 3 - 4 (chain), 2 - 5 leaf, 3 - 6 leaf; routes of the frames follow the chain
    const uint32_t home = 0xC0FFEE01;
    struct Rt { int src, dst; std::vector<int> reps; };
    const std::vector<Rt> routes = {{1, 4, {2, 3}}, {4, 1, {3, 2}}, {1, 5, {2}}, {5, 1, {2}}, {1, 6, {2, 3}}, {6, 1, {3, 2}}};
    std::set<std::string> trueRoute = {key("C0FFEE01/1", "C0FFEE01/2"), key("C0FFEE01/2", "C0FFEE01/3"), key("C0FFEE01/3", "C0FFEE01/4"), key("C0FFEE01/2", "C0FFEE01/5"), key("C0FFEE01/3", "C0FFEE01/6")};
    printf("zwave routed frames (6 routes, 6 repeater hops, lossy receiver)\n  loss  frames  routeRecall  precision  worstCut\n");
    for (double loss : {0.0, 0.3, 0.6, 0.8}) {
        std::mt19937 g(77);
        MeshTopology t; int fed = 0;
        for (int k = 0; k < 120; k++) {
            const Rt& r = routes[(size_t)k % routes.size()];
            for (int hop = 0; hop <= (int)r.reps.size(); hop++) {
                if (std::uniform_real_distribution<double>(0, 1)(g) < loss) continue;
                zwave::Frame f; f.homeId = home; f.src = r.src; f.dst = r.dst; f.headerType = zwave::kHdrSinglecast; f.routed = true; f.seq = k & 15;
                f.payload = zwave::buildRoute(0, hop, r.reps); f.payload.push_back(0x25); f.payload.push_back(0x01); f.payload.push_back(0xFF);
                const int rate = 1 + (k % 3);
                std::vector<uint8_t> mpdu = zwave::build(f, rate);
                zwave::Frame back;
                if (!zwave::parse(mpdu.data(), mpdu.size(), rate, back) || !back.checkOk) { CHECK(false, "z-wave frame did not survive build/parse"); continue; }
                topoFeedZwave(t, back, 5.0f, 100 + k); fed++;
            }
        }
        TopoReport rep = t.analyse("C0FFEE01");
        std::set<std::string> got;
        double linkOk = 0, linkAll = 0;
        for (const auto& kv : t.edges()) { const TopoEdge& e = kv.second; if (e.weight() <= 0) continue; linkAll += 1; if (trueRoute.count(key(e.a, e.b))) { linkOk += 1; got.insert(key(e.a, e.b)); } }
        const double recall = (double)got.size() / trueRoute.size(), prec = linkAll > 0 ? linkOk / linkAll : 1;
        printf("  %.1f  %6d  %.2f         %.2f       %s%d\n", loss, fed, recall, prec, rep.cuts.empty() ? "-" : rep.cuts[0].id.c_str() + 9, rep.cuts.empty() ? 0 : rep.cuts[0].orphans);
        CHECK(prec == 1.0, "no invented links at loss %.1f", loss);
        if (loss <= 0.6) CHECK(recall == 1.0, "every link found at loss %.1f (%.2f)", loss, recall);
        if (loss <= 0.6) CHECK(!rep.cuts.empty() && rep.cuts[0].id == "C0FFEE01/2" && rep.cuts[0].orphans == 4 && rep.root == "C0FFEE01/1", "node 2 carries 4 nodes (%s %d, root %s)", rep.cuts.empty() ? "" : rep.cuts[0].id.c_str(), rep.cuts.empty() ? 0 : rep.cuts[0].orphans, rep.root.c_str());
    }
    // the description
    zwave::Frame f; f.homeId = home; f.src = 1; f.dst = 4; f.routed = true; f.headerType = zwave::kHdrSinglecast;
    f.payload = zwave::buildRoute(0, 1, {2, 3}); f.payload.push_back(0x25); f.payload.push_back(0x01); f.payload.push_back(0xFF);
    const std::string d = zwave::describe(f);
    CHECK(d.find("1 > 2 > 3 > 4") != std::string::npos && d.find("hop 1 of 3") != std::string::npos && d.find("SWITCH_BINARY") != std::string::npos, "describe: %s", d.c_str());
    // plain frames and acks
    MeshTopology t;
    zwave::Frame a; a.homeId = home; a.src = 7; a.dst = 1; a.headerType = zwave::kHdrAck;
    topoFeedZwave(t, a, 4, 1);
    auto it = t.edges().find({"C0FFEE01/1", "C0FFEE01/7"});
    CHECK(it != t.edges().end() && it->second.link == 1, "ack proves the link");
    zwave::Frame m; m.homeId = home; m.src = 1; m.dst = 255; m.headerType = zwave::kHdrSinglecast;
    topoFeedZwave(t, m, 4, 2);
    CHECK(t.find("C0FFEE01/1")->role == "controller", "node 1 is the controller");
}

// ----------------------------------------------------------- Zigbee and Thread
static zigbee::Frame zbFrame(uint16_t pan, uint16_t macSrc, uint16_t macDst, const zigbee::NwkBuild* nwk, int type = zigbee::Data) {
    zigbee::MacBuild m; m.type = type; m.destPan = m.srcPan = pan; m.srcShort = macSrc; m.destShort = macDst; m.seq = 1;
    if (nwk) m.payload = zigbee::buildNwk(*nwk);
    std::vector<uint8_t> psdu = zigbee::buildMac(m);
    return zigbee::parse(psdu.data(), psdu.size());
}

static void testZigbee() {
    // source route round trip
    zigbee::NwkBuild nb; nb.src = 0x0001; nb.dest = 0x0007; nb.relays = {0x0005, 0x0003}; nb.relayIndex = 1; nb.seq = 9; nb.route = false;
    nb.payload = {0x00};
    zigbee::Frame f = zbFrame(0x1A62, 0x0003, 0x0005, &nb);
    CHECK(f.ok && f.hasNwk && f.nwkSourceRoute && f.nwkRelays.size() == 2 && f.nwkRelays[0] == 0x0005 && f.nwkRelays[1] == 0x0003 && f.nwkRelayIndex == 1, "source route parsed (%d relays)", (int)f.nwkRelays.size());
    MeshTopology t;
    topoFeedIeee(t, f, nullptr, 0x1A62, 4, 1);
    // path: 0001 > 0003 > 0005 > 0007 (the list is walked from its end)
    auto has = [&](const char* a, const char* b) { auto it = t.edges().find({std::string("1A62/") + a < std::string("1A62/") + b ? std::string("1A62/") + a : std::string("1A62/") + b, std::string("1A62/") + a < std::string("1A62/") + b ? std::string("1A62/") + b : std::string("1A62/") + a}); return it != t.edges().end() && it->second.weight() > 0; };
    CHECK(has("0x0001", "0x0003") && has("0x0003", "0x0005") && has("0x0005", "0x0007"), "source route edges");
    CHECK(t.find("1A62/0x0003")->relayed == 1 && t.find("1A62/0x0003")->role == "router", "the transmitter relayed for 0x0001");

    // a network of coordinator 0000, routers 0001 0002, end devices behind them; frames: MAC hop by hop
    printf("zigbee relayed unicasts (coordinator, 2 routers in a chain, 4 end devices, lossy receiver)\n  loss  frames  linkRecall  precision  worstCut\n");
    struct Hop { uint16_t orig, dest, macSrc, macDst; };
    // true links: 0000-0001, 0001-0002, 0001-0010, 0002-0020, 0002-0021, 0000-0030
    const std::vector<Hop> flows = {{0x0010, 0x0000, 0x0010, 0x0001}, {0x0010, 0x0000, 0x0001, 0x0000}, {0x0020, 0x0000, 0x0020, 0x0002}, {0x0020, 0x0000, 0x0002, 0x0001}, {0x0020, 0x0000, 0x0001, 0x0000},
                                    {0x0021, 0x0000, 0x0021, 0x0002}, {0x0021, 0x0000, 0x0002, 0x0001}, {0x0021, 0x0000, 0x0001, 0x0000}, {0x0030, 0x0000, 0x0030, 0x0000}};
    const std::set<std::string> trueL = {key("A5/0x0000", "A5/0x0001"), key("A5/0x0001", "A5/0x0002"), key("A5/0x0001", "A5/0x0010"), key("A5/0x0002", "A5/0x0020"), key("A5/0x0002", "A5/0x0021"), key("A5/0x0000", "A5/0x0030")};
    for (double loss : {0.0, 0.5, 0.8}) {
        std::mt19937 g(5);
        MeshTopology tt; int fed = 0;
        for (int k = 0; k < 150; k++) {
            const Hop& h = flows[(size_t)k % flows.size()];
            if (std::uniform_real_distribution<double>(0, 1)(g) < loss) continue;
            zigbee::NwkBuild n2; n2.src = h.orig; n2.dest = h.dest; n2.seq = k & 255; n2.payload = {0x00};
            zigbee::Frame ff = zbFrame(0x00A5, h.macSrc, h.macDst, &n2);
            if (!ff.ok) { CHECK(false, "zigbee frame"); continue; }
            topoFeedIeee(tt, ff, nullptr, 0x00A5, 5, k); fed++;
        }
        TopoReport rep = tt.analyse("00A5");
        std::set<std::string> got; double ok = 0, all = 0;
        std::string pre = "00A5/";
        for (const auto& kv : tt.edges()) {
            const TopoEdge& e = kv.second; if (e.weight() <= 0) continue; all++;
            std::string x = e.a.substr(5), y = e.b.substr(5);
            const std::string k2 = key("A5/" + x, "A5/" + y);
            if (trueL.count(k2)) { ok++; got.insert(k2); }
        }
        printf("  %.1f  %6d  %.2f        %.2f       %s%d\n", loss, fed, (double)got.size() / trueL.size(), all > 0 ? ok / all : 1, rep.cuts.empty() ? "-" : rep.cuts[0].id.c_str() + 5, rep.cuts.empty() ? 0 : rep.cuts[0].orphans);
        CHECK(all == 0 || ok / all == 1.0, "no invented links (loss %.1f)", loss);
        if (loss <= 0.5) CHECK(got.size() == trueL.size(), "all links found at loss %.1f (%zu of %zu)", loss, got.size(), trueL.size());
        if (loss <= 0.5) CHECK(rep.root == "00A5/0x0000" && !rep.cuts.empty() && rep.cuts[0].id == "00A5/0x0001" && rep.cuts[0].orphans == 4, "router 0001 carries 4 nodes (%s %d)", rep.cuts.empty() ? "" : rep.cuts[0].id.c_str(), rep.cuts.empty() ? 0 : rep.cuts[0].orphans);
    }
    // broadcasts feed the flood factor, beacons give roles
    MeshTopology b;
    for (int c = 0; c < 3; c++) { zigbee::NwkBuild bn; bn.src = 0x0010; bn.dest = 0xFFFD; bn.seq = 4; bn.payload = {0x00}; zigbee::Frame ff = zbFrame(0x00A5, c == 0 ? 0x0010 : (uint16_t)(0x0001 + c), 0xFFFF, &bn); topoFeedIeee(b, ff, nullptr, 0x00A5, 3, c); }
    CHECK(std::fabs(b.analyse("00A5").floodFactor - 3.0) < 1e-9, "broadcast copies counted: %.2f", b.analyse("00A5").floodFactor);

    // Thread: MAC links are in the clear even when the payload is encrypted; the mesh header gives the originator
    zigbee::MacBuild tm; tm.type = zigbee::Data; tm.destPan = tm.srcPan = 0xFACE; tm.srcShort = 0x4400; tm.destShort = 0x6C00; tm.seq = 3;
    // 6LoWPAN mesh header: 10 V F hops=3 (both short), originator 0x2000, final 0x7C00, then an IPHC byte
    tm.payload = {0xB3, 0x20, 0x00, 0x7C, 0x00, 0x7A, 0x33};
    std::vector<uint8_t> psdu = zigbee::buildMac(tm);
    zigbee::Frame tf = zigbee::parse(psdu.data(), psdu.size());
    thread::Frame th = thread::analyse(tf, psdu.data(), psdu.size() - 2);
    CHECK(th.mesh && th.meshOrig == "0x2000" && th.meshFinal == "0x7C00" && th.meshHops == 3, "thread mesh header %s %s", th.meshOrig.c_str(), th.meshFinal.c_str());
    MeshTopology tt;
    topoFeedIeee(tt, tf, &th, 0xFACE, 6, 1);
    CHECK(tt.find("FACE/0x4400") && tt.find("FACE/0x4400")->relayed == 1 && tt.find("FACE/0x2000"), "thread: relay of a mesh-header frame");
    auto it = tt.edges().find({"FACE/0x4400", "FACE/0x6C00"});
    CHECK(it != tt.edges().end() && it->second.link == 1, "thread MAC link");
}

int main() {
    testModel();
    testMeshtastic();
    testMeshCore();
    testZwave();
    testZigbee();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("mesh topology: all passed\n");
    return 0;
}
