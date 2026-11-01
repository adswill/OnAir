// The clock sync checks and the hobby NTP server: packet round trip, the reply fields (stratum 1 "GPS", stratum 2 for a time station, unsynchronised
// without a measurement), the rate limit and the LAN filter, the refusals of the one-click sync and its commands, and a request over UDP on the
// loopback address. The clock itself is never touched.
#include "../../core/src/net/netcompat.h"
#include "dect2/time/time_ntp.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2;
using namespace dect2::ntp;
#include "../common/test_util.h"

static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

static ClockMeasurement meas(double offMs, const char* label = "GPS", double at = 1000) {
    ClockMeasurement m;
    m.valid = true; m.offsetMs = offMs; m.accuracyMs = 5; m.atUnix = at; m.label = label;
    return m;
}

static bool request(sock_t s, uint16_t port, int mode, int vn, uint64_t tx, Packet& rep) {
    Packet q;
    q.mode = mode; q.vn = vn; q.transmit = tx;
    uint8_t b[kPacketLen];
    serialize(q, b);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(0x7F000001u);
    sockSendTo(s, b, sizeof b, a);
    uint8_t r[128];
    const long long n = sockRecv(s, r, sizeof r);
    return n >= (long long)kPacketLen && parse(r, (size_t)n, rep);
}

int main() {
    // packets and timestamps
    {
        Packet p;
        p.li = 3; p.vn = 3; p.mode = 4; p.stratum = 2; p.poll = 6; p.precision = -20;
        p.rootDelay = 0x00010002; p.rootDispersion = 0x0003a000;
        memcpy(p.refId, "DCF7", 4);
        p.reference = 0x0123456789abcdefULL; p.origin = 0xfedcba9876543210ULL; p.receive = 1; p.transmit = 0xffffffffffffffffULL;
        uint8_t b[kPacketLen];
        serialize(p, b);
        Packet q;
        CHECK(parse(b, sizeof b, q) && q.li == 3 && q.vn == 3 && q.mode == 4 && q.stratum == 2 && q.poll == 6 && q.precision == -20, "header round trip");
        CHECK(q.rootDelay == p.rootDelay && q.rootDispersion == p.rootDispersion && !memcmp(q.refId, "DCF7", 4), "root fields / refid");
        CHECK(q.reference == p.reference && q.origin == p.origin && q.receive == p.receive && q.transmit == p.transmit, "timestamps");
        CHECK(!parse(b, 47, q), "short packet");
        for (double u : {0.0, 1.7e9, 1786000000.123456, 2085978495.9}) CHECK(std::fabs(toUnix(fromUnix(u)) - u) < 1e-6, "unix %.3f round trip", u);
        CHECK(fromUnix(0) >> 32 == kUnixToNtpSeconds, "1970 in NTP seconds");
        CHECK(std::fabs(fromShort(toShort(0.0123)) - 0.0123) < 2e-5, "short format");
    }
    // replies
    {
        Packet req;
        req.mode = 3; req.vn = 4; req.transmit = 0x1122334455667788ULL;
        const ClockMeasurement g = meas(120, "GPS", 1000);
        ServerState s = stateFor(&g, ClockGps, 1010);
        Packet r;
        CHECK(makeReply(req, s, 1010.5, 1010.5001, r), "reply made");
        CHECK(r.mode == 4 && r.vn == 4 && r.li == 0 && r.stratum == 1 && !memcmp(r.refId, "GPS\0", 4), "GPS: server mode, stratum 1, GPS");
        CHECK(r.origin == req.transmit, "origin is the client's transmit");
        CHECK(std::fabs(toUnix(r.receive) - 1010.5) < 1e-6 && std::fabs(toUnix(r.transmit) - 1010.5001) < 1e-6 && std::fabs(toUnix(r.reference) - 1000) < 1e-6, "reply times");
        CHECK(fromShort(r.rootDispersion) > 0.0049 && fromShort(r.rootDispersion) < 0.0052, "dispersion %.4f", fromShort(r.rootDispersion));
        const ClockMeasurement d = meas(-80, "DCF77", 1000);
        s = stateFor(&d, ClockStation, 1000 + 3600);
        CHECK(makeReply(req, s, 1, 2, r) && r.stratum == 2 && r.li == 0 && !memcmp(r.refId, "DCF7", 4), "time station: stratum 2");
        CHECK(fromShort(r.rootDispersion) > 0.05, "an hour old: dispersion %.3f grew", fromShort(r.rootDispersion));
        s = stateFor(nullptr, ClockGps, 1000);
        CHECK(makeReply(req, s, 1, 2, r) && r.li == 3 && r.stratum == 16, "no measurement: unsynchronised");
        Packet bad = req; bad.mode = 4;
        CHECK(!makeReply(bad, s, 1, 2, r), "a server reply is not answered");
        bad = req; bad.mode = 1;
        CHECK(!makeReply(bad, s, 1, 2, r), "symmetric modes are not served");
        bad = req; bad.vn = 5;
        CHECK(!makeReply(bad, s, 1, 2, r), "version 5");
        bad = req; bad.vn = 3;
        CHECK(makeReply(bad, s, 1, 2, r) && r.vn == 3, "version 3 answered as 3");
    }
    // LAN filter, rate limit
    {
        CHECK(isLanAddress(0x0A000001) && isLanAddress(0xC0A80105) && isLanAddress(0xAC100001) && isLanAddress(0xAC1F0001) && isLanAddress(0x7F000001) && isLanAddress(0xA9FE0101), "LAN addresses");
        CHECK(!isLanAddress(0x08080808) && !isLanAddress(0xAC200001) && !isLanAddress(0xAC0F0001) && !isLanAddress(0xC0A70001) && !isLanAddress(0x64400001), "public addresses");
        CHECK(ipText(0xC0A80105) == "192.168.1.5", "ip text");
        RateLimiter l;
        int ok = 0;
        for (int i = 0; i < 25; i++) ok += l.allow(0xC0A80105, 100 + i * 0.1);
        CHECK(ok == RateLimiter::kPerClient, "one client limited to %d, got %d", RateLimiter::kPerClient, ok);
        CHECK(l.allow(0xC0A80106, 102), "another client is not affected");
        CHECK(l.allow(0xC0A80105, 100 + 25), "after the window the client is served again");
        RateLimiter t;
        int tot = 0;
        for (int i = 0; i < 2000; i++) tot += t.allow(0x0A000000u + (uint32_t)i, 500 + i * 1e-4);
        CHECK(tot == RateLimiter::kTotalPerS, "total per second %d", tot);
    }
    // the one-click sync: refusals and commands
    {
        CHECK(clockSyncRefusal(meas(250, "GPS", 1000), 1005).empty(), "a locked fresh measurement is accepted");
        ClockMeasurement n;
        CHECK(!clockSyncRefusal(n, 1000).empty(), "not locked: refused");
        CHECK(has(clockSyncRefusal(meas(250, "GPS", 1000), 1100), "stale"), "stale: refused");
        CHECK(has(clockSyncRefusal(meas(601000, "GPS", 1000), 1001), "10 minutes"), "more than 10 minutes: refused");
        CHECK(has(clockSyncRefusal(meas(-601000, "GPS", 1000), 1001), "10 minutes"), "more than 10 minutes behind: refused");
        CHECK(clockSyncRefusal(meas(599000, "GPS", 1000), 1001).empty(), "just under 10 minutes is accepted");
        ClockMeasurement nan = meas(NAN, "GPS", 1000);
        CHECK(!clockSyncRefusal(nan, 1001).empty(), "NaN refused");
        const std::string mac = clockSetCommand(OsMac, 3200), win = clockSetCommand(OsWindows, 3200), lin = clockSetCommand(OsLinux, 3200);
        CHECK(has(mac, "date -u -r") && has(mac, "- (3)") && has(mac, "sudo"), "mac command: %s", mac.c_str());
        CHECK(has(win, "FromMilliseconds(-3200)") && has(win, "Set-Date"), "windows command: %s", win.c_str());
        CHECK(has(lin, "now - (3200) milliseconds") && has(lin, "sudo"), "linux command: %s", lin.c_str());
        CHECK(has(clockSetCommand(OsLinux, -1500), "now - (-1500) milliseconds") && has(clockSetCommand(OsWindows, -1500), "FromMilliseconds(1500)"), "a clock behind is put forward");
        CHECK(has(clockElevatedCommand(OsMac, 3200), "osascript") && has(clockElevatedCommand(OsMac, 3200), "with administrator privileges"), "mac prompt");
        CHECK(has(clockElevatedCommand(OsWindows, 3200), "-Verb RunAs"), "UAC");
        CHECK(has(clockElevatedCommand(OsLinux, 3200), "pkexec"), "pkexec");
        CHECK(!clockSetLimit(OsMac, 300).empty() && clockSetLimit(OsMac, 900).empty() && clockSetLimit(OsLinux, 30).empty(), "macOS whole seconds only");
        // only numbers go into the commands
        CHECK(has(clockSetCommand(OsLinux, 1e30), "600000"), "the offset is clamped");
    }
    // a request over UDP on the loopback address
    {
        ntp::Server srv;
        std::string msg;
        CHECK(srv.start(41232, msg, true, 41231) && srv.running() && srv.port() == 41231, "server start: %s", msg.c_str());
        netInit();
        const sock_t c = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
        DWORD to = 1000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
#else
        timeval tv{1, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        Packet rep;
        ClockMeasurement m = meas(250, "GPS");
        ClockDiscipline::get().publish(ClockGps, m);
        const uint64_t tx = fromUnix(systemUnixNow());
        bool got = false;
        for (int tries = 0; tries < 5 && !got; tries++) got = request(c, 41231, 3, 4, tx, rep);   // a loaded CI machine can drop the first datagram
        CHECK(got, "no answer");
        CHECK(rep.mode == 4 && rep.stratum == 1 && rep.li == 0 && !memcmp(rep.refId, "GPS\0", 4) && rep.origin == tx, "GPS reply");
        const double srvT = toUnix(rep.transmit), here = systemUnixNow();
        CHECK(std::fabs((here - srvT) - 0.25) < 0.1, "server time is the computer's less 250 ms: %.3f", here - srvT);
        ClockDiscipline::get().clearAll();
        ClockMeasurement st = meas(-40, "BPM");
        ClockDiscipline::get().publish(ClockStation, st);
        CHECK(request(c, 41231, 3, 3, tx, rep) && rep.stratum == 2 && rep.vn == 3 && !memcmp(rep.refId, "BPM", 3), "station reply stratum %d", rep.stratum);
        ClockDiscipline::get().clearAll();
        CHECK(request(c, 41231, 3, 4, tx, rep) && rep.li == 3 && rep.stratum == 16, "unsynchronised reply");
        Packet none;
        CHECK(!request(c, 41231, 4, 4, tx, none), "a non-client packet gets no answer");
#ifdef _WIN32
        to = 60;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
#else
        tv = timeval{0, 60000};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        for (int i = 0; i < 14; i++) request(c, 41231, 3, 4, tx, none);
        const ServerStats s = srv.stats();
        CHECK(s.requests >= 3 && s.requests <= 10 && s.limited >= 4 && s.refused >= 1, "stats: %llu answered, %llu limited, %llu refused", (unsigned long long)s.requests, (unsigned long long)s.limited, (unsigned long long)s.refused);
        sockClose(c);
        srv.stop();
        CHECK(!srv.running(), "stopped");
        // the port in use: a second server falls back, and with no free port reports it
        ntp::Server a, b;
        CHECK(a.start(41234, msg, true, 41233) && a.port() == 41233, "a");
        CHECK(b.start(41234, msg, true, 41233) && b.port() == 41234 && has(msg, "instead"), "fallback port: %s", msg.c_str());
        ntp::Server c2;
        CHECK(!c2.start(41234, msg, true, 41233) && has(msg, "could not"), "no port: %s", msg.c_str());
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
