// Winlink leftovers of 3.9: resuming a message cut off part way, gzip proposals (FD), and peer-to-peer sessions between two OnAir
// stations. Everything runs in memory: B2F sessions joined by byte queues, and two stations over a simulated packet or ARDOP channel.
// No real gateway, RMS, CMS or radio is used.
#include "winlink_harness.h"
#include "dect2/common/inflate.h"
#include "dect2/winlink/lzhuf.h"
#include "dect2/winlink/wl_gzip.h"
#include <cstring>

using namespace wltest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> textBytes(size_t n, unsigned seed) {
    static const char* words[] = {"winlink ", "message ", "radio ", "the ", "station ", "gateway ", "LY2TST ", "73 ", "QTH ", "Vilnius\r\n"};
    std::mt19937 r(seed);
    std::vector<uint8_t> v;
    while (v.size() < n) { const char* w = words[r() % 10]; v.insert(v.end(), w, w + strlen(w)); }
    v.resize(n);
    return v;
}

// ---- gzip ------------------------------------------------------------------------------------------------------------------------------
static void testGzip() {
    std::mt19937 rng(5);
    std::vector<std::pair<const char*, std::vector<uint8_t>>> cases;
    cases.push_back({"empty", {}});
    cases.push_back({"one byte", {'x'}});
    cases.push_back({"5000 spaces", std::vector<uint8_t>(5000, ' ')});
    cases.push_back({"70000 identical", std::vector<uint8_t>(70000, 0xAA)});
    std::vector<uint8_t> rnd(30000);
    for (auto& b : rnd) b = (uint8_t)rng();
    cases.push_back({"30 kB random", rnd});
    cases.push_back({"200 kB text", textBytes(200000, 1)});
    cases.push_back({"1548 bytes of text", textBytes(1548, 2)});
    for (auto& c : cases) {
        const std::vector<uint8_t> z = gzipCompress(c.second);
        std::vector<uint8_t> back;
        const bool ok = dect2::gunzip(z.data(), z.size(), back);
        const size_t lz = lzhufCompress(c.second, true).size();
        printf("gzip %-18s %7zu -> %7zu bytes (LZHUF %zu)\n", c.first, c.second.size(), z.size(), lz);
        CHECK(ok && back == c.second, "gzip round trip: %s", c.first);
        CHECK(z.size() >= 18 && z[0] == 0x1F && z[1] == 0x8B && z[2] == 8, "gzip header: %s", c.first);
    }
    // damage is caught by the CRC-32 or the deflate decoder
    std::vector<uint8_t> z = gzipCompress(textBytes(4000, 3));
    int caught = 0;
    for (size_t i = 10; i < z.size(); i += 7) {
        std::vector<uint8_t> bad = z, out;
        bad[i] ^= 0x10;
        if (!dect2::gunzip(bad.data(), bad.size(), out)) caught++;
    }
    CHECK(caught >= (int)((z.size() - 10) / 7) - 1, "damaged gzip refused (%d)", caught);
}

// ---- B2F sessions over byte queues -------------------------------------------------------------------------------------------------------
struct Pipe {
    std::vector<uint8_t> toClient, toGw;
    size_t cutAfter = SIZE_MAX;      // gateway -> client bytes delivered before the link "drops"
    size_t deliveredToClient = 0;
    bool cut = false;
};

struct ClientSide {
    std::map<std::string, std::vector<uint8_t>> held;    // the station's partial files
    std::vector<B2Message> got;
    std::vector<int> offsets;
    std::vector<std::string> trace;
    std::set<std::string> sentMids;
    std::unique_ptr<B2fSession> s;
    void begin(Pipe& p, bool gzip, std::vector<B2Message> out = {}) {
        B2fSession::Config c;
        c.role = B2fSession::Role::Client;
        c.mycall = kMyCall; c.targetCall = "CMS"; c.locator = "KO24"; c.password = kPassword; c.gzip = gzip;
        B2fSession::Hooks h;
        h.send = [&p](const uint8_t* d, size_t n) { p.toGw.insert(p.toGw.end(), d, d + n); };
        h.outbound = [this, out]() {
            std::vector<B2Proposal> v;
            for (auto& m : out) if (!sentMids.count(m.mid())) v.push_back(makeProposal(m, s->gzipAgreed() ? 'D' : 'C'));
            return v;
        };
        h.sent = [this](const std::string& mid, bool) { sentMids.insert(mid); };
        h.received = [this](const B2Message& m, const B2Proposal& pr) { got.push_back(m); offsets.push_back(pr.offset); held.erase(pr.mid); };
        h.resumeData = [this](const B2Proposal& pr) { auto it = held.find(pr.mid); return it == held.end() ? std::vector<uint8_t>() : it->second; };
        h.partial = [this](const B2Proposal& pr, const std::vector<uint8_t>& b) { if (b.empty()) held.erase(pr.mid); else held[pr.mid] = b; };
        h.log = [this](const std::string& l) { trace.push_back(l); };
        s = std::make_unique<B2fSession>(c, h);
    }
};

// runs until both sides end or the cut; chunk: bytes handed over at a time
static void pump(Pipe& p, ClientSide& cl, MockGateway& gw, size_t chunk) {
    for (int guard = 0; guard < 200000; guard++) {
        bool moved = false;
        if (!p.toClient.empty() && !p.cut) {
            size_t n = std::min(chunk, p.toClient.size());
            if (p.deliveredToClient + n > p.cutAfter) n = p.cutAfter - p.deliveredToClient;
            if (n > 0) {
                std::vector<uint8_t> b(p.toClient.begin(), p.toClient.begin() + (long)n);
                p.toClient.erase(p.toClient.begin(), p.toClient.begin() + (long)n);
                p.deliveredToClient += n;
                cl.s->onBytes(b.data(), b.size());
                moved = true;
            }
            if (p.deliveredToClient >= p.cutAfter) { p.cut = true; cl.s->linkClosed(); gw.s->linkClosed(); return; }
        }
        if (!p.toGw.empty()) {
            const size_t n = std::min(chunk, p.toGw.size());
            std::vector<uint8_t> b(p.toGw.begin(), p.toGw.begin() + (long)n);
            p.toGw.erase(p.toGw.begin(), p.toGw.begin() + (long)n);
            gw.s->onBytes(b.data(), b.size());
            moved = true;
        }
        if (!moved) break;
    }
}

static std::vector<B2Message> gatewayMail() {
    const std::time_t t0 = 1700000000;
    std::vector<B2Message> v;
    v.push_back(msg("LY1GW", kMyCall, "short", "hello\r\n", t0));
    B2File f;
    f.name = "log.txt";
    const std::vector<uint8_t> big = textBytes(20000, 9);
    f.data = big;
    v.push_back(msg("LY1GW", kMyCall, "with a file", "see the file\r\n", t0 + 1, {f}));
    v.push_back(msg("LY1GW", kMyCall, "third", std::string(3000, 'q'), t0 + 2));
    return v;
}

static void testResume() {
    const std::vector<B2Message> mail = gatewayMail();
    // the compressed size of the big one, to place the cuts inside it
    const B2Proposal big = makeProposal(mail[1]);
    int resumedOk = 0, trials = 0;
    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)125, (size_t)4096}) {
        for (int k = 1; k <= 6; k++) {
            trials++;
            ClientSide cl;
            // first session: find where message 2's data starts on the wire, then cut part way into it
            Pipe probe;
            MockGateway g0; g0.waiting = mail;
            cl.begin(probe, false);
            g0.begin([&probe](const uint8_t* d, size_t n) { probe.toClient.insert(probe.toClient.end(), d, d + n); });
            g0.s->start(); cl.s->start();
            pump(probe, cl, g0, 1 << 20);
            CHECK(cl.got.size() == 3 && cl.s->done(), "uncut session (%zu)", cl.got.size());
            const size_t total = probe.deliveredToClient;
            // the big message is most of the stream: cut at k/7 of it
            Pipe p;
            p.cutAfter = total - (size_t)big.csize + (size_t)big.csize * (size_t)k / 7;
            ClientSide c1;
            MockGateway g1; g1.waiting = mail;
            c1.begin(p, false);
            g1.begin([&p](const uint8_t* d, size_t n) { p.toClient.insert(p.toClient.end(), d, d + n); });
            g1.s->start(); c1.s->start();
            pump(p, c1, g1, chunk);
            const size_t heldN = c1.held.count(big.mid) ? c1.held[big.mid].size() : 0;
            CHECK(c1.s->failed() && c1.got.size() == 1 && heldN > 0 && heldN < (size_t)big.csize, "chunk %zu cut %d: first message in, part of the second held (%zu)", chunk, k, heldN);
            // second session: the gateway still has messages 2 and 3; the client asks for the rest of 2
            Pipe p2;
            MockGateway g2; g2.waiting = {mail[1], mail[2]};
            ClientSide c2;
            c2.held = c1.held;
            c2.begin(p2, false);
            g2.begin([&p2](const uint8_t* d, size_t n) { p2.toClient.insert(p2.toClient.end(), d, d + n); });
            g2.s->start(); c2.s->start();
            pump(p2, c2, g2, chunk);
            bool ok = c2.s->done() && c2.got.size() == 2 && c2.offsets.size() == 2 && c2.offsets[0] == (int)heldN && c2.held.empty();
            ok = ok && c2.got[0].files.size() == 1 && c2.got[0].files[0].data == mail[1].files[0].data && c2.got[0].mid() == mail[1].mid();
            bool askedOffset = false;
            for (const auto& l : c2.trace) askedOffset = askedOffset || (l.rfind(">FS !", 0) == 0);
            CHECK(ok && askedOffset, "chunk %zu cut %d: resumed from %zu, message complete (%s)", chunk, k, heldN, c2.s->error().c_str());
            if (ok) resumedOk++;
            if (chunk == 125 && k == 3) {
                const uint64_t whole = probe.deliveredToClient, second = p2.deliveredToClient;
                printf("resume: cut after %zu of %d bytes; the second session moved %llu bytes to the client (the uncut session %llu)\n", heldN, big.csize,
                       (unsigned long long)second, (unsigned long long)whole);
            }
        }
    }
    printf("resume: %d of %d cut sessions completed from the held part\n", resumedOk, trials);

    // a held part that does not belong to the message (damaged file): the message fails, the part is dropped, the next try takes it whole
    {
        Pipe p;
        MockGateway g; g.waiting = {mail[1]};
        ClientSide c;
        std::vector<uint8_t> bogus = big.cdata;
        bogus.resize((size_t)big.csize / 2);
        for (size_t i = 10; i < bogus.size(); i += 3) bogus[i] ^= 0x5A;
        c.held[big.mid] = bogus;
        c.begin(p, false);
        g.begin([&p](const uint8_t* d, size_t n) { p.toClient.insert(p.toClient.end(), d, d + n); });
        g.s->start(); c.s->start();
        pump(p, c, g, 125);
        CHECK(c.s->failed() && c.held.empty() && c.s->error().find("held part was dropped") != std::string::npos, "a bad held part is dropped: %s", c.s->error().c_str());
        Pipe p2;
        MockGateway g2; g2.waiting = {mail[1]};
        ClientSide c2;
        c2.begin(p2, false);
        g2.begin([&p2](const uint8_t* d, size_t n) { p2.toClient.insert(p2.toClient.end(), d, d + n); });
        g2.s->start(); c2.s->start();
        pump(p2, c2, g2, 125);
        CHECK(c2.s->done() && c2.got.size() == 1 && c2.offsets[0] == 0, "then it comes whole");
    }
}

// A sender in the FBB version 1 way: on a resume it sends the 6 header bytes (CRC and length) again before the data from the offset.
// The gateway side is written out by hand here.
static void testResumeFbbStyle() {
    const B2Message m = gatewayMail()[1];
    const B2Proposal pr = makeProposal(m);
    for (int style = 0; style < 2; style++) {
        const int off = pr.csize / 3;
        ClientSide c;
        c.held[pr.mid] = std::vector<uint8_t>(pr.cdata.begin(), pr.cdata.begin() + off);
        Pipe p;
        c.begin(p, false);
        c.s->start();
        auto give = [&](const std::string& s) { c.s->onBytes((const uint8_t*)s.data(), s.size()); };
        give("[WL2K-5.0-B2FWIHJM$]\r;PQ: " + std::string(kChallenge) + "\rCMS>\r");
        // the client offers nothing: "FF"; now the gateway proposes
        const std::string line = proposalLine(pr);
        char sum[16];
        snprintf(sum, sizeof sum, "F> %02X", proposalChecksum({line}));
        give(line + "\r" + sum + "\r");
        const std::string answered(p.toGw.begin(), p.toGw.end());
        CHECK(answered.find("FS !" + std::to_string(off) + "\r") != std::string::npos, "the client asks for the rest: %s", answered.c_str());
        std::vector<uint8_t> data;
        if (style == 1) data.assign(pr.cdata.begin(), pr.cdata.begin() + 6);
        data.insert(data.end(), pr.cdata.begin() + off, pr.cdata.end());
        std::string blk;
        const std::string title = pr.title, o = std::to_string(off);
        blk += (char)1; blk += (char)(title.size() + o.size() + 2); blk += title; blk += '\0'; blk += o; blk += '\0';
        long s = 0;
        for (size_t i = 0; i < data.size(); i += 250) {
            const size_t n = std::min((size_t)250, data.size() - i);
            blk += (char)2; blk += (char)n;
            blk.append((const char*)data.data() + i, n);
            for (size_t k = 0; k < n; k++) s += data[i + k];
        }
        blk += (char)4; blk += (char)((-s) & 0xFF);
        give(blk);
        CHECK(c.got.size() == 1 && c.got[0].files.size() == 1 && c.got[0].files[0].data == m.files[0].data, "%s resume decoded (%s)",
              style ? "FBB v1 (header again)" : "wl2k-go (data from the offset)", c.s->error().c_str());
    }
}

static void testGzipSession() {
    const std::vector<B2Message> mail = gatewayMail();
    struct Case { bool cl, gw; const char* name; };
    for (const Case& k : {Case{true, true, "both gzip"}, Case{true, false, "client only"}, Case{false, true, "gateway only"}}) {
        Pipe p;
        MockGateway g; g.waiting = mail;
        ClientSide c;
        const std::time_t t0 = 1700001000;
        std::vector<B2Message> mine = {msg(kMyCall, "friend@example.com", "out gz", std::string(5000, 'z') + "end", t0)};
        c.begin(p, k.cl, mine);
        // the mock gateway with gzip: its SID gets "G" and it proposes FD when the client offers it
        B2fSession::Config gc;
        gc.role = B2fSession::Role::Gateway; gc.mycall = "CMS"; gc.targetCall = kMyCall; gc.challenge = kChallenge; gc.expectPassword = kPassword;
        gc.gzip = k.gw;
        B2fSession::Hooks gh;
        gh.send = [&p](const uint8_t* d, size_t n) { p.toClient.insert(p.toClient.end(), d, d + n); };
        B2fSession* gs = nullptr;
        std::set<std::string> done;
        std::vector<char> codesOut, codesIn;
        gh.outbound = [&]() { std::vector<B2Proposal> v; for (auto& m : mail) if (!done.count(m.mid())) { v.push_back(makeProposal(m, gs->gzipAgreed() ? 'D' : 'C')); codesOut.push_back(v.back().code); } return v; };
        gh.sent = [&](const std::string& mid, bool) { done.insert(mid); };
        gh.received = [&](const B2Message& m, const B2Proposal& pr) { g.got.push_back(m); codesIn.push_back(pr.code); };
        g.s = std::make_unique<B2fSession>(gc, gh);
        gs = g.s.get();
        g.s->start(); c.s->start();
        pump(p, c, g, 125);
        const bool agreed = k.cl && k.gw;
        CHECK(c.s->done() && c.got.size() == 3 && g.got.size() == 1, "%s: session (%s / %s), state %d, %zu in, %zu out", k.name, c.s->error().c_str(), g.s->error().c_str(),
              (int)c.s->state(), c.got.size(), g.got.size());
        if (!c.s->done()) for (auto& l : c.trace) printf("   %s\n", l.substr(0, 100).c_str());
        CHECK(c.s->gzipAgreed() == agreed && g.s->gzipAgreed() == agreed, "%s: agreement", k.name);
        bool allD = !codesOut.empty(), allC = !codesOut.empty();
        for (char x : codesOut) { allD = allD && x == 'D'; allC = allC && x == 'C'; }
        CHECK(agreed ? allD : allC, "%s: the gateway proposes %s", k.name, agreed ? "FD" : "FC");
        CHECK(!codesIn.empty() && codesIn[0] == (agreed ? 'D' : 'C'), "%s: the client proposes %s", k.name, agreed ? "FD" : "FC");
        if (c.got.size() == 3) CHECK(c.got[1].files.size() == 1 && c.got[1].files[0].data == mail[1].files[0].data, "%s: attachment intact", k.name);
    }
    // an FD proposal to a station that did not offer gzip is deferred, not taken
    {
        ClientSide c;
        Pipe p;
        c.begin(p, false);
        c.s->start();
        auto give = [&](const std::string& s) { c.s->onBytes((const uint8_t*)s.data(), s.size()); };
        give("[WL2K-5.0-B2FWIHJMG$]\r;PQ: " + std::string(kChallenge) + "\rCMS>\r");
        const B2Proposal pr = makeProposal(gatewayMail()[0], 'D');
        const std::string line = proposalLine(pr);
        char sum[16];
        snprintf(sum, sizeof sum, "F> %02X", proposalChecksum({line}));
        give(line + "\r" + sum + "\r");
        const std::string answered(p.toGw.begin(), p.toGw.end());
        CHECK(answered.find("FS =\r") != std::string::npos && line.rfind("FD EM ", 0) == 0, "FD deferred without gzip: %s", answered.c_str());
    }
}

// ---- two OnAir stations, peer to peer -------------------------------------------------------------------------------------------------
template <class FrameT> struct P2P {
    std::string dirA, dirB;
    Mailbox mbA, mbB;
    WinlinkStation a, b;
    Air<FrameT> air;
    double now = 0;
    bool silenced = false;
    P2P() : dirA(tempDir("p2pA")), dirB(tempDir("p2pB")), mbA(dirA), mbB(dirB), a(mbA), b(mbB) {}
    ~P2P() { std::error_code ec; std::filesystem::remove_all(dirA, ec); std::filesystem::remove_all(dirB, ec); }
    void step(double dt);
    // runs until A's session ends; stopAt: B's received bytes at which the channel goes silent (the link then fails)
    void run(double limit, size_t silenceAtBytes = 0) {
        for (double end = now + limit; now < end && a.running(); now += 0.05) {
            if (silenceAtBytes && !silenced && b.session() && b.session()->state() == B2fSession::State::Receiving && b.session()->rxBytes() >= silenceAtBytes) silenced = true;
            step(0.05);
        }
        for (int i = 0; i < 40; i++, now += 0.05) step(0.05);   // let B see the end
    }
};
template <> void P2P<ax25::Frame>::step(double) {
    for (auto& f : a.takeAx25()) if (!silenced) air.send(air.toGw, f, now);
    for (auto& f : b.takeAx25()) if (!silenced) air.send(air.toClient, f, now);
    air.deliver(air.toGw, now, [&](const ax25::Frame& f) { b.rxAx25(f, now); });
    air.deliver(air.toClient, now, [&](const ax25::Frame& f) { a.rxAx25(f, now); });
    a.tick(now); b.tick(now);
}
template <> void P2P<ArdopLinkFrame>::step(double) {
    for (auto& f : a.takeArdop()) if (!silenced) air.send(air.toGw, f, now);
    for (auto& f : b.takeArdop()) if (!silenced) air.send(air.toClient, f, now);
    air.deliver(air.toGw, now, [&](const ArdopLinkFrame& f) { b.rxArdop(toGwIn(f), now); });
    air.deliver(air.toClient, now, [&](const ArdopLinkFrame& f) { a.rxArdop(toGwIn(f), now); });
    a.tick(now); b.tick(now);
}

template <class FrameT> static void p2pCase(Transport tr, double loss, bool gzip, const char* name) {
    P2P<FrameT> t;
    t.air.loss = loss;
    t.air.rng.seed(77);
    const std::time_t t0 = 1700002000;
    const std::string A = "LY1AAA", B = "LY2BBB-7";
    // A: two for B (one to the plain call, one with @winlink.org), one for somebody else
    B2Message forB1 = msg(A, "LY2BBB", "for B", "hello B\r\n" + std::string(600, 'b'), t0);
    B2Message forB2 = msg(A, "ly2bbb@winlink.org", "for B again", "second\r\n", t0 + 1);
    B2Message notB = msg(A, "LY9ZZZ", "not for B", "stays\r\n", t0 + 2);
    for (auto* m : {&forB1, &forB2, &notB}) t.mbA.save(Folder::Outbox, *m);
    B2Message forA = msg(B, "LY1AAA-2", "for A", "hello A\r\n" + std::string(900, 'a'), t0 + 3);
    B2Message inet = msg(B, "someone@example.com", "internet", "stays too\r\n", t0 + 4);
    for (auto* m : {&forA, &inet}) t.mbB.save(Folder::Outbox, *m);
    StationConfig cb;
    cb.mycall = B; cb.gateway.transport = tr; cb.gzip = gzip; cb.t1Sec = 4;
    std::string err;
    CHECK(t.b.listen(cb, t.now, err) && t.b.listening(), "%s: B listens (%s)", name, err.c_str());
    StationConfig ca;
    ca.mycall = A; ca.gateway.call = B; ca.gateway.transport = tr; ca.p2p = true; ca.gzip = gzip; ca.t1Sec = 4;
    ca.password = "must-not-be-sent";
    CHECK(t.a.start(ca, t.now, err), "%s: A calls B (%s)", name, err.c_str());
    std::vector<std::string> logA, logB;
    t.run(3000);
    for (auto& l : t.a.takeLog()) logA.push_back(l);
    for (auto& l : t.b.takeLog()) logB.push_back(l);
    B2Message got;
    const bool aOk = t.a.lastOk() && t.a.received() == 1 && t.a.sent() == 2;
    CHECK(aOk, "%s: A's session: %s, %d in, %d out", name, t.a.lastError().c_str(), t.a.received(), t.a.sent());
    CHECK(t.mbB.load(Folder::Inbox, forB1.mid(), got) && got.body == forB1.body && t.mbB.load(Folder::Inbox, forB2.mid(), got), "%s: B got both", name);
    CHECK(t.mbA.load(Folder::Inbox, forA.mid(), got) && got.body == forA.body, "%s: A got B's message", name);
    CHECK(t.mbA.list(Folder::Outbox).size() == 1 && t.mbA.list(Folder::Outbox)[0].mid == notB.mid(), "%s: the message for somebody else stays in A's outbox", name);
    CHECK(t.mbB.list(Folder::Outbox).size() == 1 && t.mbB.list(Folder::Outbox)[0].mid == inet.mid(), "%s: B's internet message stays", name);
    CHECK(t.b.listening(), "%s: B listens again after the session", name);
    bool sawFD = false, sawPQ = false, sawPR = false, sawConnected = false;
    for (auto& l : logA) { sawFD = sawFD || l.rfind(">FD ", 0) == 0; sawPR = sawPR || l.find(";PR") != std::string::npos; }
    for (auto& l : logB) { sawPQ = sawPQ || l.find(";PQ") != std::string::npos; sawConnected = sawConnected || l.find("connected by LY1AAA") != std::string::npos; }
    CHECK(sawFD == gzip, "%s: gzip proposals %s", name, gzip ? "used" : "not used");
    CHECK(!sawPQ && !sawPR, "%s: no secure login between peers", name);
    CHECK(sawConnected, "%s: B logs who called", name);
    printf("P2P %s: %s, %.0f s simulated, %llu frames lost\n", name, aOk ? "ok" : "FAILED", t.now, (unsigned long long)t.air.dropped);
    t.b.stop(t.now);
    CHECK(!t.b.running(), "%s: B stops listening", name);
}

// A long message from A to B cut off by a dead channel part way, then a second call: B asks for the rest only
static void p2pResume() {
    P2P<ax25::Frame> t;
    const std::time_t t0 = 1700003000;
    const std::string A = "LY1AAA", B = "LY2BBB";
    B2File f;
    f.name = "big.txt";
    f.data = textBytes(12000, 4);
    for (auto& c : f.data) c = (uint8_t)(c ^ (uint8_t)(&c - f.data.data()));   // less compressible
    B2Message big = msg(A, B, "big", "attached\r\n", t0, {f});
    t.mbA.save(Folder::Outbox, big);
    const B2Proposal pr = makeProposal(big);
    StationConfig cb; cb.mycall = B; cb.gateway.transport = Transport::Packet9600; cb.t1Sec = 3; cb.t3Sec = 20;
    StationConfig ca; ca.mycall = A; ca.gateway.call = B; ca.gateway.transport = Transport::Packet9600; ca.p2p = true; ca.t1Sec = 3; ca.t3Sec = 20;
    std::string err;
    CHECK(t.b.listen(cb, t.now, err) && t.a.start(ca, t.now, err), "start (%s)", err.c_str());
    t.run(3000, (size_t)pr.csize / 2);
    // the silent channel: both links give up after their retries
    for (int i = 0; i < 4000 && (t.b.session() && !t.b.listening()); i++, t.now += 0.05) t.step(0.05);
    const std::vector<std::string> parts = t.mbB.partials();
    CHECK(!t.a.lastOk() && parts.size() == 1 && parts[0] == big.mid(), "the cut message is held by B (%zu parts)", parts.size());
    CHECK(t.mbA.list(Folder::Outbox).size() == 1, "and stays in A's outbox");
    CHECK(t.b.listening(), "B listens again");
    t.silenced = false;
    CHECK(t.a.start(ca, t.now, err), "second call (%s)", err.c_str());
    std::vector<std::string> logB;
    for (double end = t.now + 3000; t.now < end && t.a.running(); t.now += 0.05) { t.step(0.05); for (auto& l : t.b.takeLog()) logB.push_back(l); }
    for (int i = 0; i < 40; i++, t.now += 0.05) { t.step(0.05); for (auto& l : t.b.takeLog()) logB.push_back(l); }
    B2Message got;
    bool resumed = false;
    for (auto& l : logB) resumed = resumed || l.find("resumed from byte") != std::string::npos;
    CHECK(t.a.lastOk() && t.mbB.load(Folder::Inbox, big.mid(), got) && got.files.size() == 1 && got.files[0].data == f.data, "the resumed message is whole (%s)", t.a.lastError().c_str());
    CHECK(resumed && t.mbB.partials().empty(), "B resumed and dropped the held part");
    printf("P2P resume over AX.25 9600: the second call finished at %.0f s simulated\n", t.now);
}

static void testMailboxPartials() {
    const std::string dir = tempDir("part");
    Mailbox mb(dir);
    B2Proposal p;
    p.mid = "ABCDEF123456"; p.code = 'C'; p.size = 1000; p.csize = 600;
    const std::vector<uint8_t> h(300, 7);
    CHECK(mb.savePartial(p, h) && mb.loadPartial(p) == h && mb.partials().size() == 1, "partial round trip");
    B2Proposal other = p;
    other.csize = 601;
    CHECK(mb.loadPartial(other).empty() && mb.partials().empty(), "a partial of another proposal is dropped");
    B2Proposal evil = p;
    evil.mid = "../../etc";
    CHECK(!mb.savePartial(evil, h) && mb.loadPartial(evil).empty(), "bad message IDs refused");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static void testAddressed() {
    const B2Message m = makeMessage("LY1AAA", {"ly2bbb-7", "x@example.com"}, {"LY3CCC@winlink.org"}, "s", "b", {}, 1700000000);
    CHECK(addressedTo(m, "LY2BBB") && addressedTo(m, "LY2BBB-3") && addressedTo(m, "LY3CCC"), "calls with any SSID and @winlink.org");
    CHECK(!addressedTo(m, "LY4DDD") && !addressedTo(m, "x") && !addressedTo(m, ""), "others are not");
    Gateway g; g.call = "LY2BBB-7"; g.transport = Transport::Ardop; g.mhz = 3.585; g.peer = true;
    Gateway h; h.call = "LY0GW-10"; h.transport = Transport::Packet1200;
    const std::vector<Gateway> back = loadGateways(saveGateways({g, h}));
    CHECK(back.size() == 2 && back[0].peer && !back[1].peer && back[0].call == "LY2BBB-7", "the peer flag is kept in the list");
    CHECK(loadGateways("LY0GW|0|144.8|||8772|old line\n").size() == 1 && !loadGateways("LY0GW|0|144.8|||8772|old line\n")[0].peer, "a 3.9.0 list still loads");
}

int main() {
    testGzip();
    testMailboxPartials();
    testAddressed();
    testResume();
    testResumeFbbStyle();
    testGzipSession();
    p2pCase<ax25::Frame>(Transport::Packet1200, 0.0, false, "AX.25 1200, clean");
    p2pCase<ax25::Frame>(Transport::Packet9600, 0.15, true, "AX.25 9600, 15% loss, gzip");
    p2pCase<ArdopLinkFrame>(Transport::Ardop, 0.1, false, "ARDOP, 10% loss");
    p2pResume();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("winlink_p2p: all passed\n");
    return 0;
}
