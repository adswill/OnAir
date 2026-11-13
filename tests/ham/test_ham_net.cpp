// The spot feeds (4.0) without a network: telnet lines (IAC dropped, CR removed, split packets, a prompt without newline), the login prompt
// test, the feed URLs (https only, the callsign only in the PSK Reporter query), and the HTTP polls with a fake getter: off sends nothing,
// each feed at its own interval, PSK Reporter never more often than every 5 minutes, errors reported in the status.
#include "dect2/ham/ham_net.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    {
        TelnetLines t;
        const char a[] = "\xff\xfb\x01Welcome\r\nPlease enter your call: ";
        auto v = t.feed(a, sizeof a - 1);
        CHECK(v.size() == 1 && v[0] == "Welcome" && t.partial() == "Please enter your call: " && telnetAsksCall(t.partial()));
        const char b[] = "\r\nDX de W3LPL:     14025.0  JA1A";
        v = t.feed(b, sizeof b - 1);
        CHECK(v.size() == 1 && t.partial() == "DX de W3LPL:     14025.0  JA1A");
        const char c[] = "BC       CW 599                     1234Z\r\n\r\n";
        v = t.feed(c, sizeof c - 1);
        CHECK(v.size() == 1 && v[0] == "DX de W3LPL:     14025.0  JA1ABC       CW 599                     1234Z" && t.partial().empty());
        CHECK(telnetAsksCall("login:") && telnetAsksCall("Callsign: ") && !telnetAsksCall("DX de W3LPL: 14025.0 JA1ABC CW 1234Z"));
    }
    CHECK(potaUrl().rfind("https://", 0) == 0 && sotaUrl().rfind("https://", 0) == 0);
    CHECK(pskrUrl("LY1ABC/P") == "https://retrieve.pskreporter.info/query?senderCallsign=LY1ABC/P&flowStartSeconds=-900&rronly=1");
    {
        int pota = 0, sota = 0, pskr = 0;
        HttpGet fake = [&](const std::string& url) {
            UpResponse r; r.status = 200;
            if (url == potaUrl()) { pota++; r.body = R"([{"activator":"K1ABC","frequency":"14062","mode":"CW","reference":"US-0001","spotTime":"2026-11-12T10:15:30"}])"; }
            else if (url == sotaUrl()) { sota++; r.status = 503; }
            else { pskr++; r.body = R"(<receptionReports><receptionReport receiverCallsign="DL1XYZ" senderCallsign="LY1ABC" frequency="14075879" flowStartSeconds="1794440000" mode="FT8" sNR="-12"/></receptionReports>)"; }
            return r;
        };
        SpotFeeds f(fake);
        f.pollHttp(1000);
        CHECK(pota + sota + pskr == 0 && f.take().empty());           // all off: nothing is fetched
        SpotFeedConfig c; c.pota = true; c.sota = true; c.pskr = true; c.myCall = "LY1ABC"; c.pskrEverySec = 10;
        SpotFeeds g(fake);
        g.setConfig(c, false);                                         // no worker: the test drives pollHttp with its own clock
        g.pollHttp(1000);
        const int p0 = pota, s0 = sota, r0 = pskr;
        CHECK(p0 >= 1 && s0 >= 1 && r0 >= 1);
        const auto v = g.take();
        CHECK(!v.empty());
        CHECK(g.status(SpotSrc::Sota).find("error") == 0 && g.status(SpotSrc::Pota).find("1 spots") == 0);
        g.pollHttp(1030);
        CHECK(pota == p0 && sota == s0 && pskr == r0);                 // too soon for all
        g.pollHttp(1061);
        CHECK(pota == p0 + 1 && sota == s0 && pskr == r0);             // POTA every 60 s
        g.pollHttp(1121);
        CHECK(sota == s0 + 1 && pskr == r0);                            // SOTA every 120 s; PSK Reporter not before 300 s although 10 was asked
        g.pollHttp(1301);
        CHECK(pskr == r0 + 1);
        c.myCall.clear(); c.pota = false; c.sota = false;
        g.setConfig(c, false);                                          // PSK Reporter without a callsign: not asked
        g.pollHttp(5000);
        CHECK(pskr == r0 + 1 && !g.running());
    }
    printf(fails ? "ham_net: %d FAILED\n" : "ham_net: all passed\n", fails);
    return fails ? 1 : 0;
}
