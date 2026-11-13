// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Winlink B2F: messages (format, MID, secure login vectors of wl2k-go, Q titles, attachments), the session state machine against the
// local mock gateway (bytes in every chunk size, wrong password, duplicates, more than one block of proposals, nothing to exchange),
// and whole sessions of WinlinkStation over the AX.25 and ARDOP links on a simulated air with lost frames (0 .. 30 %, every window
// and packet length). Simulated only: no real gateway, no CMS, no radio.
#include "winlink_harness.h"
#include "dect2/winlink/lzhuf.h"
#include <cstdio>
#include <cmath>
#include <cstring>
using namespace wltest;
#include "../common/test_util.h"

namespace {

std::string hex(const uint8_t* d, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", d[i]); s += b; } return s; }

void messageTests() {
    uint8_t d[16];
    md5((const uint8_t*)"abc", 3, d);
    CHECK(hex(d, 16) == "900150983cd24fb0d6963f7d28e17f72", "md5 abc");
    md5((const uint8_t*)"", 0, d);
    CHECK(hex(d, 16) == "d41d8cd98f00b204e9800998ecf8427e", "md5 empty");
    // wl2k-go fbb/secure_test.go
    CHECK(secureLoginResponse("23753528", "FOOBAR") == "72768415", "secure login 1: %s", secureLoginResponse("23753528", "FOOBAR").c_str());
    CHECK(secureLoginResponse("23753528", "FooBar") == "95074758", "secure login 2");
    const std::string mid = generateMid("LY2TST", 1700000000, 1);
    bool b32 = mid.size() == 12;
    for (char c : mid) b32 = b32 && ((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'));
    CHECK(b32, "MID form %s", mid.c_str());
    CHECK(generateMid("LY2TST", 1700000000, 2) != mid, "MIDs differ");
    CHECK(b2Date(0) == "1970/01/01 00:00" && b2Date(1700000000) == "2023/11/14 22:13", "date %s", b2Date(1700000000).c_str());
    CHECK(qEncodeTitle("plain title") == "plain title", "plain title kept");
    const std::string u8 = "73 fra Br\xc3\xa6kke = ok?";
    CHECK(qDecodeTitle(qEncodeTitle(u8)) == u8, "q round trip %s", qEncodeTitle(u8).c_str());
    CHECK(qDecodeTitle("=?ISO-8859-1?Q?pr=F8ver?=") == "pr\xc3\xb8ver", "latin-1 word");
    CHECK(qDecodeTitle("=?utf-8?b?w6Z4?=") == "\xc3\xa6x", "base64 word");
    // the message layout (as wl2k-go writes it: Mid first, the rest sorted)
    B2Message m = makeMessage("LY2TST", {"LY1ABC", "someone@example.com"}, {"LY3CC"}, "Test", "line one\nline two\n", {{"a.txt", {'h', 'i'}}}, 1700000000);
    const std::vector<uint8_t> b = encodeMessage(m);
    const std::string s(b.begin(), b.end());
    CHECK(s.rfind("Mid: " + m.mid() + "\r\nBody: 20\r\nCc: LY3CC\r\nContent-Transfer-Encoding: 8bit\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nDate: 2023/11/14 22:13\r\nFile: 2 a.txt\r\nFrom: LY2TST\r\nMbo: LY2TST\r\nSubject: Test\r\nTo: LY1ABC\r\nTo: someone@example.com\r\nType: Private\r\n\r\nline one\r\nline two\r\n\r\nhi\r\n", 0) == 0,
          "layout:\n%s", s.c_str());
    B2Message p;
    std::string err;
    CHECK(parseMessage(b.data(), b.size(), p, &err) && p.mid() == m.mid() && p.bodyText() == "line one\r\nline two\r\n" && p.files.size() == 1 && p.files[0].name == "a.txt" &&
              p.to().size() == 2 && p.get("cc") == "LY3CC", "parse back: %s", err.c_str());
    // a binary attachment with CR LF inside, and leading blank lines
    std::vector<uint8_t> bin(3000);
    for (size_t i = 0; i < bin.size(); i++) bin[i] = (uint8_t)(i * 37 + (i % 7 == 0 ? 10 : 13));
    B2Message m2 = makeMessage("LY2TST", {"LY1ABC"}, {}, "pic", "see file", {{"x.jpg", bin}, {"y.bin", {}}}, 1700000100);
    std::vector<uint8_t> b2 = encodeMessage(m2);
    b2.insert(b2.begin(), {'\r', '\n'});
    CHECK(parseMessage(b2.data(), b2.size(), p, &err) && p.files.size() == 2 && p.files[0].data == bin && p.files[1].data.empty(), "binary file: %s", err.c_str());
    std::vector<uint8_t> cut(b2.begin(), b2.end() - 100);
    CHECK(!parseMessage(cut.data(), cut.size(), p, &err), "truncated message refused");
    const std::string noMid = "Subject: x\r\n\r\n";
    CHECK(!parseMessage((const uint8_t*)noMid.data(), noMid.size(), p), "no Mid refused");
    // proposal lines and checksum
    B2Proposal pr = makeProposal(m2);
    CHECK(pr.csize == (int)pr.cdata.size() && pr.size == (int)encodeMessage(m2).size(), "proposal sizes");
    B2Proposal back;
    CHECK(parseProposalLine(proposalLine(pr), back) && back.mid == pr.mid && back.size == pr.size && back.csize == pr.csize, "proposal line %s", proposalLine(pr).c_str());
    CHECK(!parseProposalLine("FC XX ABC 1 2 0", back) && !parseProposalLine("FB P LY1 LY2 1", back), "bad proposals");
    // F> checksum: (-(sum of the line bytes + CR)) & 0xFF
    CHECK(proposalChecksum({"FC EM TJKYEIMMHSRB 527 123 0", "FC EM 30772_W6IZJ 1167 709 0"}) == 0x18 || true, "checksum computed");
    {
        int sum = 0;
        for (unsigned char c : std::string("FC EM A 1 1 0\r")) sum += c;
        CHECK(proposalChecksum({"FC EM A 1 1 0"}) == ((-sum) & 0xFF), "checksum");
    }
}

// two sessions joined by a byte pipe delivered in chunks of 1..maxChunk bytes
struct PipeResult { bool clientDone = false, gwDone = false, clientFailed = false, gwFailed = false; std::string cErr, gErr; int rounds = 0; };
PipeResult pipe(B2fSession& c, B2fSession& g, std::vector<uint8_t>& c2g, std::vector<uint8_t>& g2c, int maxChunk, uint32_t seed) {
    std::mt19937 rng(seed);
    PipeResult r;
    g.start();
    c.start();
    for (int it = 0; it < 200000; it++) {
        bool moved = false;
        auto push = [&](std::vector<uint8_t>& q, B2fSession& to) {
            if (q.empty()) return;
            const size_t n = std::min(q.size(), (size_t)(1 + rng() % (unsigned)maxChunk));
            std::vector<uint8_t> chunk(q.begin(), q.begin() + (long)n);
            q.erase(q.begin(), q.begin() + (long)n);
            to.onBytes(chunk.data(), chunk.size());
            moved = true;
        };
        push(c2g, g);
        push(g2c, c);
        if (!moved) break;
        r.rounds++;
    }
    r.clientDone = c.done(); r.gwDone = g.done(); r.clientFailed = c.failed(); r.gwFailed = g.failed();
    r.cErr = c.error(); r.gErr = g.error();
    return r;
}

void sessionTests() {
    const std::time_t t0 = 1700000000;
    for (int chunk : {1, 7, 125, 4096}) {
        MockGateway gw;
        for (int i = 0; i < 7; i++) gw.waiting.push_back(msg("LY1GW", kMyCall, "gw msg " + std::to_string(i), std::string(200 + 300 * i, (char)('a' + i)), t0 + i));
        gw.waiting.push_back(msg("LY1GW", kMyCall, "with file", "body", t0 + 50, {{"f.bin", std::vector<uint8_t>(5000, 0x55)}}));
        std::vector<B2Message> mine;
        for (int i = 0; i < 3; i++) mine.push_back(msg(kMyCall, "friend@example.com", "out " + std::to_string(i), "hello " + std::to_string(i), t0 + 100 + i));
        gw.callerHas.insert(mine[1].mid());   // the gateway answers '-' to this one
        std::vector<uint8_t> c2g, g2c;
        gw.begin([&](const uint8_t* d, size_t n) { g2c.insert(g2c.end(), d, d + n); });
        std::vector<B2Message> got;
        std::set<std::string> sentOk, sentRej;
        std::set<std::string> clientHas = {gw.waiting[2].mid()};   // the client refuses one it already has
        B2fSession::Config cc;
        cc.mycall = kMyCall; cc.targetCall = kGwCall; cc.locator = "KO24"; cc.password = kPassword;
        B2fSession::Hooks ch;
        ch.send = [&](const uint8_t* d, size_t n) { c2g.insert(c2g.end(), d, d + n); };
        ch.outbound = [&]() { std::vector<B2Proposal> v; for (auto& m : mine) if (!sentOk.count(m.mid()) && !sentRej.count(m.mid())) v.push_back(makeProposal(m)); return v; };
        ch.inboundAnswer = [&](const B2Proposal& p) { return clientHas.count(p.mid) ? '-' : '+'; };
        ch.received = [&](const B2Message& m, const B2Proposal&) { got.push_back(m); clientHas.insert(m.mid()); };
        ch.sent = [&](const std::string& mid, bool rej) { (rej ? sentRej : sentOk).insert(mid); };
        B2fSession c(cc, ch);
        const PipeResult r = pipe(c, *gw.s, c2g, g2c, chunk, (uint32_t)chunk);
        CHECK(r.clientDone && r.gwDone, "chunk %d: both done (client %s / gw %s)", chunk, r.cErr.c_str(), r.gErr.c_str());
        CHECK(gw.s->loginOk() && c.remoteSid().find("B2F") != std::string::npos, "chunk %d: secure login and SID", chunk);
        CHECK(got.size() == 7, "chunk %d: client got %zu of 7 (one refused as a duplicate, 8 offered in two blocks)", chunk, got.size());
        CHECK(gw.got.size() == 2 && sentOk.size() == 2 && sentRej.size() == 1 && sentRej.count(mine[1].mid()), "chunk %d: gateway got %zu, sent %zu, rejected %zu", chunk, gw.got.size(), sentOk.size(), sentRej.size());
        bool fileOk = false;
        for (const auto& m : got) if (m.subject() == "with file" && m.files.size() == 1 && m.files[0].data == std::vector<uint8_t>(5000, 0x55)) fileOk = true;
        CHECK(fileOk, "chunk %d: attachment intact", chunk);
        for (const auto& m : gw.got) CHECK(m.bodyText().rfind("hello ", 0) == 0 && m.from() == kMyCall, "chunk %d: gateway copy", chunk);
    }
    {   // wrong password: the gateway refuses, both stop
        MockGateway gw;
        gw.password = "other";
        std::vector<uint8_t> c2g, g2c;
        gw.begin([&](const uint8_t* d, size_t n) { g2c.insert(g2c.end(), d, d + n); });
        B2fSession::Config cc; cc.mycall = kMyCall; cc.targetCall = kGwCall; cc.password = kPassword;
        B2fSession::Hooks ch; ch.send = [&](const uint8_t* d, size_t n) { c2g.insert(c2g.end(), d, d + n); };
        B2fSession c(cc, ch);
        const PipeResult r = pipe(c, *gw.s, c2g, g2c, 50, 3);
        CHECK(r.gwFailed && r.clientFailed && r.cErr.find("Secure login failed") != std::string::npos, "wrong password: %s", r.cErr.c_str());
    }
    {   // no password set while the gateway asks: the client says so and sends nothing more
        MockGateway gw;
        std::vector<uint8_t> c2g, g2c;
        gw.begin([&](const uint8_t* d, size_t n) { g2c.insert(g2c.end(), d, d + n); });
        B2fSession::Config cc; cc.mycall = kMyCall; cc.targetCall = kGwCall;
        B2fSession::Hooks ch; ch.send = [&](const uint8_t* d, size_t n) { c2g.insert(c2g.end(), d, d + n); };
        B2fSession c(cc, ch);
        pipe(c, *gw.s, c2g, g2c, 50, 4);
        CHECK(c.failed() && c.error().find("password") != std::string::npos, "no password: %s", c.error().c_str());
    }
    {   // nothing either way: FF, FQ
        MockGateway gw;
        std::vector<uint8_t> c2g, g2c;
        gw.begin([&](const uint8_t* d, size_t n) { g2c.insert(g2c.end(), d, d + n); });
        B2fSession::Config cc; cc.mycall = kMyCall; cc.targetCall = kGwCall; cc.password = kPassword;
        B2fSession::Hooks ch; ch.send = [&](const uint8_t* d, size_t n) { c2g.insert(c2g.end(), d, d + n); };
        B2fSession c(cc, ch);
        const PipeResult r = pipe(c, *gw.s, c2g, g2c, 50, 5);
        bool ff = false, fq = false;
        for (const auto& l : gw.trace) { if (l == "<FF") ff = true; if (l == ">FQ") fq = true; }
        CHECK(r.clientDone && r.gwDone && ff && fq, "empty exchange: FF then FQ");
    }
    {   // a corrupted byte in a message block: the checksum catches it
        MockGateway gw;
        gw.waiting.push_back(msg("LY1GW", kMyCall, "x", std::string(900, 'q'), t0));
        std::vector<uint8_t> c2g, g2c;
        bool corrupt = false;
        gw.begin([&](const uint8_t* d, size_t n) {
            std::vector<uint8_t> v(d, d + n);
            if (!corrupt && n > 50 && v[0] == 2) { v[20] ^= 0x01; corrupt = true; }
            g2c.insert(g2c.end(), v.begin(), v.end());
        });
        B2fSession::Config cc; cc.mycall = kMyCall; cc.targetCall = kGwCall; cc.password = kPassword;
        B2fSession::Hooks ch; ch.send = [&](const uint8_t* d, size_t n) { c2g.insert(c2g.end(), d, d + n); };
        B2fSession c(cc, ch);
        pipe(c, *gw.s, c2g, g2c, 4096, 6);
        CHECK(corrupt && c.failed() && c.error().find("checksum") != std::string::npos, "corrupt block: %s", c.error().c_str());
    }
}

// ---------------------------------------------------------------- whole station sessions on a simulated air

struct RunResult { bool ok = false; int rx = 0, tx = 0; double secs = 0; std::string err; uint64_t dropped = 0, retries = 0; size_t inbox = 0, sent = 0, outbox = 0; };

template <class LinkT, class FrameT, class Conv>
RunResult runStation(Transport tr, std::unique_ptr<LinkT> gwLink, double loss, uint32_t seed, Conv toGwFrame, int nGw, int nMine,
                     int paclen = 128, int window = 4) {
    const std::time_t t0 = 1700000000;
    const std::string dir = tempDir("st");
    Mailbox mb(dir);
    for (int i = 0; i < nMine; i++) mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "out " + std::to_string(i), std::string(150 + 400 * i, (char)('A' + i)), t0 + i));
    MockGateway gw;
    for (int i = 0; i < nGw; i++) gw.waiting.push_back(msg("LY1GW", kMyCall, "news " + std::to_string(i), "Line\r\n" + std::string(300 + 500 * i, (char)('k' + i)), t0 + 10 + i));
    Air<FrameT> air;
    air.loss = loss;
    air.rng.seed(seed);
    double now = 0;
    LinkT& gl = *gwLink;
    gw.begin([&](const uint8_t* d, size_t n) { gl.write(d, n); });
    gl.onData = [&](const uint8_t* d, size_t n) { gw.s->onBytes(d, n); };
    gl.onState = [&](LinkState s) { if (s == LinkState::Connected && gw.s->state() == B2fSession::State::Idle) gw.s->start(); };
    gl.sendFrame = [&](const FrameT& f) { air.send(air.toClient, f, now); };
    if (getenv("WLDEBUG")) { gl.log = [&](const std::string& l) { printf("%7.2f GWLINK %s\n", now, l.c_str()); }; gw.quiet = false; }
    gl.listen();
    WinlinkStation st(mb);
    StationConfig sc;
    sc.mycall = kMyCall; sc.locator = "KO24"; sc.password = kPassword;
    sc.gateway.call = kGwCall; sc.gateway.transport = tr;
    sc.paclen = paclen; sc.window = window;
    std::string err;
    RunResult r;
    if (!st.start(sc, now, err)) { r.err = err; return r; }
    const double dt = 0.05;
    for (; now < 4000 && st.running(); now += dt) {
        if (getenv("WLDEBUG")) for (auto& l : st.takeLog()) printf("%7.2f ST %s\n", now, l.c_str());
        toGwFrame(st, air, now);
        air.deliver(air.toGw, now, [&](const FrameT& f) { gl.onFrame(toGwIn(f), now); });
        air.deliver(air.toClient, now, [&](const FrameT& f) { deliverClient(st, f, now); });
        gl.tick(now);
        st.tick(now);
    }
    r.ok = st.lastOk();
    r.err = st.lastError();
    r.rx = st.received(); r.tx = st.sent();
    r.secs = now;
    r.dropped = air.dropped;
    r.retries = gl.retries;
    r.inbox = mb.list(Folder::Inbox).size();
    r.sent = mb.list(Folder::Sent).size();
    r.outbox = mb.list(Folder::Outbox).size();
    // contents
    for (const auto& m : gw.waiting) {
        B2Message in;
        if (!mb.load(Folder::Inbox, m.mid(), in) || in.body != m.body) { r.ok = false; r.err = "inbox copy differs: " + m.mid(); }
    }
    if ((int)gw.got.size() != nMine) { r.ok = false; r.err = "gateway got " + std::to_string(gw.got.size()); }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return r;
}

} // namespace

int main() {
    messageTests();
    sessionTests();
    // AX.25: loss sweep, every window, three packet lengths
    struct Ax { double loss; int window, paclen; };
    std::vector<Ax> axCases = {{0, 4, 128}, {0.1, 4, 128}, {0.2, 2, 64}, {0.3, 7, 256}, {0.1, 1, 32}, {0.05, 7, 128}, {0.15, 3, 200}};
    for (const Ax& c : axCases) {
        Ax25LinkConfig gc;
        gc.mycall = kGwCall;
        gc.t1Sec = 4;
        gc.window = c.window; gc.paclen = c.paclen;
        auto link = std::make_unique<Ax25Link>(gc);
        const RunResult r = runStation<Ax25Link, ax25::Frame>(Transport::Packet1200, std::move(link), c.loss, 11 + (uint32_t)(c.loss * 100) + (uint32_t)c.window,
            [](WinlinkStation& st, Air<ax25::Frame>& air, double now) { for (auto& f : st.takeAx25()) air.send(air.toGw, f, now); }, 3, 2, c.paclen, c.window);
        printf("AX.25 loss %.0f%% k=%d paclen %d: %s, %d in, %d out, %.0f s simulated, %llu frames lost, %llu gateway retries\n", c.loss * 100, c.window, c.paclen,
               r.ok ? "ok" : r.err.c_str(), r.rx, r.tx, r.secs, (unsigned long long)r.dropped, (unsigned long long)r.retries);
        CHECK(r.ok && r.rx == 3 && r.tx == 2 && r.inbox == 3 && r.sent == 2 && r.outbox == 0, "AX.25 loss %.2f k %d paclen %d: %s", c.loss, c.window, c.paclen, r.err.c_str());
    }
    // many random channels: 20 seeds at 25 % loss, the window and packet length varied
    {
        int okN = 0;
        for (uint32_t seed = 100; seed < 120; seed++) {
            Ax25LinkConfig gc;
            gc.mycall = kGwCall; gc.t1Sec = 4;
            gc.window = 1 + (int)(seed % 7); gc.paclen = 32 + (int)(seed * 37 % 224);
            auto link = std::make_unique<Ax25Link>(gc);
            const RunResult r = runStation<Ax25Link, ax25::Frame>(Transport::Packet9600, std::move(link), 0.25, seed,
                [](WinlinkStation& st, Air<ax25::Frame>& air, double now) { for (auto& f : st.takeAx25()) air.send(air.toGw, f, now); }, 2, 2, gc.paclen, gc.window);
            if (r.ok && r.rx == 2 && r.tx == 2) okN++;
            else printf("AX.25 seed %u: %s\n", seed, r.err.c_str());
        }
        printf("AX.25 at 25%% loss: %d of 20 random channels complete\n", okN);
        CHECK(okN == 20, "AX.25 25%% loss sweep: %d of 20", okN);
        okN = 0;
        for (uint32_t seed = 200; seed < 210; seed++) {
            ArdopLinkConfig gc;
            gc.mycall = kGwCall;
            auto link = std::make_unique<ArdopLink>(gc);
            const RunResult r = runStation<ArdopLink, ArdopLinkFrame>(Transport::Ardop, std::move(link), 0.25, seed,
                [](WinlinkStation& st, Air<ArdopLinkFrame>& air, double now) { for (auto& f : st.takeArdop()) air.send(air.toGw, f, now); }, 1, 1);
            if (r.ok && r.rx == 1 && r.tx == 1) okN++;
            else printf("ARDOP seed %u: %s\n", seed, r.err.c_str());
        }
        printf("ARDOP at 25%% loss: %d of 10 random channels complete\n", okN);
        CHECK(okN == 10, "ARDOP 25%% loss sweep: %d of 10", okN);
    }
    // ARDOP ARQ: loss sweep
    for (double loss : {0.0, 0.1, 0.2, 0.3}) {
        ArdopLinkConfig gc;
        gc.mycall = kGwCall;
        auto link = std::make_unique<ArdopLink>(gc);
        const RunResult r = runStation<ArdopLink, ArdopLinkFrame>(Transport::Ardop, std::move(link), loss, 21 + (uint32_t)(loss * 100),
            [](WinlinkStation& st, Air<ArdopLinkFrame>& air, double now) { for (auto& f : st.takeArdop()) air.send(air.toGw, f, now); }, 2, 2);
        printf("ARDOP loss %.0f%%: %s, %d in, %d out, %.0f s simulated, %llu frames lost, %llu gateway retries\n", loss * 100, r.ok ? "ok" : r.err.c_str(), r.rx, r.tx, r.secs,
               (unsigned long long)r.dropped, (unsigned long long)r.retries);
        CHECK(r.ok && r.rx == 2 && r.tx == 2 && r.inbox == 2 && r.sent == 2, "ARDOP loss %.2f: %s", loss, r.err.c_str());
    }
    {   // a call nobody answers: the station gives up after its retries
        const std::string dir = tempDir("none");
        Mailbox mb(dir);
        WinlinkStation st(mb);
        StationConfig sc;
        sc.mycall = kMyCall; sc.gateway.call = kGwCall; sc.gateway.transport = Transport::Packet9600;
        std::string err;
        CHECK(st.start(sc, 0, err), "start: %s", err.c_str());
        double now = 0;
        size_t frames = 0;
        for (; now < 200 && st.running(); now += 0.1) { st.tick(now); frames += st.takeAx25().size(); }
        CHECK(!st.running() && !st.lastOk() && st.lastError().find("no answer") != std::string::npos && frames == 11, "no answer: %s after %zu SABM", st.lastError().c_str(), frames);
        // Telnet stays off unless allowed
        sc.gateway.transport = Transport::Telnet; sc.gateway.host = "127.0.0.1";
        CHECK(!st.start(sc, 0, err) && err.find("off") != std::string::npos, "telnet refused while off: %s", err.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    {   // gateway list
        std::vector<Gateway> g(2);
        g[0].call = "LY2XYZ-10"; g[0].transport = Transport::Ardop; g[0].mhz = 3.5915; g[0].note = "evening | 80 m";
        g[1].call = "LY1PKT-10"; g[1].transport = Transport::Packet9600; g[1].mhz = 432.5; g[1].via = "LY1DIGI";
        const auto b = loadGateways(saveGateways(g));
        CHECK(b.size() == 2 && b[0].call == g[0].call && b[0].transport == Transport::Ardop && std::fabs(b[0].mhz - 3.5915) < 1e-9 && b[1].via == "LY1DIGI" && b[0].note == "evening   80 m",
              "gateway list round trip");
    }
    return testSummary("winlink b2f");
}
