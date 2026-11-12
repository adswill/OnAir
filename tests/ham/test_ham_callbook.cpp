// Callsign lookup (4.0): QRZ and HamQTH XML (record, login answer, errors), the logbook as a callbook, filling a QSO, the cache file round trip,
// and the client with a fake transport: off by default sends nothing, credentials only in the POST body, one login then lookups, the cache
// answers the second time, an expired session logs in once more. The services themselves were not contacted.
#include "dect2/ham/ham_callbook.h"
#include <cstdio>
#include <filesystem>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char* kQrz = R"(<?xml version="1.0" ?>
<QRZDatabase version="1.34" xmlns="http://xmldata.qrz.com">
<Callsign>
<call>AA7BQ</call><fname>FRED L</fname><name>LLOYD</name><addr1>8711 E PINNACLE PEAK RD 159</addr1><addr2>SCOTTSDALE</addr2>
<state>AZ</state><zip>85014</zip><country>United States</country><lat>34.23456</lat><lon>-112.34356</lon><grid>DM32af</grid>
<county>Maricopa</county><dxcc>291</dxcc><cqzone>3</cqzone><ituzone>6</ituzone><lotw>1</lotw><eqsl>0</eqsl><qslmgr>via LoTW &amp; direct</qslmgr>
</Callsign>
<Session><Key>2331uf894c4bd29f3923f3bacf02c532d7bd9</Key><Count>123</Count><GMTime>Sun Aug 16 03:51:47 2012</GMTime></Session>
</QRZDatabase>)";
static const char* kQrzLogin = R"(<?xml version="1.0" ?><QRZDatabase version="1.34"><Session><Key>KEY123</Key><Count>1</Count></Session></QRZDatabase>)";
static const char* kQrzExpired = R"(<?xml version="1.0" ?><QRZDatabase version="1.34"><Session><Error>Session Timeout</Error></Session></QRZDatabase>)";
static const char* kQrzNotFound = R"(<?xml version="1.0" ?><QRZDatabase version="1.34"><Session><Key>KEY123</Key><Error>Not found: XX1XX</Error></Session></QRZDatabase>)";
static const char* kHamQth = R"(<?xml version="1.0"?>
<HamQTH version="2.7" xmlns="https://www.hamqth.com"><search><callsign>ok2cqr</callsign><nick>Petr</nick><qth>Neratovice</qth>
<country>Czech Republic</country><adif>503</adif><itu>28</itu><cq>15</cq><grid>jo70gg</grid><adr_name>Petr Hlozek</adr_name>
<lotw>Y</lotw><eqsl>Y</eqsl><qsl_via></qsl_via><latitude>50.25</latitude><longitude>14.51</longitude></search></HamQTH>)";

int main() {
    // ---- the services' answers ----
    {
        const CallbookReply r = parseQrzXml(kQrz);
        CHECK(r.ok && r.e.call == "AA7BQ" && r.e.name == "FRED L LLOYD" && r.e.qth == "SCOTTSDALE" && r.e.state == "AZ" && r.e.grid == "DM32af");
        CHECK(r.e.dxcc == 291 && r.e.cqz == 3 && r.e.ituz == 6 && r.e.lotw && !r.e.eqsl && r.e.hasPos && r.e.county == "Maricopa");
        CHECK(r.e.qslVia == "via LoTW & direct" && r.sessionKey == "2331uf894c4bd29f3923f3bacf02c532d7bd9" && r.e.source == "QRZ");
        const CallbookReply l = parseQrzXml(kQrzLogin);
        CHECK(!l.ok && l.sessionKey == "KEY123" && l.error.empty());
        const CallbookReply x = parseQrzXml(kQrzExpired);
        CHECK(!x.ok && x.sessionKey.empty() && x.error == "Session Timeout");
        CHECK(!parseQrzXml("garbage").ok);
        const CallbookReply h = parseHamQthXml(kHamQth);
        CHECK(h.ok && h.e.call == "OK2CQR" && h.e.name == "Petr" && h.e.qth == "Neratovice" && h.e.dxcc == 503 && h.e.cqz == 15 && h.e.ituz == 28);
        CHECK(h.e.grid == "jo70gg" && h.e.lotw && h.e.eqsl && h.e.source == "HamQTH");
        const CallbookReply hs = parseHamQthXml(R"(<HamQTH><session><session_id>abc</session_id></session></HamQTH>)");
        CHECK(!hs.ok && hs.sessionKey == "abc");
        const CallbookReply he = parseHamQthXml(R"(<HamQTH><session><error>Wrong user name or password</error></session></HamQTH>)");
        CHECK(!he.ok && he.error == "Wrong user name or password");
    }

    // ---- the logbook ----
    {
        std::vector<LogQso> log(3);
        log[0].call = "K1ABC"; log[0].name = "Bob"; log[0].qth = "Boston"; log[0].grid = "FN42"; log[0].startUtc = 1000; log[0].state = "MA";
        log[1].call = "K1ABC/P"; log[1].name = "Robert"; log[1].grid = "FN42ab"; log[1].startUtc = 2000; log[1].dxcc = 291;
        log[2].call = "W9ZZZ"; log[2].name = "Zed"; log[2].startUtc = 3000;
        const CallbookEntry e = callbookFromLog(log, "K1ABC");
        CHECK(e.name == "Robert" && e.qth == "Boston" && e.grid == "FN42ab" && e.state == "MA" && e.dxcc == 291 && e.source == "log");
        CHECK(callbookFromLog(log, "N0NE").empty());
        LogQso q; q.call = "K1ABC"; q.name = "Already";
        CHECK(callbookFill(q, e) == 4 && q.name == "Already" && q.qth == "Boston" && q.grid == "FN42AB" && q.dxcc == 291);
        CallbookEntry a; a.name = "A"; CallbookEntry b; b.name = "B"; b.qth = "Q"; b.grid = "KO24pq"; a.grid = "KO24";
        callbookMerge(a, b);
        CHECK(a.name == "A" && a.qth == "Q" && a.grid == "KO24pq");
    }

    // ---- the cache line ----
    {
        CallbookEntry e = parseQrzXml(kQrz).e;
        e.name = "Fred\tL"; e.fetchedUtc = 1794441600;
        CallbookEntry r;
        CHECK(callbookCacheParse(callbookCacheLine(e), r));
        CHECK(r.call == "AA7BQ" && r.name == "Fred L" && r.grid == "DM32af" && r.dxcc == 291 && r.lotw && r.hasPos && r.fetchedUtc == 1794441600);
        CHECK(!callbookCacheParse("too\tfew", r) && !callbookCacheParse("", r));
    }

    // ---- the client ----
    {
        int calls = 0, logins = 0, expireOnce = 0;
        std::string lastUrl, lastBody;
        UpTransport fake = [&](const std::string& url, const std::string& body) {
            calls++; lastUrl = url; lastBody = body;
            UpResponse r; r.status = 200;
            if (body.find("username=") == 0) { logins++; r.body = kQrzLogin; return r; }
            if (expireOnce > 0) { expireOnce--; r.body = kQrzExpired; return r; }
            if (body.find("callsign=XX1XX") != std::string::npos) { r.body = kQrzNotFound; return r; }
            r.body = kQrz;
            return r;
        };
        CallbookClient c(fake);
        CallbookEntry e;
        std::string err;
        CHECK(!c.lookup("AA7BQ", 100, e, err) && calls == 0);   // off by default: nothing is sent
        CallbookConfig cfg; cfg.service = CallbookService::Qrz; cfg.user = "me"; cfg.password = "p&ss word";
        c.setConfig(cfg);
        const std::string dir = (std::filesystem::temp_directory_path() / "onair_cb_test").string();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::string path = dir + "/callbook.tsv";
        CHECK(!c.loadCache(path));                               // no file yet
        CHECK(c.lookup("aa7bq", 100, e, err) && e.name == "FRED L LLOYD" && logins == 1 && calls == 2);
        CHECK(lastUrl.find("password") == std::string::npos && lastUrl.find("me") == std::string::npos);
        CHECK(callbookLoginRequest(cfg).body == "username=me&password=p%26ss%20word&agent=OnAir");
        CHECK(callbookLoginRequest(cfg).url.rfind("https://", 0) == 0);
        CHECK(c.lookup("AA7BQ", 200, e, err) && e.source == "cache" && calls == 2);   // the cache answers
        CHECK(c.lookup("AA7BQ", 200 + 181 * 86400.0, e, err) && calls == 3 && e.source == "QRZ");   // too old: looked up again
        expireOnce = 1;
        CHECK(c.lookup("W1AW", 300, e, err) && logins == 2);      // the session expired: one more login
        CHECK(!c.lookup("XX1XX", 300, e, err) && err.find("Not found") != std::string::npos);
        CallbookClient d(fake);
        CHECK(d.loadCache(path) && d.cacheSize() == 1);           // only AA7BQ was found (W1AW got the AA7BQ answer, stored under AA7BQ)
        CHECK(d.local("AA7BQ", nullptr).name == "FRED L LLOYD" && d.local("AA7BQ/P", nullptr).source == "cache");
        CHECK(d.local("N0NE", nullptr).empty());
        std::filesystem::remove_all(dir);
        // HamQTH request shape
        CallbookConfig h; h.service = CallbookService::HamQth; h.user = "u"; h.password = "p";
        CHECK(callbookLoginRequest(h).url == "https://www.hamqth.com/xml.php" && callbookLoginRequest(h).body == "u=u&p=p");
        CHECK(callbookLookupRequest(h, "S1", "ok2cqr").body == "id=S1&callsign=OK2CQR&prg=OnAir");
        CHECK(!CallbookConfig().ready());
    }

    printf(fails ? "ham_callbook: %d FAILED\n" : "ham_callbook: all passed\n", fails);
    return fails ? 1 : 0;
}
