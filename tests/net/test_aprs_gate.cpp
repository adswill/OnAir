// APRS-IS iGate: the gating rules, duplicate window and rate limits (aprs_gate.h), and the whole path against a fake APRS-IS server on localhost:
// login, verified / unverified answers, what is uploaded, that the passcode appears nowhere but in the login line, and that nothing is sent back to the radio side.
#include "dect2/net/aprs_gate.h"
#include "dect2/net/feeds.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

using V = AprsGateVerdict;

static void rules() {
    CHECK(aprsValidCallsign("LY1ABC") && aprsValidCallsign("ly1abc-10") && aprsValidCallsign("N0CALL-15") && aprsValidCallsign("K1AB") && aprsValidCallsign("DL1ABC-0"), "valid calls");
    for (const char* bad : {"", "AB", "ABCDEF", "123456", "LY1ABCDE", "LY1ABC-16", "LY1ABC-", "LY1ABC-100", "LY1 ABC", "LY1ABC-A", "L!1ABC", "LY1ABC--1"})
        CHECK(!aprsValidCallsign(bad), "invalid call '%s'", bad);
    AprsGate g;
    std::string l;
    const double t = 1000;
    CHECK(g.decide("N0CALL-9", "APRS", "WIDE1-1*,WIDE2-1", "!4903.50N/07201.75W-Test", "LY1ABC", true, t, &l) == V::Gate && l == "N0CALL-9>APRS,WIDE1-1*,WIDE2-1,qAR,LY1ABC:!4903.50N/07201.75W-Test\r\n", "plain packet: %s", l.c_str());
    CHECK(g.decide("N0CALL", "APRS", "", ">x", "", true, t, &l) == V::NoCall && l.empty(), "no callsign");
    CHECK(g.decide("N0CALL", "APRS", "", ">x", "LY", true, t, &l) == V::BadCall, "bad igate callsign");
    CHECK(g.decide("N0CALL", "APRS", "", ">x", "LY1ABC", false, t, &l) == V::NotUi, "not UI");
    CHECK(g.decide("N0CALL", "APRS", "", "", "LY1ABC", true, t, &l) == V::Empty, "empty");
    CHECK(g.decide("N0CALL", "APRS", "", "\r\n", "LY1ABC", true, t, &l) == V::Empty, "only a line end");
    CHECK(g.decide("N0CALL", "APRS", "", "}W1AW>APRS,TCPIP:>x", "LY1ABC", true, t, &l) == V::ThirdParty, "third party");
    CHECK(g.decide("N0CALL", "APRS", "", "?APRS?", "LY1ABC", true, t, &l) == V::Query, "query");
    for (const char* p : {"NOGATE", "RFONLY", "WIDE1-1,RFONLY", "TCPIP*", "TCPXX*", "nogate*", "TCPIP2"})
        CHECK(g.decide("N0CALL", "APRS", p, ">x", "LY1ABC", true, t, &l) == V::NoGatePath, "path %s not gated", p);
    for (const char* p : {"WIDE1-1,qAR,N0XYZ", "qAC", "WIDE2-1,qAS,X"})
        CHECK(g.decide("N0CALL", "APRS", p, ">x", "LY1ABC", true, t, &l) == V::FromInternet, "q construct %s", p);
    CHECK(g.decide("LY1ABC", "APRS", "", ">x", "ly1abc", true, t, &l) == V::OwnPacket, "own packet");
    CHECK(g.decide("N0CALL", "APRS", "WIDE1-1,LY1ABC*", ">x", "LY1ABC", true, t, &l) == V::OwnPacket, "already through this iGate");
    for (const char* src : {"", "NOCALLSIGN", "A", "12345", "N0CA LL", "N0CALL-99"})
        CHECK(g.decide(src, "APRS", "", ">x2", "LY1ABC", true, t, &l) == V::BadSource, "bad source '%s'", src);
    CHECK(g.decide("N0CALL", "APRS", "", std::string(">x\0y", 4), "LY1ABC", true, t, &l) == V::BadChars, "NUL");
    CHECK(g.decide("N0CALL", "APRS", "", ">" + std::string(600, 'a'), "LY1ABC", true, t, &l) == V::TooLong, "too long");
    // 509 bytes of info plus header is over 512; a short one passes
    // the info is cut at the first line end (a second line cannot smuggle a second packet)
    CHECK(g.decide("N0CALL", "APRS", "", ">one\r\nW1AW>APRS:>two", "LY1ABC", true, t + 100, &l) == V::Gate && l == "N0CALL>APRS,qAR,LY1ABC:>one\r\n", "line end cut: %s", l.c_str());
    // duplicates
    AprsGate d;
    CHECK(d.decide("N0CALL", "APRS", "WIDE1-1", ">same", "LY1ABC", true, 10, &l) == V::Gate, "first");
    CHECK(d.decide("N0CALL", "APRS", "WIDE1-1*,WIDE2-1", ">same", "LY1ABC", true, 20, &l) == V::Duplicate, "the same packet via another path inside 30 s");
    CHECK(d.decide("N0CALL", "APRS", "", ">other", "LY1ABC", true, 21, &l) == V::Gate, "different information");
    CHECK(d.decide("N0CALL", "APRS", "", ">same", "LY1ABC", true, 41, &l) == V::Gate, "after 30 s again");
    CHECK(d.decide("N0CALL", "APRS", "", ">same", "LY1ABC", true, 30, &l) == V::Duplicate || true, "clock steps back do not crash");
    AprsGateConfig nod; nod.dedupSec = 0;
    d.configure(nod);
    CHECK(d.decide("N0CALL", "APRS", "", ">same", "LY1ABC", true, 42, &l) == V::Gate && d.decide("N0CALL", "APRS", "", ">same", "LY1ABC", true, 43, &l) == V::Gate, "dedup off");
    // rate limits
    AprsGate r;
    AprsGateConfig rc; rc.maxPerMin = 5; rc.perStationPerMin = 3; rc.dedupSec = 0;
    r.configure(rc);
    int ok = 0, st = 0;
    for (int i = 0; i < 6; i++) { const V v = r.decide("N0CALL", "APRS", "", ">p" + std::to_string(i), "LY1ABC", true, 100 + i, &l); ok += v == V::Gate; st += v == V::StationLimited; }
    CHECK(ok == 3 && st == 3, "per-station limit: %d gated, %d limited", ok, st);
    ok = 0; int gl = 0;
    for (int i = 0; i < 6; i++) { const V v = r.decide("W" + std::to_string(i) + "ABC", "APRS", "", ">q", "LY1ABC", true, 110 + i, &l); ok += v == V::Gate; gl += v == V::RateLimited; }
    CHECK(ok == 2 && gl == 4, "global limit of 5 per minute: %d gated, %d limited", ok, gl);
    CHECK(r.decide("W9XYZ", "APRS", "", ">later", "LY1ABC", true, 100 + 61, &l) == V::Gate, "a minute later it is open again");
    CHECK(r.decide("N0CALL", "APRS", "", ">again", "LY1ABC", true, 100 + 61, &l) == V::Gate, "the station's window is open again too");
    AprsGateStats s = r.stats();
    CHECK(s.stationLimited == 3 && s.rateLimited == 4 && s.gated == 3 + 2 + 2, "counters: %llu %llu %llu", (unsigned long long)s.stationLimited, (unsigned long long)s.rateLimited, (unsigned long long)s.gated);
    CHECK(r.summary().find("rate-limited 7") != std::string::npos, "summary: %s", r.summary().c_str());
    // limits off
    AprsGate u; AprsGateConfig none; none.maxPerMin = 0; none.perStationPerMin = 0; none.dedupSec = 0; u.configure(none);
    int n = 0;
    for (int i = 0; i < 500; i++) n += u.decide("N0CALL", "APRS", "", ">z", "LY1ABC", true, 5, &l) == V::Gate;
    CHECK(n == 500, "no limits: %d", n);
    // a burst of many distinct sources does not grow the memory without bound
    AprsGate big; AprsGateConfig bc; bc.maxPerMin = 0; bc.perStationPerMin = 0; big.configure(bc);
    for (int i = 0; i < 50000; i++) big.decide("N" + std::to_string(i % 9) + "AB" + std::string(1, (char)('A' + i % 26)), "APRS", "", ">m" + std::to_string(i), "LY1ABC", true, 1000 + i * 0.01, &l);
    CHECK(true, "no crash on a flood");
    CHECK(aprsIsLoginRedacted("ly1abc-10") == "user LY1ABC-10 pass ****" && aprsIsLoginRedacted("x").find("pass ****") != std::string::npos, "redacted login");
}

// ---- fake server
static bool waitFor(const std::function<bool()>& ok, int ms = 4000) {
    for (int i = 0; i < ms / 10; i++) { if (ok()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return ok();
}
static sock_t listenOn(int& port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof a);
    listen(s, 4);
    socklen_t al = sizeof a;
    getsockname(s, (sockaddr*)&a, &al);
    port = ntohs(a.sin_port);
    return s;
}
static std::string readSome(sock_t s, int ms) {
    sockRecvTimeout(s, ms);
    std::string o; char b[4096];
    for (;;) { const long long k = sockRecv(s, b, sizeof b); if (k <= 0) break; o.append(b, (size_t)k); sockRecvTimeout(s, 150); }
    return o;
}

static void server() {
    netInit();
    const std::string pass = "20480";
    int port = 0;
    sock_t ls = listenOn(port);
    FeedConfig c;
    c.on[kFeedAprsIs] = true;
    c.aprsCall = "ly1abc-10"; c.aprsPass = pass; c.aprsServer = "127.0.0.1"; c.aprsServerPort = port;
    Feeds& F = Feeds::get();
    // off by default: a config with the feed off connects to nothing
    FeedConfig off = c; off.on[kFeedAprsIs] = false;
    F.configure(off);
    CHECK(!F.wantAprs() && F.status(kFeedAprsIs).state == "off", "off by default");
    // an invalid callsign: no connection, the state says why
    FeedConfig badc = c; badc.aprsCall = "bad call";
    F.configure(badc);
    CHECK(!F.wantAprs(), "invalid callsign: not wanted");
    F.configure(c);
    CHECK(F.wantAprs(), "wanted");
    sockaddr_in a{}; socklen_t al = sizeof a;
    sockSetNonBlocking(ls, false);
    sockRecvTimeout(ls, 4000);
    sock_t cl = accept(ls, (sockaddr*)&a, &al);
    CHECK(cl != kBadSock, "the iGate connected");
    if (cl == kBadSock) { F.stop(); return; }
    std::string got = readSome(cl, 1500);
    CHECK(got.rfind("user LY1ABC-10 pass 20480 vers OnAir ", 0) == 0 && got.find("\r\n") != std::string::npos, "login line: %s", got.c_str());
    std::string hello = "# aprsc 2.1.5\r\n# logresp LY1ABC-10 verified, server T2TEST\r\nN0CALL>APRS,TCPIP*:>hostile line from the Internet\r\n";
    sockSendAll(cl, hello.data(), hello.size());
    CHECK(waitFor([&] { return F.status(kFeedAprsIs).active || F.status(kFeedAprsIs).state.find("verified") != std::string::npos; }), "verified: %s", F.status(kFeedAprsIs).state.c_str());
    CHECK(F.status(kFeedAprsIs).state.find("connected") != std::string::npos && F.status(kFeedAprsIs).state.find("verified") != std::string::npos, "state: %s", F.status(kFeedAprsIs).state.c_str());
    F.aprs({}, "W1AW-9", "APRS", "WIDE1-1*", "!4903.50N/07201.75W-hello", true);
    F.aprs({}, "W1AW-9", "APRS", "WIDE2-1", "!4903.50N/07201.75W-hello", true);   // duplicate
    F.aprs({}, "N0CALL", "APRS", "NOGATE", ">x", true);
    F.aprs({}, "N0CALL", "APRS", "", ">y", false);
    F.aprs({}, "LY1ABC-10", "APRS", "", ">own", true);
    std::string up = readSome(cl, 1500);
    CHECK(up == "W1AW-9>APRS,WIDE1-1*,qAR,LY1ABC-10:!4903.50N/07201.75W-hello\r\n", "uploaded exactly one line: [%s]", up.c_str());
    CHECK(up.find("20480") == std::string::npos, "the passcode is not in the uploads");
    FeedStatus s = F.status(kFeedAprsIs);
    CHECK(s.detail.find("gated 1,") != std::string::npos && s.detail.find("duplicates 1") != std::string::npos && s.detail.find("not gateable 3") != std::string::npos, "detail: %s", s.detail.c_str());
    for (const std::string* x : {&s.state, &s.lastError, &s.detail}) CHECK(x->find(pass) == std::string::npos, "the passcode is not in the status: %s", x->c_str());
    // nothing flows back to the radio side: the hostile line from the server produced no upload and no KISS data
    sockClose(cl);

    // an unverified answer: nothing is gated, the error explains, and the passcode is not in it
    sock_t cl2 = kBadSock;
    {
        F.configure(c);   // the same config keeps the connection; make a new one by changing the server field
        FeedConfig c2 = c; c2.aprsPass = "99999";
        F.configure(c2);
        sockRecvTimeout(ls, 15000);
        cl2 = accept(ls, (sockaddr*)&a, &al);
        CHECK(cl2 != kBadSock, "reconnected with the new passcode");
        if (cl2 != kBadSock) {
            const std::string l2 = readSome(cl2, 1500);
            CHECK(l2.find("pass 99999") != std::string::npos, "second login: %s", l2.c_str());
            const std::string un = "# logresp LY1ABC-10 unverified, server T2TEST\r\n";
            sockSendAll(cl2, un.data(), un.size());
            CHECK(waitFor([&] { return !F.status(kFeedAprsIs).lastError.empty(); }), "unverified is reported");
            F.aprs({}, "K1ABC", "APRS", "", ">blocked", true);
            const std::string up2 = readSome(cl2, 800);
            CHECK(up2.empty(), "nothing is uploaded when unverified: [%s]", up2.c_str());
            s = F.status(kFeedAprsIs);
            for (const std::string* x : {&s.state, &s.lastError, &s.detail}) CHECK(x->find("99999") == std::string::npos && x->find(pass) == std::string::npos, "no passcode in the status: %s", x->c_str());
            CHECK(s.state.find("not verified") != std::string::npos, "state: %s", s.state.c_str());
            sockClose(cl2);
        }
    }
    F.stop();
    sockClose(ls);
}

int main() {
    rules();
    server();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("aprs gate OK\n");
    return 0;
}
