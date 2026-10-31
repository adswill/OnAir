// OnAir Web directory heartbeat, the pure part: the signing text (byte for byte the same as web/onair-directory/src/logic.js; the vector below is
// checked by test/logic.test.js too), the Ed25519 sign / verify round trip, field cleaning, URL and location normalisation, the record built from
// the live info, and the confirmation code that keeps anything from being sent before the host has seen it.
#include "../../server/web/web_directory_proto.h"
#include "../../server/web/web_json.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace onair_web;
#include "../common/test_util.h"

static const char* kVecPub = "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664";
static const char* kVecSig = "e9cb580456c8a6b9e4624da86c4432d5e127b103787337150f94bfcdc4098e63175e721095bee0d123318d0adbfc9dfc2a7d50bce2cd8ab8e963897b7c4d5d08";
static const char* kVecText =
    "OnAir-Directory-v1\naction=heartbeat\nid=test-server-01\npub=79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664\nts=1790000000\n"
    "name=Vilnius roof\nurl=https://sdr.example.org/\nloc=54.69,25.28\nfreq=88000000-108000000;1000000-30000000\nradio=HackRF Pro\ncap=20\nusers=3\n"
    "interval=300\nver=3.0.0\ncontact=\ndesc=Wideband receiver\nreach=1\n";

static DirRecord vector() {
    DirRecord r;
    r.id = "test-server-01";
    r.pub = kVecPub;
    r.ts = 1790000000;
    r.name = "Vilnius roof";
    r.url = "https://sdr.example.org/";
    r.loc = "54.69,25.28";
    r.freq = {{88000000, 108000000}, {1000000, 30000000}};
    r.radio = "HackRF Pro";
    r.cap = 20;
    r.users = 3;
    r.interval = 300;
    r.ver = "3.0.0";
    r.desc = "Wideband receiver";
    r.reach = 1;
    return r;
}

static uint8_t kSeed[32];

static void testVector() {
    for (int i = 0; i < 32; i++) kSeed[i] = (uint8_t)(i + 1);
    const DirRecord r = vector();
    CHECK(dirCanonical(r) == kVecText, "canonical text differs from the shared vector:\n%s", dirCanonical(r).c_str());
    CHECK(dirPubHex(kSeed) == kVecPub, "public key of the vector seed");
    CHECK(dirSign(kSeed, r) == kVecSig, "Ed25519 signature equals the one made by WebCrypto: %s", dirSign(kSeed, r).c_str());
    CHECK(dirVerify(kVecPub, kVecSig, r), "the vector verifies");
    DirRecord t = r;
    t.users = 4;
    CHECK(!dirVerify(kVecPub, kVecSig, t), "a changed user count must not verify");
    t = r;
    t.url = "https://evil.example.org/";
    CHECK(!dirVerify(kVecPub, kVecSig, t), "a changed url must not verify");
    std::string bad = kVecSig;
    bad[0] = bad[0] == '0' ? '1' : '0';
    CHECK(!dirVerify(kVecPub, bad, r), "a flipped signature bit must not verify");
    CHECK(!dirVerify("00", kVecSig, r) && !dirVerify(kVecPub, "zz", r), "malformed hex is refused");
    uint8_t other[32];
    memset(other, 7, 32);
    CHECK(!dirVerify(dirPubHex(other), kVecSig, r), "another key must not verify");
    CHECK(dirVerify(dirPubHex(other), dirSign(other, r), r), "round trip with another key");
    // delete
    DirRecord d;
    d.action = "delete";
    d.id = "test-server-01";
    d.pub = kVecPub;
    d.ts = 5;
    CHECK(dirCanonical(d) == std::string("OnAir-Directory-v1\naction=delete\nid=test-server-01\npub=") + kVecPub + "\nts=5\n", "delete text");
    CHECK(dirVerify(kVecPub, dirSign(kSeed, d), d), "delete round trip");
    // the id comes from the key
    const std::string id = dirIdFromPub(kVecPub);
    CHECK(id.size() == 22 && id.compare(0, 6, "onair-") == 0, "id shape %s", id.c_str());
    CHECK(dirIdFromPub("zz").empty(), "bad key, no id");
}

static void testBody() {
    const DirRecord r = vector();
    const std::string b = dirBodyJson(r, kVecSig);
    Json j;
    std::string e;
    CHECK(jsonParse(b, j, e), "body is valid JSON: %s", e.c_str());
    CHECK(b.size() < 4096, "body fits the Worker's limit (%zu)", b.size());
    CHECK(j.get("v") && j.get("v")->num == 1 && j.get("name") && j.get("name")->str == "Vilnius roof" && j.get("sig") && j.get("sig")->str == kVecSig, "fields");
    CHECK(j.get("freq") && j.get("freq")->arr.size() == 2 && j.get("freq")->arr[0].arr[0].num == 88000000, "freq pairs");
    CHECK(j.get("users") && j.get("users")->num == 3 && j.get("reach")->num == 1, "numbers stay numbers");
    // quotes and backslashes in free text stay JSON-safe
    DirRecord q = r;
    q.desc = "a \"quoted\" \\ text";
    Json j2;
    CHECK(jsonParse(dirBodyJson(q, kVecSig), j2, e) && j2.get("desc")->str == q.desc, "escaping");
    // there is no listener data field anywhere
    for (const char* f : {"address", "ip", "listeners", "user_list", "freq_hz"}) CHECK(!j.get(f), "no %s in the body", f);
}

static void testClean() {
    CHECK(dirClean("  hello\n\tworld \x01", 100) == "helloworld", "control characters and ends: '%s'", dirClean("  hello\n\tworld \x01", 100).c_str());
    CHECK(dirClean("abcdef", 3) == "abc", "cut");
    CHECK(dirClean("ab\xc3\xa9" "def", 3) == "ab", "never cut inside a UTF-8 character");   // "abé": the 2-byte é starts at byte 2
    CHECK(dirClean("a\xc2\x85z", 10) == "az", "C1 control removed");
    CHECK(dirClean("a\xe2\x80\xa8z", 10) == "az", "line separator removed");
    CHECK(dirClean("caf\xc3\xa9", 10) == "caf\xc3\xa9", "letters stay");
}

static void testUrl() {
    std::string o, e;
    CHECK(dirNormalizeUrl("https://SDR.Example.org", o, e) && o == "https://sdr.example.org/", "lower case and trailing slash: %s", o.c_str());
    CHECK(dirNormalizeUrl("https://sdr.example.org:443/", o, e) && o == "https://sdr.example.org/", "default port dropped");
    CHECK(dirNormalizeUrl("https://sdr.example.org:8443/radio", o, e) && o == "https://sdr.example.org:8443/radio/", "port and path kept");
    for (const char* bad : {"http://sdr.example.org/", "sdr.example.org", "ftp://x.org/", "https://u:p@x.org/", "https://x.org/?a=1", "https://x.org/#f", "https://x .org/",
                            "https://x.org/a b", "https://x.org/%2e", "https://", "https://x.org/\\y"})
        CHECK(!dirNormalizeUrl(bad, o, e) && !e.empty(), "must refuse %s", bad);
    CHECK(!dirNormalizeUrl("https://" + std::string(250, 'a') + ".org/", o, e), "too long");
}

static void testLoc() {
    std::string o, e;
    CHECK(dirNormalizeLoc("54.6931,25.2797", 2, o, e) && o == "54.69,25.28", "round to 2: %s", o.c_str());
    CHECK(dirNormalizeLoc("54.6931,25.2797", 0, o, e) && o == "55,25", "round to 0: %s", o.c_str());
    CHECK(dirNormalizeLoc("54.6931,25.2797", 4, o, e) && o == "54.6931,25.2797", "more if the host chooses: %s", o.c_str());
    CHECK(dirNormalizeLoc("54.6931,25.2797", 9, o, e) && o == "54.69310,25.27970", "decimals capped at 5: %s", o.c_str());
    CHECK(dirNormalizeLoc("-33.87,151.21", 2, o, e) && o == "-33.87,151.21", "southern hemisphere");
    CHECK(dirNormalizeLoc("KO24", 2, o, e) && o == "KO24", "locator");
    CHECK(dirNormalizeLoc("ko24pq", 2, o, e) && o == "ko24pq", "locator, 6 characters");
    CHECK(dirNormalizeLoc("", 2, o, e) && o.empty(), "empty is allowed");
    for (const char* bad : {"91,0", "0,181", "a,b", "1,", ",1", "KO2", "ZZ99", "KO24p", "12345678", "1,2,3"}) CHECK(!dirNormalizeLoc(bad, 2, o, e), "must refuse %s", bad);
    std::vector<std::pair<long long, long long>> f;
    CHECK(dirParseFreqMHz("88-108, 1.0-30", f, e) && f.size() == 2 && f[0].first == 88000000 && f[1].second == 30000000 && f[1].first == 1000000, "freq list");
    CHECK(dirParseFreqMHz("", f, e) && f.empty(), "empty freq list");
    for (const char* bad : {"108-88", "x", "88", "-5-3", "1-400000", "1-2,3-4,5-6,7-8,9-10,11-12,13-14,15-16,17-18"}) CHECK(!dirParseFreqMHz(bad, f, e), "must refuse freq %s", bad);
}

static void testRecordFromInfo() {
    DirectoryConfig c;
    c.publicUrl = "https://sdr.example.org";
    c.loc = "54.6931,25.2797";
    c.radio = "HackRF Pro";
    c.version = "3.0.0";
    const std::string info = "{\"proto\":1,\"name\":\"Vilnius roof\",\"location\":\"home\",\"antenna\":\"loop\",\"users\":3,\"max_users\":20,\"capture\":{\"centre_hz\":100000000,\"rate_hz\":20000000}}";
    DirRecord r;
    std::string e;
    CHECK(dirBuildRecord(c, info, 1790000000, "onair-0123456789abcdef", kVecPub, r, e), "build: %s", e.c_str());
    CHECK(r.name == "Vilnius roof" && r.url == "https://sdr.example.org/" && r.loc == "54.69,25.28" && r.cap == 20 && r.users == 3 && r.ver == "3.0.0", "fields from config and info");
    CHECK(r.freq.size() == 1 && r.freq[0].first == 90000000 && r.freq[0].second == 110000000, "coverage is the capture window");
    CHECK(r.interval == 300 && r.reach == 0, "defaults: 300 s, no reachability check");
    // nothing about listeners or the free-text location/antenna of /info is forwarded
    CHECK(dirBodyJson(r, "x").find("home") == std::string::npos && dirBodyJson(r, "x").find("loop") == std::string::npos, "location and antenna text of /info are not sent");
    c.freq = "144-146";
    CHECK(dirBuildRecord(c, info, 1, "onair-0123456789abcdef", kVecPub, r, e) && r.freq.size() == 1 && r.freq[0].first == 144000000, "web_directory_freq wins");
    DirectoryConfig bad = c;
    bad.publicUrl = "http://sdr.example.org/";
    CHECK(!dirBuildRecord(bad, info, 1, "onair-0123456789abcdef", kVecPub, r, e) && e.find("web_public_url") != std::string::npos, "http is refused: %s", e.c_str());
    bad = c;
    bad.publicUrl.clear();
    CHECK(!dirBuildRecord(bad, info, 1, "onair-0123456789abcdef", kVecPub, r, e), "no public url");
    bad = c;
    bad.loc = "nowhere";
    CHECK(!dirBuildRecord(bad, info, 1, "onair-0123456789abcdef", kVecPub, r, e), "bad location");
    CHECK(!dirBuildRecord(c, "not json", 1, "onair-0123456789abcdef", kVecPub, r, e), "bad info");
    CHECK(!dirBuildRecord(c, "{\"name\":\"\"}", 1, "onair-0123456789abcdef", kVecPub, r, e), "empty name");
    const std::string longInfo = "{\"name\":\"" + std::string(100, 'n') + "\",\"users\":50,\"max_users\":10}";
    CHECK(dirBuildRecord(c, longInfo, 1, "onair-0123456789abcdef", kVecPub, r, e) && r.name.size() == 48 && r.users == 10, "name cut to 48, users capped at capacity");
    DirectoryConfig iv = c;
    iv.intervalSec = 5;
    CHECK(dirBuildRecord(iv, info, 1, "onair-0123456789abcdef", kVecPub, r, e) && r.interval == 60, "interval clamped to 60..3600");
}

static void testAck() {
    DirectoryConfig c;
    c.publicUrl = "https://sdr.example.org";
    c.url = "https://directory.example.org";
    const std::string info = "{\"name\":\"A\",\"users\":1,\"max_users\":5,\"capture\":{\"centre_hz\":100000000,\"rate_hz\":2000000}}";
    DirRecord a, b;
    std::string e;
    CHECK(dirBuildRecord(c, info, 100, "onair-0123456789abcdef", kVecPub, a, e), "%s", e.c_str());
    const std::string code = dirAck(a, c.url);
    CHECK(code.size() == 12, "12 character code");
    // the things that change by themselves do not change the code
    const std::string info2 = "{\"name\":\"A\",\"users\":4,\"max_users\":5,\"capture\":{\"centre_hz\":433000000,\"rate_hz\":2000000}}";
    dirBuildRecord(c, info2, 99999, "onair-0123456789abcdef", kVecPub, b, e);
    CHECK(dirAck(b, c.url) == code, "time, users and capture do not change the code");
    DirectoryConfig v = c;
    v.version = "9.9";
    dirBuildRecord(v, info, 100, "onair-0123456789abcdef", kVecPub, b, e);
    CHECK(dirAck(b, c.url) == code, "the version does not change the code");
    // what the host chose does
    auto changed = [&](DirectoryConfig x) { DirRecord r; dirBuildRecord(x, info, 100, "onair-0123456789abcdef", kVecPub, r, e); return dirAck(r, x.url) != code; };
    DirectoryConfig x = c; x.publicUrl = "https://other.example.org"; CHECK(changed(x), "public url");
    x = c; x.url = "https://other-directory.example.org"; CHECK(changed(x), "directory url");
    x = c; x.loc = "54.69,25.28"; CHECK(changed(x), "location");
    x = c; x.contact = "me@example.org"; CHECK(changed(x), "contact");
    x = c; x.desc = "text"; CHECK(changed(x), "description");
    x = c; x.radio = "RTL"; CHECK(changed(x), "radio");
    x = c; x.reach = true; CHECK(changed(x), "reachability switch");
    x = c; x.intervalSec = 600; CHECK(changed(x), "interval");
    // a different key gives a different id and pub, so a different code
    DirRecord k = a;
    k.pub = "00" + a.pub.substr(2);
    CHECK(dirAck(k, c.url) != code, "key");
    // the preview shows the target, the exact body and the opt-in text
    const std::string p = dirPreview(a, c.url);
    CHECK(p.find("https://directory.example.org/api/v1/heartbeat") != std::string::npos && p.find("\"name\":\"A\"") != std::string::npos, "preview shows target and body");
    CHECK(p.find("web/v1/info") == std::string::npos, "no reachability text unless opted in");
    a.reach = 1;
    CHECK(dirPreview(a, c.url).find("web/v1/info") != std::string::npos, "reachability text when opted in");
}


#define CHK(c) CHECK(c, "%s", #c)
// ---- what happened, in words a person can act on (step 2 of the OnAir Web setup)
static void testOutcomes() {
    DirOutcome o = dirClassifyHttp(200, "{\"ok\":true}", 0, false);
    CHK(o.ok && o.code == "ok");
    o = dirClassifyHttp(400, "{\"ok\":false,\"error\":\"invalid\",\"details\":[\"ts is too far from the directory's clock\"]}", 900, true);
    CHK(!o.ok && o.code == "clock" && o.advice.find("automatic date and time") != std::string::npos);
    o = dirClassifyHttp(400, "{\"ok\":false,\"error\":\"invalid\",\"details\":[\"url host is not a public name\"]}", 0, false);
    CHK(o.code == "invalid" && o.message.find("url host is not a public name") != std::string::npos && o.advice.find("public https://") != std::string::npos);
    o = dirClassifyHttp(422, "{\"ok\":false,\"error\":\"url host does not resolve to public addresses only\"}", 0, false);
    CHK(o.code == "resolving" && o.retrySoon);
    o = dirClassifyHttp(429, "{}", 0, false);
    CHK(o.code == "rate" && !o.retrySoon);
    o = dirClassifyHttp(503, "{\"error\":\"the directory is full\"}", 0, false);
    CHK(o.code == "full");
    o = dirClassifyHttp(502, "<html>bad gateway</html>", 0, false);
    CHK(o.code == "server" && o.advice.find("Nothing is wrong with your setup") != std::string::npos);
    o = dirClassifyHttp(409, "{\"error\":\"ts is not newer than the last accepted request\"}", 0, false);
    CHK(o.code == "conflict" && o.retrySoon);
    o = dirClassifyHttp(401, "{}", 0, false);
    CHK(o.code == "key");
    // no answer at all
    CHK(dirClassifyNet("dns", "", "web.on-air.dev").retrySoon);
    CHK(dirClassifyNet("tls_cert", "", "x").advice.find("date and time") != std::string::npos);
    CHK(dirClassifyNet("no_curl", "", "x").code == "no_curl");
    CHK(dirClassifyNet("weird", "boom", "x").code == "other");
    // the public address
    CHK(dirClassifyPublic(200, true, "", "https://a.trycloudflare.com/").ok);
    o = dirClassifyPublic(0, false, "timeout", "https://a.trycloudflare.com/");
    CHK(!o.ok && o.advice.find("tunnel") != std::string::npos && o.advice.find("port") != std::string::npos);
    CHK(dirClassifyPublic(530, false, "", "u").message.find("web server behind it") != std::string::npos);
    // retry schedule: seconds for a probably temporary failure, backing off for the others, never over the beat interval
    CHK(dirRetryDelay(1, true, 300) == 5 && dirRetryDelay(3, true, 300) == 15 && dirRetryDelay(99, true, 300) == 60);
    CHK(dirRetryDelay(1, false, 300) == 20 && dirRetryDelay(5, false, 300) == 300 && dirRetryDelay(5, false, 60) == 60);
    // the server's clock from its Date header
    long long t = 0;
    CHK(dirParseHttpDate("Sat, 31 Oct 2026 10:00:00 GMT", t) && t == 1793440800);
    CHK(!dirParseHttpDate("yesterday", t));
    // the status file round trip
    DirStatus ds;
    ds.state = "error"; ds.last = dirClassifyHttp(422, "{}", 0, false); ds.attempts = 4; ds.fails = 3; ds.lastTry = 100; ds.nextTry = 110; ds.id = "onair-0123456789abcdef";
    DirStatus back;
    CHK(dirStatusParse(dirStatusJson(ds), back));
    CHK(back.state == "error" && back.last.code == "resolving" && back.last.retrySoon && back.fails == 3 && back.nextTry == 110 && back.last.advice == ds.last.advice);
    CHK(!dirStatusParse("not json", back));
    // the directory address: https, or plain http for this computer only (the tests)
    std::string out, err;
    CHK(dirNormalizeEndpoint("https://web.on-air.dev", out, err) && out == "https://web.on-air.dev/");
    CHK(dirNormalizeEndpoint("http://127.0.0.1:9911/x", out, err) && out == "http://127.0.0.1:9911/x/");
    CHK(!dirNormalizeEndpoint("http://example.org/", out, err));
    CHK(!dirNormalizeEndpoint("http://127.0.0.1.evil.org/", out, err));
}

int main() {
    testOutcomes();
    testVector();
    testBody();
    testClean();
    testUrl();
    testLoc();
    testRecordFromInfo();
    testAck();
    return testSummary("web_directory");
}
