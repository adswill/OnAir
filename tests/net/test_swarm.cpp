// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm pure logic: announce parsing with hostile packets, node table rules, message de-duplication, range splitting, pairing handshake
// (right code, wrong code, lockout, replay, other peers, malformed lines), tokens, rate limit and memory sync.
#include "dect2/net/swarm.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
using namespace dect2::swarm;
#include "../common/test_util.h"

static uint32_t ip(int a, int b, int c, int d) { return ((uint32_t)a << 24) | (b << 16) | (c << 8) | d; }
static RandFn fixedRandom(uint8_t seed) {
    auto n = std::make_shared<uint8_t>(seed);
    return [n](uint8_t* p, size_t len) { for (size_t i = 0; i < len; i++) p[i] = (uint8_t)((*n)++ * 37 + 11); };
}
static Announce good() {
    Announce a;
    a.id = "0123456789abcdef"; a.name = "Roof Pi"; a.radio = "RTL-SDR"; a.mode = "ADS-B"; a.summary = "42 aircraft";
    a.loHz = 24e6; a.hiHz = 1766e6; a.load = 35; a.ctlPort = 8790;
    return a;
}
static bool parse(const std::string& s, Announce& a, std::string& e) { return parseAnnounce((const uint8_t*)s.data(), s.size(), a, e); }

static void testAnnounce() {
    Announce a, o = good();
    std::string e, pkt = encodeAnnounce(o);
    CHECK(parse(pkt, a, e), "roundtrip: %s", e.c_str());
    CHECK(a.id == o.id && a.name == o.name && a.mode == o.mode && a.load == 35 && a.ctlPort == 8790 && a.hiHz == 1766e6, "fields");
    CHECK(announcePreview(o).find("local network only") != std::string::npos, "preview says local only");
    // hostile
    Announce keep = good();
    keep.name = "kept";
    auto bad = [&](const std::string& s, const char* what) { Announce t = keep; std::string er; CHECK(!parse(s, t, er), "accepted: %s", what); CHECK(t.name == "kept", "modified on failure: %s", what); };
    bad("", "empty");
    bad("OASWARM1\n", "no fields");
    bad(pkt.substr(0, pkt.size() - 1), "no final newline");
    bad("XXSWARM1\n" + pkt.substr(9), "magic");
    bad(pkt + "evil=1\n", "unknown key");
    bad(pkt + "id=0123456789abcdef\n", "duplicate key");
    bad(std::string(600, 'a'), "too long");
    std::string nul = pkt; nul[12] = 0; bad(nul, "NUL");
    { std::string s = pkt; s.replace(s.find("id=0123456789abcdef"), 19, "id=0123456789ABCDEF"); bad(s, "uppercase id"); }
    { std::string s = pkt; s.replace(s.find("id="), 19, "id=../../etc/passwd"); bad(s, "id path"); }
    { Announce x = o; x.loHz = 2e9; x.hiHz = 1e9; bad(encodeAnnounce(x), "hi < lo"); }
    { Announce x = o; x.load = 101; bad(encodeAnnounce(x), "load 101"); }
    { Announce x = o; x.ctlPort = 70000; bad(encodeAnnounce(x), "port"); }
    { std::string s = pkt; s.replace(s.find("lo=24000000"), 11, "lo=nan"); bad(s, "nan"); }
    { std::string s = pkt; s.replace(s.find("lo=24000000"), 11, "lo=1e99"); bad(s, "1e99"); }
    { std::string s = pkt; s += "x\n"; bad(s, "line without ="); }
    { std::string s = pkt; s.replace(s.find("name=Roof Pi"), 12, "name=Roof\x01Pi"); bad(s, "control char in name"); }
    { std::string s = pkt; s.replace(s.find("name=Roof Pi"), 12, "name="); bad(s, "empty name"); }
    // encoder sanitises: control chars and '=' removed, length capped
    { Announce x = o; x.name = std::string("a\nb=c") + std::string(100, 'z'); std::string er; Announce r; CHECK(parse(encodeAnnounce(x), r, er) && r.name.size() == 32, "sanitised name %s", er.c_str()); }
    // fuzz: random bytes never crash or succeed by accident
    uint32_t s = 12345; int okc = 0;
    for (int i = 0; i < 5000; i++) {
        std::string f = pkt;
        for (int k = 0; k < 4; k++) { s = s * 1664525u + 1013904223u; f[(s >> 8) % f.size()] = (char)(s >> 24); }
        Announce t; std::string er;
        if (parse(f, t, er)) okc++;
    }
    CHECK(okc < 5000, "fuzz");
    CHECK(isPrivateIpv4(ip(192, 168, 1, 5)) && isPrivateIpv4(ip(10, 1, 2, 3)) && isPrivateIpv4(ip(172, 31, 0, 1)) && !isPrivateIpv4(ip(172, 32, 0, 1)) && !isPrivateIpv4(ip(8, 8, 8, 8)), "private ranges");
    CHECK(onLocalSubnet(ip(192, 168, 1, 9), ip(192, 168, 1, 5), 0xFFFFFF00) && !onLocalSubnet(ip(192, 168, 2, 9), ip(192, 168, 1, 5), 0xFFFFFF00), "subnet");
    CHECK(!onLocalSubnet(ip(10, 0, 0, 1), ip(192, 168, 1, 5), 0), "mask 0 never matches everything");
    CHECK(!onLocalSubnet(ip(10, 0, 0, 1), ip(10, 9, 9, 9), 0xFF00FF00), "non-contiguous mask refused");
    CHECK(!onLocalSubnet(ip(224, 0, 0, 1), ip(224, 0, 0, 2), 0xFFFFFF00) && !onLocalSubnet(0xFFFFFFFF, ip(255, 255, 255, 1), 0xFFFFFF00), "multicast / broadcast source refused");
}

static void testNodes() {
    NodeTable t("ffffffffffffffff");
    uint32_t local = ip(192, 168, 1, 2), mask = 0xFFFFFF00;
    Announce a = good();
    CHECK(t.update(a, ip(192, 168, 1, 10), local, mask, 1000) == NodeTable::Accepted, "accepted");
    CHECK(t.update(a, ip(192, 168, 1, 10), local, mask, 1500) == NodeTable::RateLimited, "rate limit per source");
    CHECK(t.update(a, ip(192, 168, 1, 10), local, mask, 2500) == NodeTable::Accepted, "refresh later");
    CHECK(t.update(a, ip(192, 168, 1, 11), local, mask, 3000) == NodeTable::Spoofed, "same id other address refused while alive");
    CHECK(t.update(a, ip(192, 168, 2, 10), local, mask, 3000) == NodeTable::WrongSubnet, "other subnet");
    CHECK(t.update(a, ip(8, 8, 8, 8), local, mask, 3000) == NodeTable::BadSource, "public source");
    Announce self = a; self.id = "ffffffffffffffff";
    CHECK(t.update(self, ip(192, 168, 1, 12), local, mask, 3000) == NodeTable::SelfId, "own id");
    CHECK(t.size() == 1, "one node");
    CHECK(t.update(a, ip(192, 168, 1, 11), local, mask, 40000) == NodeTable::Accepted, "takeover allowed after expiry");
    t.expire(80000);
    CHECK(t.size() == 0, "expired");
    // one address cannot flood the table with fake ids
    NodeTable f; int acc = 0;
    for (int i = 0; i < 20; i++) {
        Announce x = good(); char b[20]; snprintf(b, sizeof b, "%016x", i + 1); x.id = b;
        if (f.update(x, ip(192, 168, 1, 50), local, mask, 1000 + i * 1500) == NodeTable::Accepted) acc++;
    }
    CHECK(acc == 4, "at most 4 ids per address (got %d)", acc);
    NodeTable g("", 30000, 3); acc = 0;
    for (int i = 0; i < 10; i++) {
        Announce x = good(); char b[20]; snprintf(b, sizeof b, "%016x", i + 1); x.id = b;
        if (g.update(x, ip(192, 168, 1, 100 + i), local, mask, 1000) == NodeTable::Accepted) acc++;
    }
    CHECK(acc == 3 && g.size() == 3, "table cap");
}

static void testMerge() {
    MessageMerger m;
    int64_t now = 1000000;
    Msg a{"adsb", "ICAO 4840D6 callsign KLM1234  alt 36000", "nodeA", now};
    CHECK(m.add(a, now) == MessageMerger::New, "new");
    Msg b = a; b.nodeId = "nodeB"; b.text = "icao 4840d6 CALLSIGN klm1234 alt 36000"; b.timeMs = now + 800;
    CHECK(m.add(b, now) == MessageMerger::Duplicate, "same content other node = duplicate");
    CHECK(m.rows().size() == 1 && m.rows()[0].nodes.size() == 2, "tagged with both nodes");
    Msg c = a; c.nodeId = "nodeA"; c.timeMs = now + 800;
    m.add(c, now);
    CHECK(m.rows()[0].nodes.size() == 2, "same node twice not tagged twice");
    Msg d = a; d.timeMs = now + 5000;
    CHECK(m.add(d, now) == MessageMerger::New && m.rows().size() == 2, "same content much later is a new row");
    Msg e = a; e.kind = "ais"; e.timeMs = now;
    CHECK(m.add(e, now) == MessageMerger::New, "other kind is separate");
    // rejects
    Msg x = a; x.text = "";
    CHECK(m.add(x, now) == MessageMerger::Rejected, "empty text");
    x = a; x.text = std::string(513, 'a');
    CHECK(m.add(x, now) == MessageMerger::Rejected, "too long");
    x = a; x.kind = "Bad Kind!";
    CHECK(m.add(x, now) == MessageMerger::Rejected, "bad kind");
    x = a; x.timeMs = now + 10 * 86400000LL;
    CHECK(m.add(x, now) == MessageMerger::Rejected, "time far in the future");
    x = a; x.timeMs = -5;
    CHECK(m.add(x, now) == MessageMerger::Rejected, "negative time");
    x = a; x.text = std::string("ab\0cd", 5);
    CHECK(m.add(x, now) == MessageMerger::Rejected, "NUL");
    x = a; x.text = "   \t\n ";
    CHECK(m.add(x, now) == MessageMerger::Rejected, "only whitespace");
    x = a; x.nodeId = "";
    CHECK(m.add(x, now) == MessageMerger::Rejected, "no node id");
    // a flood of node tags is bounded, a flood of rows is bounded
    MessageMerger f(2000, 100);
    for (int i = 0; i < 40; i++) { Msg y = a; y.text = "same"; y.nodeId = "n" + std::to_string(i); f.add(y, now); }
    CHECK(f.rows().size() == 1 && f.rows()[0].nodes.size() == 16, "node tags capped");
    for (int i = 0; i < 1000; i++) { Msg y = a; y.text = "row " + std::to_string(i); f.add(y, now); }
    CHECK(f.rows().size() == 100, "rows capped (%zu)", f.rows().size());
    Msg old = a; old.text = "row 5";
    CHECK(f.add(old, now) == MessageMerger::New, "evicted row is not a duplicate any more");
    // tracks
    TrackBoard t;
    CHECK(t.update("adsb", "4840D6", 52.1, 4.5, "A", now, now), "track");
    CHECK(t.update("adsb", "4840D6", 52.2, 4.6, "B", now + 1000, now), "update newer");
    CHECK(t.update("adsb", "4840D6", 10.0, 10.0, "C", now - 1000, now), "older report only adds tag");
    auto l = t.list();
    CHECK(l.size() == 1 && l[0].lat == 52.2 && l[0].nodes.size() == 3 && l[0].nodeId == "B", "one combined track");
    CHECK(!t.update("adsb", "X", 91, 0, "A", now, now) && !t.update("adsb", "X", 0, 0, "A", now, now) && !t.update("adsb", "X", NAN, 1, "A", now, now) &&
          !t.update("adsb", "X;DROP", 1, 1, "A", now, now) && !t.update("adsb", "", 1, 1, "A", now, now), "bad tracks refused");
    t.expire(now + 300000);
    CHECK(t.list().empty(), "tracks expire");
    TrackBoard small(2);
    CHECK(small.update("ais", "1", 1, 1, "A", now, now) && small.update("ais", "2", 1, 1, "A", now, now) && !small.update("ais", "3", 1, 1, "A", now, now), "track cap");
}

static void testSplit() {
    SplitResult r; std::string e;
    std::vector<Cap> caps = {{"A", 24e6, 1766e6, 2e6, 0}, {"B", 1e6, 6000e6, 20e6, 0}};
    CHECK(splitRange(100e6, 200e6, caps, 100e3, r, e), "split: %s", e.c_str());
    double covered = 0, a = 0, b = 0, prevHi = 100e6; bool contiguous = true;
    for (auto& s : r.slices) { covered += s.hiHz - s.loHz; (s.id == "A" ? a : b) += s.hiHz - s.loHz; if (std::fabs(s.loHz - prevHi) > 1e-3) contiguous = false; prevHi = s.hiHz; }
    CHECK(std::fabs(covered - 100e6) < 1 && contiguous && r.uncovered.empty(), "full coverage, no gaps or overlaps");
    CHECK(b > a * 5 && a > 0, "the wide node gets more (A %.0f B %.0f)", a, b);
    // busy node gets less
    caps = {{"A", 0, 1e9, 10e6, 0}, {"B", 0, 1e9, 10e6, 90}};
    splitRange(0, 400e6, caps, 1e5, r, e);
    a = b = 0; for (auto& s : r.slices) (s.id == "A" ? a : b) += s.hiHz - s.loHz;
    CHECK(a > b * 3, "load weighting (A %.0f B %.0f)", a, b);
    // a part nobody can tune is reported
    caps = {{"A", 100e6, 150e6, 2e6, 0}};
    splitRange(50e6, 200e6, caps, 1e5, r, e);
    CHECK(r.uncovered.size() == 2 && r.uncovered[0].first == 50e6 && r.uncovered[0].second == 100e6 && r.uncovered[1].first == 150e6, "gaps reported");
    // bad input
    CHECK(!splitRange(200e6, 100e6, caps, 1e5, r, e), "reversed");
    CHECK(!splitRange(1e6, 2e6, {}, 1e5, r, e), "no nodes");
    CHECK(!splitRange(1e6, 2e6, caps, 0, r, e), "step 0");
    CHECK(!splitRange(NAN, 2e6, caps, 1e5, r, e) && !splitRange(1e6, INFINITY, caps, 1e5, r, e), "nan / inf");
    CHECK(!splitRange(0, 1e9, {{"A", 0, 1e9, 1, 0}}, 1, r, e), "too many slices refused (%s)", e.c_str());
    CHECK(!splitRange(1e6, 2e6, {{"A", 5, 1, 1e6, 0}}, 1e3, r, e), "bad cap range");
    CHECK(!splitRange(1e6, 2e6, {{"A", 0, 1e9, 1e6, 101}}, 1e3, r, e), "bad load");
    CHECK(!splitRange(1e6, 2e6, {{"A", 0, 1e9, 1e6, 0}, {"A", 0, 1e9, 1e6, 0}}, 1e3, r, e), "duplicate id");
    // hits
    std::vector<Hit> h = {{100.0e6, -60, "A"}, {100.0e6 + 2e3, -50, "B"}, {101e6, -70, "A"}, {NAN, 0, "A"}, {-1, 0, "A"}, {102e6, INFINITY, "A"}, {103e6, -40, "bad\nnode"}};
    auto m = mergeHits(h, 5e3);
    CHECK(m.size() == 2 && m[0].level == -50 && m[0].freqHz == 100.0e6 + 2e3 && m[0].nodes.size() == 2, "hits merged, invalid dropped (%zu)", m.size());
    CHECK(mergeHits(h, NAN).size() >= 2 && mergeHits({}, 1).empty(), "nan tolerance / empty");
}

static void testPairing() {
    auto rnd = fixedRandom(7);
    std::string code = newPairingCode(rnd);
    CHECK(code.size() == 32 && normalisePairingCode(code) == code, "code is 32 base32 characters");
    for (char c : code) CHECK((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'), "base32 char");
    CHECK(normalisePairingCode(code.substr(0, 4) + " - " + code.substr(4)) == code, "separators ignored");
    CHECK(normalisePairingCode(code.substr(0, 31)).empty() && normalisePairingCode(code + "A").empty() && normalisePairingCode("1" + code.substr(1)).empty() &&
          normalisePairingCode(code.substr(0, 31) + "!").empty() && normalisePairingCode("").empty(), "bad length / characters refused");
    { std::string lower = code; for (char& c : lower) c = (char)tolower((unsigned char)c); CHECK(normalisePairingCode(lower) == code, "lower case typed"); }
    { PairResponder rr; CHECK(!rr.offer("123456", 0) && !rr.offer(std::string(32, 'a'), 0) && !rr.offer(std::string(), 0), "short / unnormalised code refused as offer");
      CHECK(rr.handle("OAPAIR1 HELLO 00112233445566778899aabbccddeeff", "p", 0) == "OAPAIR1 ERR not pairing", "nothing offered"); }
    CHECK(newPairingCode(systemRandom()) != newPairingCode(systemRandom()), "random codes differ");
    int64_t t = 1000;
    // good handshake
    PairResponder r(fixedRandom(1));
    PairInitiator i(fixedRandom(99));
    r.offer(code, t);
    std::string l1 = i.hello(), l2 = r.handle(l1, "peer1", t), l3 = i.handle(l2, code), l4 = r.handle(l3, "peer1", t);
    CHECK(l2.rfind("OAPAIR1 CHAL ", 0) == 0 && l3.rfind("OAPAIR1 PROOF ", 0) == 0 && l4.rfind("OAPAIR1 OK ", 0) == 0, "lines: %s | %s | %s", l2.c_str(), l3.c_str(), l4.c_str());
    CHECK(i.handle(l4, code).empty() && i.paired(), "initiator paired");
    std::string tok, peer;
    CHECK(r.takePaired(tok, peer) && tok == i.token() && tok.size() == 24 && peer == "peer1", "same token both sides");
    CHECK(!r.takePaired(tok, peer), "token handed out once");
    CHECK(l4.find(tok) == std::string::npos && l3.find(code) == std::string::npos && l1.find(code) == std::string::npos, "neither code nor token sent in clear");
    CHECK(r.handle(l1, "peer1", t).rfind("OAPAIR1 ERR", 0) == 0, "code is single use");
    TokenBook tb; tb.add(tok, "Roof Pi");
    std::string label;
    CHECK(tb.authorise(tok, &label) && label == "Roof Pi" && !tb.authorise(tok.substr(0, 23)) && !tb.authorise(tok + "0") && !tb.authorise("") && tb.revoke("Roof Pi") && !tb.authorise(tok), "token book");
    // wrong code on the initiator side: three tries then locked, correct code afterwards refused
    PairResponder r2(fixedRandom(3));
    r2.offer(code, t);
    for (int k = 0; k < 3; k++) {
        PairInitiator bad(fixedRandom(50 + k));
        std::string c = r2.handle(bad.hello(), "evil", t);
        std::string p = bad.handle(c, std::string(32, 'C'));
        std::string res = r2.handle(p, "evil", t);
        CHECK(res == "OAPAIR1 ERR bad proof", "bad proof %d: %s", k, res.c_str());
    }
    CHECK(r2.state(t) == PairResponder::Locked, "locked after 3");
    PairInitiator late(fixedRandom(77));
    CHECK(r2.handle(late.hello(), "peer1", t) == "OAPAIR1 ERR locked", "locked even for right code");
    r2.offer(code, t);
    CHECK(r2.state(t) == PairResponder::Offered, "new offer re-arms");
    // expiry
    PairResponder r3(fixedRandom(4)); r3.offer(code, t, 1000);
    CHECK(r3.handle(PairInitiator(fixedRandom(1)).hello(), "p", t + 2000) == "OAPAIR1 ERR not pairing", "expired offer");
    // not offered at all
    PairResponder r4;
    CHECK(r4.handle("OAPAIR1 HELLO 00112233445566778899aabbccddeeff", "p", t) == "OAPAIR1 ERR not pairing", "idle refuses");
    // second peer while one is mid-handshake; the first can still finish; proof from the wrong peer is refused
    PairResponder r5(fixedRandom(5)); r5.offer(code, t);
    PairInitiator ia(fixedRandom(10)), ib(fixedRandom(20));
    std::string ca = r5.handle(ia.hello(), "A", t);
    CHECK(r5.handle(ib.hello(), "B", t + 100) == "OAPAIR1 ERR busy", "busy");
    std::string pa = ia.handle(ca, code);
    CHECK(r5.handle(pa, "B", t + 200) == "OAPAIR1 ERR no challenge", "proof relayed by another peer refused");
    CHECK(r5.handle(pa, "A", t + 300).rfind("OAPAIR1 OK ", 0) == 0, "first peer finishes");
    // a stale challenge frees the slot
    PairResponder r6(fixedRandom(6)); r6.offer(code, t);
    r6.handle(PairInitiator(fixedRandom(1)).hello(), "A", t);
    CHECK(r6.handle(PairInitiator(fixedRandom(2)).hello(), "B", t + 11000).rfind("OAPAIR1 CHAL", 0) == 0, "challenge times out");
    // a fake server that does not know the code is detected by the initiator
    PairInitiator ic(fixedRandom(30));
    std::string h = ic.hello();
    PairResponder fake(fixedRandom(8)); fake.offer(std::string(32, 'B'), t);
    std::string chal = fake.handle(h, "p", t), proof = ic.handle(chal, code);
    CHECK(fake.handle(proof, "p", t) == "OAPAIR1 ERR bad proof" && !ic.paired(), "wrong code -> no pairing");
    PairInitiator id(fixedRandom(31));
    id.hello();
    id.handle("OAPAIR1 CHAL 00112233445566778899aabbccddeeff", code);
    CHECK(id.handle("OAPAIR1 OK " + std::string(64, '0') + " " + std::string(48, '1'), code).empty() && id.failed() && !id.paired() && id.token().empty(), "forged OK refused");
    // malformed lines never crash or pair
    PairResponder r7(fixedRandom(9)); r7.offer(code, t);
    const char* junk[] = {"", " ", "OAPAIR1", "OAPAIR1 ", "OAPAIR1 HELLO", "OAPAIR1 HELLO zz", "OAPAIR1 HELLO 0011", "OAPAIR1  HELLO 00112233445566778899aabbccddeeff",
                          "OAPAIR1 PROOF 00", "OAPAIR1 PROOF gg112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "OAPAIR2 HELLO 00112233445566778899aabbccddeeff",
                          "OAPAIR1 HELLO 00112233445566778899aabbccddeeff extra word here", "OAPAIR1 BYE x", "OAPAIR1 OK a b"};
    for (const char* j : junk) CHECK(r7.handle(j, "p", t).rfind("OAPAIR1 ERR", 0) == 0, "junk accepted: %s", j);
    CHECK(r7.handle(std::string("OAPAIR1 HELLO ") + std::string(5000, 'a'), "p", t).rfind("OAPAIR1 ERR", 0) == 0, "long line");
    CHECK(r7.handle(std::string("OAPAIR1 HELLO 0011223344556677\0099aabbccddeeff", 46), "p", t).rfind("OAPAIR1 ERR", 0) == 0, "NUL in line");
    CHECK(r7.handle("OAPAIR1 PROOF " + std::string(64, '0'), "p", t) == "OAPAIR1 ERR no challenge", "proof without hello");
    CHECK(r7.state(t) == PairResponder::Offered, "junk does not lock the offer");
    PairInitiator ie(fixedRandom(1));
    CHECK(ie.handle("OAPAIR1 OK x y", code).empty() && !ie.paired(), "initiator ignores OK before hello");
    // control whitelist
    CHECK(controlAllowed("set_frequency") && controlAllowed("scan_slice") && controlAllowed("status"), "allowed");
    const char* denied[] = {"tx", "ptt", "transmit", "set_frequency;tx", "voice_keys", "get_key", "settings", "set_token", "shell", "SET_FREQUENCY", "", "set_frequency ", "../status"};
    for (const char* d : denied) CHECK(!controlAllowed(d), "allowed: %s", d);
    // rate limiter
    RateLimiter rl(5, 10);
    int ok = 0;
    for (int k = 0; k < 100; k++) if (rl.allow(1000)) ok++;
    CHECK(ok == 10, "burst %d", ok);
    CHECK(!rl.allow(1100) && rl.allow(1400), "refills at 5/s");
    CHECK(!rl.allow(500) || true, "clock going back does not crash");
    ok = 0; for (int k = 0; k < 100; k++) if (rl.allow(1000)) ok++;
    CHECK(ok <= 1, "backwards time gives no free tokens (%d)", ok);
}

static void testMemories() {
    std::vector<Mem> l = {{"a", "Airport", "AM", 118.1e6, "", 100, false}, {"b", "FM", "WFM", 93.9e6, "", 100, false}};
    std::vector<Mem> r = {{"a", "Airport tower", "AM", 118.1e6, "", 200, false}, {"b", "FM", "WFM", 93.9e6, "", 50, false}, {"c", "New", "NFM", 145.5e6, "", 10, false}};
    auto st = mergeMemories(l, r);
    CHECK(st.added == 1 && st.updated == 1 && st.kept == 1 && l.size() == 3 && l[0].name == "Airport tower", "merge: +%d ~%d =%d", st.added, st.updated, st.kept);
    std::vector<Mem> del = {{"b", "", "", 0, "", 300, true}};
    st = mergeMemories(l, del);
    CHECK(st.removed == 1 && l[1].deleted, "tombstone wins when newer");
    std::vector<Mem> back = {{"b", "FM", "WFM", 93.9e6, "", 200, false}};
    st = mergeMemories(l, back);
    CHECK(st.kept == 1 && l[1].deleted, "older edit does not resurrect");
    // hostile
    std::vector<Mem> evil = {{"", "x", "", 1, "", 1, false}, {"../x", "x", "", 1, "", 1, false}, {"k", std::string(65, 'a'), "", 1, "", 1, false}, {"k", "n\n", "", 1, "", 1, false},
                             {"k", "n", "", NAN, "", 1, false}, {"k", "n", "", -5, "", 1, false}, {"k", "n", "", 1e15, "", 1, false}, {"k", "n", "", 1, std::string(201, 'a'), 1, false},
                             {"k", "n", "", 1, "", -1, false}, {std::string(49, 'k'), "n", "", 1, "", 1, false}};
    size_t before = l.size();
    st = mergeMemories(l, evil);
    CHECK(st.rejected == 10 && l.size() == before, "hostile entries rejected (%d)", st.rejected);
    std::vector<Mem> big;
    for (int i = 0; i < 12000; i++) big.push_back({"k" + std::to_string(i), "n", "", 1, "", 1, false});
    std::vector<Mem> dst;
    mergeMemories(dst, big);
    CHECK(dst.size() == 10000, "entry cap");
    std::vector<Mem> dup = {{"z", "one", "", 1, "", 5, false}, {"z", "two", "", 1, "", 6, false}, {"z", "three", "", 1, "", 4, false}};
    std::vector<Mem> d2; mergeMemories(d2, dup);
    CHECK(d2.size() == 1 && d2[0].name == "two", "duplicates in remote: newest wins");
}

int main() {
    testAnnounce();
    testNodes();
    testMerge();
    testSplit();
    testPairing();
    testMemories();
    return testSummary("swarm");
}
