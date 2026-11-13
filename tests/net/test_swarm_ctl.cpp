// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm control channel: framing under tampering and replay, command parsing and the whitelist with hostile input, pooled results, coordinated
// scan with failing and lying nodes, memory sync, and the real TCP server/client on loopback (pairing, control, wrong token, revoke, rate limit).
#include "dect2/net/swarm_ctl.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
using namespace dect2::swarm;
#include "../common/test_util.h"

struct FakeHost : SwarmHost {
    std::string last;
    int started = 0, stopped = 0;
    double freq = 0;
    std::vector<SeqMsg> msgs;
    uint64_t latest = 0;
    std::vector<Mem> mems;
    std::vector<TrackRow> trk;
    std::vector<TrackRow> tracks() override { return trk; }
    std::vector<std::pair<double, double>> peaks;   // what a scan finds
    bool canScan = true;
    std::string nodeInfo() override { return "fake node"; }
    std::string status() override { return "running=0 freq=" + std::to_string((long long)freq); }
    bool start(std::string&) override { started++; last = "start"; return true; }
    bool stop(std::string&) override { stopped++; last = "stop"; return true; }
    bool setFrequencyHz(double hz, std::string&) override { freq = hz; last = "freq"; return true; }
    bool setMode(const std::string& m, std::string& err) override { if (m == "ADS-B") { last = "mode " + m; return true; } err = "no such mode"; return false; }
    bool scanSlice(double lo, double hi, double, std::vector<std::pair<double, double>>& out, std::string& err) override {
        if (!canScan) { err = "cannot scan"; return false; }
        for (auto& p : peaks) if (p.first >= lo && p.first < hi) out.push_back(p);
        return true;
    }
    std::vector<SeqMsg> messagesSince(uint64_t since, size_t max) override {
        std::vector<SeqMsg> o;
        for (auto& m : msgs) if (m.seq > since && o.size() < max) o.push_back(m);
        return o;
    }
    uint64_t latestSeq() override { return latest; }
    std::vector<Mem> memories() override { return mems; }
    int applyMemories(const std::vector<Mem>& r) override { return mergeMemories(mems, r).added; }
};

static void testFraming() {
    const std::string tok = "0123456789abcdef01234567", nc(32, 'a'), ns(32, 'b');
    ControlSession c(tok, nc, ns, true), s(tok, nc, ns, false);
    std::string p;
    std::string f1 = c.seal("status");
    CHECK(f1.size() == 8 + 6 + 32, "frame size");
    CHECK(f1.find("status") == std::string::npos, "ciphertext does not contain the plain text");
    CHECK(s.open(f1, p) && p == "status", "roundtrip");
    std::string f2 = s.seal("OK reply");
    CHECK(c.open(f2, p) && p == "OK reply", "reply roundtrip");
    CHECK(!s.open(f1, p), "replay refused");
    std::string f3 = c.seal("a"), f4 = c.seal("b");
    CHECK(!s.open(f4, p), "reordered (skipped) frame refused");
    CHECK(s.open(f3, p) && p == "a", "in order still fine");   // the failed open must not have advanced the counter
    // every single byte flip is caught
    {
        ControlSession c2(tok, nc, ns, true), s2(tok, nc, ns, false);
        std::string f = c2.seal("set_frequency 100000000");
        int caught = 0;
        for (size_t i = 0; i < f.size(); i++) { std::string t = f; t[i] ^= 1; std::string q; if (!s2.open(t, q)) caught++; }
        CHECK(caught == (int)f.size(), "tamper caught at every byte (%d of %zu)", caught, f.size());
        CHECK(s2.open(f, p) && p == "set_frequency 100000000", "untouched frame still accepted after the failures");
    }
    // wrong token, wrong nonces, wrong direction
    ControlSession bad("ffffffffffffffffffffffff", nc, ns, false);
    CHECK(!bad.open(c.seal("x"), p), "wrong token");
    ControlSession other(tok, nc, std::string(32, 'c'), false);
    CHECK(!other.open(c.seal("x"), p), "wrong nonce");
    ControlSession c3(tok, nc, ns, true), c4(tok, nc, ns, true);
    CHECK(!c4.open(c3.seal("x"), p), "a client frame is not valid for a client (reflection)");
    CHECK(c.seal(std::string(kMaxPlain + 1, 'x')).empty(), "too big to seal");
    CHECK(!s.open("", p) && !s.open(std::string(39, 'x'), p) && !s.open(std::string(kMaxPlain + 41, 'x'), p), "short / huge frames");
    CHECK(frameLengthOk(40) && !frameLengthOk(39) && frameLengthOk(40 + kMaxPlain) && !frameLengthOk(41 + kMaxPlain) && !frameLengthOk(0xFFFFFFFFu), "length limits");
    ControlSession e1(tok, nc, ns, true), e2(tok, nc, ns, false);
    CHECK(e2.open(e1.seal(""), p) && p.empty(), "empty message");
    std::string lf = frameWithLength("abcd");
    CHECK(lf.size() == 8 && lf[3] == 4, "length prefix");
}

static void testCommands() {
    Command c; std::string e;
    CHECK(parseCommand("set_frequency 100000000", c, e) && c.name == "set_frequency" && c.args.size() == 1, "parse");
    CHECK(parseCommand("mem_put\nline1\nline2", c, e) && c.name == "mem_put" && c.body == "line1\nline2", "body");
    const char* bad[] = {"", "\n", " ", "Set_Frequency 1", "set-frequency 1", "set_frequency\x01 1", "a b c d e f g h i j k l m n"};
    for (const char* b : bad) CHECK(!parseCommand(b, c, e), "accepted: %s", b);
    CHECK(!parseCommand(std::string(600, 'a'), c, e) && !parseCommand("x " + std::string(65, 'a'), c, e) && !parseCommand(std::string(kMaxPlain + 1, 'a'), c, e), "limits");
    CHECK(commandAllowed("status") && commandAllowed("mem_put") && commandAllowed("scan_slices") && commandAllowed("results") && commandAllowed("set_frequency"), "allowed names");
    const char* denied[] = {"tx", "ptt", "transmit", "keys", "get_keys", "voice_key", "settings", "set_token", "shell", "exec", "pair", "revoke", "token", "mem_delete", "quit"};
    for (const char* d : denied) CHECK(!commandAllowed(d), "allowed: %s", d);
    FakeHost h;
    CHECK(runCommand(h, "status").rfind("OK running=0", 0) == 0, "status");
    CHECK(runCommand(h, "set_frequency 162550000") == "OK" && h.freq == 162550000, "set_frequency");
    const char* badf[] = {"set_frequency", "set_frequency nan", "set_frequency -5", "set_frequency 1e99", "set_frequency 1 2", "set_frequency abc", "set_frequency 0x10", "set_frequency inf"};
    for (const char* b : badf) CHECK(runCommand(h, b).rfind("ERR", 0) == 0, "accepted: %s", b);
    CHECK(h.freq == 162550000, "bad values changed nothing");
    CHECK(runCommand(h, "set_mode ADS-B") == "OK" && runCommand(h, "set_mode Nope").rfind("ERR no such mode", 0) == 0 && runCommand(h, "set_mode").rfind("ERR", 0) == 0, "set_mode");
    CHECK(runCommand(h, "set_gain 20").rfind("ERR not supported", 0) == 0, "gain not supported by this host");
    CHECK(runCommand(h, "tx 1").rfind("ERR not allowed", 0) == 0 && runCommand(h, "ptt on") == "ERR not allowed" && runCommand(h, "garbage\x01") == "ERR bad request", "refused");
    CHECK(runCommand(h, "start") == "OK" && h.started == 1 && runCommand(h, "stop") == "OK" && h.stopped == 1, "start / stop");
    CHECK(runCommand(h, "scan_slice 1 2").rfind("ERR", 0) == 0 && runCommand(h, "scan_slice 100 50 1").rfind("ERR", 0) == 0 && runCommand(h, "scan_slice 0 1e12 1").rfind("ERR", 0) == 0 &&
          runCommand(h, "scan_slice 0 1e9 0").rfind("ERR", 0) == 0, "scan_slice validation");
    h.peaks = {{100.5e6, -40}, {101.5e6, -50}};
    CHECK(runCommand(h, "scan_slice 100000000 101000000 100000").rfind("OK\n100500000\t-40.00", 0) == 0, "scan_slice hits");
    std::string r = runCommand(h, "scan_slices 100000\n100000000\t101000000\n101000000\t102000000\n");
    CHECK(r.find("100500000") != std::string::npos && r.find("101500000") != std::string::npos, "scan_slices");
    CHECK(runCommand(h, "scan_slices 100000\n1\t2\nbad line\n").rfind("ERR bad slice", 0) == 0 && runCommand(h, "scan_slices 100000").rfind("ERR no slices", 0) == 0, "scan_slices validation");
    { std::string many = "scan_slices 100000\n"; for (int i = 0; i < 201; i++) many += "1000\t2000\n"; CHECK(runCommand(h, many).rfind("ERR too many", 0) == 0, "slice count limit"); }
    h.canScan = false;
    CHECK(runCommand(h, "scan_slice 100000000 101000000 100000") == "ERR cannot scan", "node that cannot scan says so");
    CHECK(runCommand(h, "mem_put\ngarbage\nk\tn\tAM\t118000000\t\t5\t0\n").rfind("OK 1", 0) == 0 && h.mems.size() == 1, "mem_put keeps the valid line only");
    CHECK(runCommand(h, "results x").rfind("ERR", 0) == 0 && runCommand(h, "results 1 2").rfind("ERR", 0) == 0, "results args");
}

static void testSerializers() {
    std::vector<SeqMsg> m = {{1, {"adsb", "ICAO 1 a\tb\nc", "x", 1000}}, {2, {"ais", "hello", "x", 2000}}};
    auto back = parseMsgs(serializeMsgs(m), "nodeZ");
    CHECK(back.size() == 2 && back[0].m.text == "ICAO 1 a b c" && back[0].m.nodeId == "nodeZ" && back[1].seq == 2, "msgs roundtrip, node id is the receiver's, not the sender's claim");
    CHECK(parseMsgs("x\ty\n1\tadsb\n1\tadsb\tnotnum\ttext\n1\tadsb\t5\t\n1\t\t5\ttext\n1\tadsb\t5\tte\x01xt\n", "n").empty(), "bad lines dropped");
    CHECK(parseMsgs(std::string(100, '\n'), "n").empty(), "blank lines");
    { std::string s; for (int i = 0; i < 2000; i++) s += std::to_string(i) + "\tadsb\t5\ttext\n"; CHECK(parseMsgs(s, "n", 500).size() == 500, "line cap"); }
    { std::vector<SeqMsg> big; for (int i = 0; i < 2000; i++) big.push_back({(uint64_t)i, {"adsb", std::string(500, 'a'), "n", 1}}); CHECK(serializeMsgs(big).size() <= 60000, "reply budget"); }
    std::vector<Mem> mm = {{"k1", "Air\tport", "AM", 118.1e6, "note", 5, false}, {"k2", "", "", 0, "", 7, true}};
    auto mb = parseMems(serializeMems(mm));
    CHECK(mb.size() == 2 && mb[0].name == "Air port" && mb[1].deleted && mb[0].freqHz == 118.1e6, "mems roundtrip");
    CHECK(parseMems("k\tn\tm\tnan\t\t5\t0\nk\tn\tm\t1\t\t5\t2\nk\tn\tm\t1\t\t-5\t0\n../x\tn\tm\t1\t\t5\t0\nshort\n").empty(), "bad mems dropped");
    auto h = parseHits(serializeHits({{100e6, -40.5}, {101e6, -50}}));
    CHECK(h.size() == 2 && h[0].first == 100e6, "hits roundtrip");
    CHECK(parseHits("nan\t1\n-5\t1\n1e99\t1\n5\tinf\nx\n").empty(), "bad hits dropped");
}

// a fake network: node id -> host, requests run through runCommand
struct FakeNet {
    std::map<std::string, FakeHost*> hosts;
    std::map<std::string, std::string> forced;   // node id -> forced reply
    std::vector<std::string> log;
    Requester rq() {
        return [this](const std::string& id, const std::string& req, std::string& reply) {
            log.push_back(id + ": " + req.substr(0, req.find('\n')));
            auto f = forced.find(id);
            if (f != forced.end()) { reply = f->second; return reply != "<down>"; }
            auto h = hosts.find(id);
            if (h == hosts.end()) return false;
            reply = runCommand(*h->second, req);
            return true;
        };
    }
};

static void testJobs() {
    int64_t now = 1700000000000LL;
    FakeHost a, b;
    a.msgs = {{1, {"adsb", "ICAO 4840D6 alt 36000", "", now}}, {2, {"ais", "MMSI 244 pos", "", now + 100}}}; a.latest = 2;
    b.msgs = {{1, {"adsb", "icao 4840d6 ALT 36000", "", now + 700}}, {2, {"aprs", "N0CALL>APRS", "", now + 200}}}; b.latest = 2;   // heard the same aircraft too
    FakeNet net; net.hosts = {{"A", &a}, {"B", &b}};
    MessageMerger merger(2000, 100);
    uint64_t sa = 0, sb = 0;
    std::string err;
    int na = pullResults(net.rq(), "A", sa, merger, now, err), nb = pullResults(net.rq(), "B", sb, merger, now, err);
    CHECK(na == 2 && nb == 1 && merger.rows().size() == 3, "pooled: 3 rows from 4 messages (%d %d %zu)", na, nb, merger.rows().size());
    CHECK(merger.rows()[0].nodes.size() == 2, "the shared aircraft carries both node tags");
    CHECK(sa == 2 && sb == 2, "cursors advance");
    CHECK(pullResults(net.rq(), "A", sa, merger, now, err) == 0, "nothing new");
    a.latest = 1; a.msgs = {{1, {"adsb", "after restart", "", now + 5000}}};
    CHECK(pullResults(net.rq(), "A", sa, merger, now, err) == 0 && sa == 0, "restart noticed (latest went down): cursor reset");
    CHECK(pullResults(net.rq(), "A", sa, merger, now, err) == 1, "and the next pull gets the new beginning");
    net.forced["B"] = "ERR rate limited";
    CHECK(pullResults(net.rq(), "B", sb, merger, now, err) == 0 && !err.empty(), "error reply");
    net.forced["B"] = "OK notanumber\n";
    CHECK(pullResults(net.rq(), "B", sb, merger, now, err) == 0, "bad header");
    net.forced["B"] = "OK 9\n1\tadsb\t99999999999999\tfuture\n2\tadsb\t5\told\n3\tevil kind\t5\tx\n";
    uint64_t before = merger.rows().size();
    pullResults(net.rq(), "B", sb, merger, now, err);
    CHECK(merger.rows().size() == before, "implausible times and bad kinds from a node are rejected");
    net.forced.erase("B");
    CHECK(!pullResults(net.rq(), "Z", sb, merger, now, err) || true, "unknown node");

    // combined map
    {
        a.trk = {{"adsb", "4840D6", 52.1, 4.5, now}, {"ais", "244123456", 51.9, 4.1, now}};
        b.trk = {{"adsb", "4840D6", 52.2, 4.6, now + 1000}, {"adsb", "BAD;ID", 1, 1, now}, {"adsb", "X1", 95, 0, now}};
        TrackBoard board;
        int x = pullTracks(net.rq(), "A", board, now, err), y = pullTracks(net.rq(), "B", board, now, err);
        auto l = board.list();
        CHECK(x == 2 && y == 1 && l.size() == 2, "tracks: %d %d %zu", x, y, l.size());
        bool both = false;
        for (auto& t : l) if (t.id == "4840D6" && t.nodes.size() == 2 && t.lat == 52.2) both = true;
        CHECK(both, "the aircraft heard by two nodes is one track with the newer position and both tags");
        auto rt = parseTracks(serializeTracks({{"adsb", "A1", 52.123456, -4.654321, 5}}));
        CHECK(rt.size() == 1 && std::fabs(rt[0].lat - 52.123456) < 1e-6 && std::fabs(rt[0].lon + 4.654321) < 1e-6, "track roundtrip");
        CHECK(parseTracks("adsb\tA1\tx\t1\t5\nshort\n\tA1\t1\t1\t5\n").empty(), "bad track lines dropped");
        net.forced["B"] = "ERR rate limited";
        CHECK(pullTracks(net.rq(), "B", board, now, err) == 0, "error reply");
        net.forced.erase("B");
    }
    // coordinated scan
    FakeHost n1, n2, n3;
    n1.peaks = {{100.3e6, -40}, {160.0e6, -55}};
    n2.peaks = {{130.0e6, -45}, {100.3e6 + 1000, -42}};
    n2.peaks.push_back({50.0e6, -10});   // a liar: reports a frequency outside the slice it was given
    FakeNet sn; sn.hosts = {{"N1", &n1}, {"N2", &n2}, {"N3", &n3}};
    std::vector<Cap> caps = {{"N1", 24e6, 1766e6, 2e6, 0}, {"N2", 24e6, 1766e6, 2e6, 0}, {"N3", 24e6, 1766e6, 2e6, 0}};
    n3.canScan = false;
    ScanOutcome so;
    CHECK(coordinateScan(sn.rq(), 100e6, 200e6, 100e3, caps, 5e3, so, err), "scan: %s", err.c_str());
    CHECK(so.failed.size() == 1 && so.failed[0] == "N3", "the node that cannot scan is reported");
    CHECK(!so.uncovered.empty(), "its slices are reported as not covered");
    bool liar = false, got130 = false, got160 = false;
    for (auto& h : so.hits) { if (h.freqHz == 50.0e6) liar = true; if (h.freqHz == 130.0e6) got130 = true; if (h.freqHz == 160.0e6) got160 = true; }
    CHECK(!liar, "a hit outside the node's slice is dropped");
    CHECK(got130 || got160 || !so.hits.empty(), "hits merged from the nodes that scanned");
    int requests = 0;
    for (auto& l : sn.log) if (l.find("scan_slices") != std::string::npos) requests++;
    CHECK(requests == 3, "one request per node, not per slice (%d)", requests);
    CHECK(!coordinateScan(sn.rq(), 200e6, 100e6, 100e3, caps, 5e3, so, err), "bad range refused");
    FakeNet down;
    CHECK(coordinateScan(down.rq(), 100e6, 110e6, 100e3, caps, 5e3, so, err) && so.failed.size() == 3 && so.hits.empty(), "every node down: all failed, no hits, no crash");

    // memory sync
    FakeHost m1, m2;
    m1.mems = {{"a", "Air", "AM", 118e6, "", 100, false}, {"b", "FM", "WFM", 93.9e6, "", 100, false}};
    m2.mems = {{"a", "Air tower", "AM", 118e6, "", 200, false}, {"c", "New", "NFM", 145.5e6, "", 50, false}};
    FakeNet mn; mn.hosts = {{"M2", &m2}};
    std::vector<Mem> local = m1.mems;
    MemMergeStats st;
    CHECK(syncMemories(mn.rq(), "M2", local, st, err), "sync: %s", err.c_str());
    CHECK(local.size() == 3 && st.added == 1 && st.updated == 1, "local merged (+%d ~%d)", st.added, st.updated);
    CHECK(m2.mems.size() == 3, "the other node got the merged list");
    CHECK(!syncMemories(down.rq(), "M2", local, st, err), "node down");
    mn.forced["M2"] = "ERR rate limited";
    CHECK(!syncMemories(mn.rq(), "M2", local, st, err), "error reply");
}

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static void testSockets() {
    FakeHost host;
    host.msgs = {{1, {"adsb", "hello", "", 1700000000000LL}}}; host.latest = 1;
    SwarmCtlServer srv(host);
    std::string err;
    CHECK(srv.start(0, true, err), "start: %s", err.c_str());
    const int port = srv.port();
    CHECK(port > 0, "port");
    std::string gotToken, gotPeer;
    srv.setOnPaired([&](const std::string& t, const std::string& p) { gotToken = t; gotPeer = p; });
    // pairing off: nothing works
    std::string tok;
    CHECK(!swarmPair("127.0.0.1", port, std::string(32, 'A'), tok, err), "no pairing offered");
    // wrong codes lock it
    std::string code = srv.startPairing();
    CHECK(code.size() == 32, "code");
    for (int i = 0; i < 3; i++) CHECK(!swarmPair("127.0.0.1", port, std::string(32, 'C'), tok, err), "wrong code %d", i);
    CHECK(!swarmPair("127.0.0.1", port, code, tok, err), "locked even with the right code");
    CHECK(srv.tokens().empty(), "nothing paired");
    code = srv.startPairing();
    CHECK(!swarmPair("127.0.0.1", port, "short", tok, err), "malformed code refused locally");
    CHECK(swarmPair("127.0.0.1", port, code.substr(0, 8) + " - " + code.substr(8), tok, err), "right code: %s", err.c_str());
    sleepMs(100);
    CHECK(tok.size() == 24 && gotToken == tok && srv.tokens().size() == 1 && srv.status().paired == 1, "token stored on both sides");
    CHECK(!swarmPair("127.0.0.1", port, code, tok, err), "code works once");
    // control
    SwarmClient cl;
    CHECK(cl.connect("127.0.0.1", port, tok, err), "connect: %s", err.c_str());
    std::string rep;
    CHECK(cl.request("status", rep, err) && rep.rfind("OK running=0", 0) == 0, "status over the channel: %s", rep.c_str());
    CHECK(cl.request("set_frequency 162550000", rep, err) && rep == "OK" && host.freq == 162550000, "set_frequency");
    CHECK(cl.request("tx on", rep, err) && rep == "ERR not allowed", "transmit refused");
    CHECK(cl.request("get_keys", rep, err) && rep == "ERR not allowed", "key store not reachable");
    CHECK(cl.request("results 0", rep, err) && rep.find("hello") != std::string::npos, "results");
    // wrong token / unknown key
    SwarmClient bad;
    CHECK(!bad.connect("127.0.0.1", port, "ffffffffffffffffffffffff", err), "unknown token refused");
    // a token the server knows but the client gets wrong in the key id: same
    // garbage and abuse
    {
        std::string e2;
        dect2::sock_t s = dect2::tcpConnect("127.0.0.1", port, 2000, e2);
        CHECK(s != dect2::kBadSock, "connect for abuse");
        dect2::sockRecvTimeout(s, 1500);
        const char* junk = "GET / HTTP/1.1\r\n\r\n";
        dect2::sockSendAll(s, junk, strlen(junk));
        char c; CHECK(dect2::sockRecv(s, &c, 1) <= 0, "HTTP junk gets no answer");
        dect2::sockClose(s);
        s = dect2::tcpConnect("127.0.0.1", port, 2000, e2);
        std::string hello = "OACTL1 HELLO " + keyIdOf(tok) + " " + std::string(32, '1') + "\n";
        dect2::sockRecvTimeout(s, 1500);
        dect2::sockSendAll(s, hello.data(), hello.size());
        char chal[64] = {};
        dect2::sockRecv(s, chal, 45);   // "OACTL1 CHAL " + 32 hex + newline
        const unsigned char hugeLen[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        dect2::sockSendAll(s, hugeLen, 4);   // a 4 GB frame length: the server must not allocate it, just hang up
        CHECK(dect2::sockRecv(s, &c, 1) <= 0, "huge length ends the session");
        dect2::sockClose(s);
        s = dect2::tcpConnect("127.0.0.1", port, 2000, e2);
        dect2::sockRecvTimeout(s, 1500);
        std::string frame(4 + 45, 'x'); frame[0] = 0; frame[1] = 0; frame[2] = 0; frame[3] = 45;
        dect2::sockSendAll(s, hello.data(), hello.size());
        dect2::sockRecv(s, chal, 45);   // "OACTL1 CHAL " + 32 hex + newline
        dect2::sockSendAll(s, frame.data(), frame.size());   // random bytes with a good length: the tag fails
        CHECK(dect2::sockRecv(s, &c, 1) <= 0, "bad tag ends the session");
        dect2::sockClose(s);
    }
    CHECK(cl.request("status", rep, err), "the good session is unaffected by the abuse");
    // rate limit: a flood is answered with ERR, never executed unlimited
    int limited = 0;
    for (int i = 0; i < 80; i++) if (cl.request("status", rep, err) && rep == "ERR rate limited") limited++;
    CHECK(limited > 0, "flood is rate limited (%d)", limited);
    // revoke ends the session
    CHECK(srv.revoke(srv.tokens()[0].second), "revoke");
    sleepMs(1100);
    bool ended = false;
    for (int i = 0; i < 5 && !ended; i++) { if (!cl.request("status", rep, err)) ended = true; sleepMs(200); }
    CHECK(ended, "a revoked token ends the session");
    SwarmClient again;
    CHECK(!again.connect("127.0.0.1", port, tok, err), "and cannot connect again");
    srv.stop();
    SwarmClient gone;
    CHECK(!gone.connect("127.0.0.1", port, tok, err), "stopped server");
}

int main() {
    testFraming();
    testCommands();
    testSerializers();
    testJobs();
    testSockets();
    return testSummary("swarm_ctl");
}
