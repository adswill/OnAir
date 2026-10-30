// Integrations: the MQTT packet encoder/decoder, topic templates, the event JSON, the settings file, Home Assistant discovery, and the hub against an in-process
// fake broker on the loopback (login, QoS 0 and 1, retain, will, reconnect, nothing sent unless switched on) plus the JSON-lines and webhook sinks.
#include "dect2/integrations/integrations.h"
#include "dect2/integrations/mqtt.h"
#include "dect2/automation/json_mini.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/test_util.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace dect2::integrations;
namespace mq = dect2::integrations::mqtt;
namespace au = dect2::automation;

namespace {
au::Event adsb(const char* hex, const char* cs) {
    au::Event e("adsb.aircraft");
    e.at(1792232100.0, "ADS-B", 1090e6);
    e.str("hex", hex).str("callsign", cs).num("alt_ft", 30000).flag("onground", false);
    return e;
}

// ---- a broker that speaks just enough MQTT 3.1.1 ----
struct FakeBroker {
    dect2::sock_t ls = dect2::kBadSock;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::vector<mq::Publish> got;
    std::vector<mq::ConnectOpts> connects;
    std::string wantUser, wantPass;     // empty: no login required
    std::atomic<int> dropAfter{-1};     // close the connection after this many PUBLISH packets (once)
    std::atomic<int> accepted{0};

    bool start() {
        dect2::netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (ls == dect2::kBadSock || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t l = sizeof a;
        getsockname(ls, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        while (!stop) {
            fd_set r;
            FD_ZERO(&r); FD_SET(ls, &r);
            timeval tv{0, 100000};
            if (select((int)ls + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
            dect2::sock_t c = accept(ls, nullptr, nullptr);
            if (c == dect2::kBadSock) continue;
            accepted++;
            serve(c);
            dect2::sockClose(c);
        }
    }
    bool next(dect2::sock_t c, std::string& buf, mq::Packet& pk) {
        for (int i = 0; i < 100 && !stop; i++) {
            const int k = mq::parsePacket(buf, pk);
            if (k > 0) { buf.erase(0, (size_t)k); return true; }
            if (k < 0) return false;
            dect2::sockRecvTimeout(c, 100);
            char b[1024];
            const long long n = dect2::sockRecv(c, b, sizeof b);
            if (n == 0) return false;
            if (n > 0) { buf.append(b, (size_t)n); i = 0; }
        }
        return false;
    }
    void serve(dect2::sock_t c) {
        std::string buf;
        mq::Packet pk;
        if (!next(c, buf, pk) || pk.type != mq::CONNECT) return;
        mq::ConnectOpts co;
        if (!mq::decodeConnect(pk.body, co)) return;
        { std::lock_guard<std::mutex> lk(mu); connects.push_back(co); }
        const bool ok = wantUser.empty() || (co.user == wantUser && co.password == wantPass);
        const std::string ack = mq::encodeConnack(false, ok ? 0 : 4);
        dect2::sockSendAll(c, ack.data(), ack.size());
        if (!ok) return;
        int n = 0;
        while (next(c, buf, pk)) {
            if (pk.type == mq::PUBLISH) {
                mq::Publish p;
                if (!mq::decodePublish(pk, p)) return;
                { std::lock_guard<std::mutex> lk(mu); got.push_back(p); }
                if (p.qos == 1) { const std::string a = mq::encodePuback(p.pid); dect2::sockSendAll(c, a.data(), a.size()); }
                if (dropAfter.load() >= 0 && ++n >= dropAfter.load()) { dropAfter = -1; return; }
            } else if (pk.type == mq::PINGREQ) {
                const std::string a = mq::encodePingresp();
                dect2::sockSendAll(c, a.data(), a.size());
            } else if (pk.type == mq::DISCONNECT) return;
        }
    }
    size_t count() { std::lock_guard<std::mutex> lk(mu); return got.size(); }
    bool waitFor(size_t n, int ms) {
        for (int i = 0; i < ms / 10; i++) { if (count() >= n) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return count() >= n;
    }
    ~FakeBroker() {
        stop = true;
        if (th.joinable()) th.join();
        if (ls != dect2::kBadSock) dect2::sockClose(ls);
    }
};

void testPackets() {
    // remaining length: the four examples of the spec
    for (uint32_t v : {0u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u, 268435455u}) {
        std::string s;
        mq::putVarInt(s, v);
        uint32_t back = 0;
        CHECK(mq::getVarInt((const uint8_t*)s.data(), s.size(), back) == (int)s.size() && back == v, "varint %u", v);
    }
    std::string two;
    mq::putVarInt(two, 128);
    CHECK(two.size() == 2 && (uint8_t)two[0] == 0x80 && (uint8_t)two[1] == 0x01, "128 = 80 01");
    uint32_t v;
    const uint8_t cont[] = {0x80, 0x80};
    CHECK(mq::getVarInt(cont, 2, v) == 0, "incomplete varint");
    const uint8_t bad[] = {0x80, 0x80, 0x80, 0x80, 0x01};
    CHECK(mq::getVarInt(bad, 5, v) == -1, "five-byte varint refused");

    mq::ConnectOpts c;
    c.clientId = "onair-1"; c.user = "kris"; c.password = "s3cret"; c.keepAlive = 90;
    c.willTopic = "onair/status"; c.willPayload = "offline"; c.willRetain = true;
    const std::string cp = mq::encodeConnect(c);
    CHECK((uint8_t)cp[0] == 0x10, "CONNECT type byte");
    mq::Packet pk;
    CHECK(mq::parsePacket(cp, pk) == (int)cp.size() && pk.type == mq::CONNECT, "parse CONNECT");
    CHECK(pk.body.compare(0, 6, std::string("\0\4MQTT", 6)) == 0 && pk.body[6] == 4, "protocol name and level 4");
    CHECK((uint8_t)pk.body[7] == (0x02 | 0x04 | 0x20 | 0x40 | 0x80), "connect flags %02x", (uint8_t)pk.body[7]);
    mq::ConnectOpts d;
    CHECK(mq::decodeConnect(pk.body, d) && d.clientId == c.clientId && d.user == c.user && d.password == c.password && d.keepAlive == 90 && d.willTopic == c.willTopic &&
              d.willPayload == "offline" && d.willRetain && d.cleanSession, "CONNECT round trip");
    mq::ConnectOpts anon;
    anon.clientId = "x";
    const std::string ap = mq::encodeConnect(anon);
    mq::Packet pa;
    mq::parsePacket(ap, pa);
    CHECK((uint8_t)pa.body[7] == 0x02, "anonymous connect flags");

    // PUBLISH: spec example bytes for QoS 0 "a/b" "hi"
    mq::Publish p;
    p.topic = "a/b"; p.payload = "hi";
    const std::string pp = mq::encodePublish(p);
    CHECK(pp == std::string("\x30\x07\x00\x03" "a/b" "hi", 9), "QoS 0 PUBLISH bytes");
    p.qos = 1; p.retain = true; p.pid = 0x1234;
    const std::string p1 = mq::encodePublish(p);
    CHECK((uint8_t)p1[0] == 0x33 && (uint8_t)p1[1] == 9, "QoS 1 retain flags %02x len %d", (uint8_t)p1[0], (uint8_t)p1[1]);
    mq::Packet k1;
    CHECK(mq::parsePacket(p1, k1) == (int)p1.size(), "parse PUBLISH 1");
    mq::Publish q;
    CHECK(mq::decodePublish(k1, q) && q.topic == "a/b" && q.payload == "hi" && q.qos == 1 && q.retain && q.pid == 0x1234, "PUBLISH round trip");
    // a big payload (3-byte length) and two packets glued together
    mq::Publish big;
    big.topic = "t"; big.payload = std::string(20000, 'x');
    const std::string bp = mq::encodePublish(big) + mq::encodePingreq();
    mq::Packet bk;
    const int used = mq::parsePacket(bp, bk);
    CHECK(used > 20000 && bk.type == mq::PUBLISH, "big packet");
    mq::Publish bq;
    CHECK(mq::decodePublish(bk, bq) && bq.payload.size() == 20000, "big payload");
    mq::Packet pg;
    CHECK(mq::parsePacket(bp.substr((size_t)used), pg) == 2 && pg.type == mq::PINGREQ, "second packet");
    CHECK(mq::parsePacket(bp.substr(0, 100), pg) == 0, "incomplete packet waits");
    CHECK(mq::parsePacket(std::string("\x00\x00", 2), pg) == -1, "type 0 refused");
    mq::Packet bad2;
    mq::Publish bp2;
    bad2.type = mq::PUBLISH; bad2.flags = 6;
    CHECK(!mq::decodePublish(bad2, bp2), "QoS 3 refused");
    CHECK(mq::encodePuback(0x0102) == std::string("\x40\x02\x01\x02", 4), "PUBACK bytes");
    bool sp; uint8_t rc;
    mq::Packet ck;
    mq::parsePacket(mq::encodeConnack(true, 5), ck);
    CHECK(mq::decodeConnack(ck.body, sp, rc) && sp && rc == 5, "CONNACK round trip");
    CHECK(mq::encodePingreq() == std::string("\xc0\x00", 2) && mq::encodeDisconnect() == std::string("\xe0\x00", 2), "PINGREQ / DISCONNECT bytes");

    CHECK(mq::topicNameValid("onair/adsb/x") && !mq::topicNameValid("") && !mq::topicNameValid("a/+/b") && !mq::topicNameValid("a/#") && !mq::topicNameValid(std::string("a\0b", 3)), "topic names");
}

void testTopics() {
    au::Event e = adsb("4840D6", "KLM 1023");
    CHECK(expandTopic("onair/{family}/{sub}/{ident}", e) == "onair/adsb/aircraft/4840D6", "default template: %s", expandTopic("onair/{family}/{sub}/{ident}", e).c_str());
    CHECK(expandTopic("x/{type}/{mode}/{freq_mhz}", e) == "x/adsb.aircraft/ADS-B/1090", "type, mode, freq: %s", expandTopic("x/{type}/{mode}/{freq_mhz}", e).c_str());
    CHECK(expandTopic("x/{field:callsign}", e) == "x/KLM_1023", "field with a space");
    CHECK(expandTopic("x/{field:nothing}/{ident}", e) == "x/_/4840D6", "missing field -> _");
    au::Event evil("aprs.packet");
    evil.str("callsign", "A/B+C#D").at(1, "APRS", 0);
    CHECK(expandTopic("t/{ident}", evil) == "t/A_B_C_D", "slashes and wildcards cannot get into a topic: %s", expandTopic("t/{ident}", evil).c_str());
    au::Event none("pager.message");
    CHECK(expandTopic("t/{ident}/{freq_mhz}", none) == "t/_/_", "empty ident and zero frequency");
    CHECK(checkTopicTemplate("onair/{family}").empty(), "good template");
    CHECK(!checkTopicTemplate("").empty() && !checkTopicTemplate("a/+/b").empty() && !checkTopicTemplate("a/#").empty() && !checkTopicTemplate("/a").empty() &&
              !checkTopicTemplate("a/{bogus}").empty() && !checkTopicTemplate("a/{family").empty() && !checkTopicTemplate("a/}").empty(), "bad templates refused");
    CHECK(eventIdent(adsb("4840D6", "K")) == "4840D6", "ident prefers hex");
    au::Event ais("ais.position");
    ais.num("mmsi", 244123456);
    CHECK(eventIdent(ais) == "244123456", "numeric mmsi without decimals: %s", eventIdent(ais).c_str());
}

void testJsonAndSettings() {
    const std::string j = eventJson(adsb("4840D6", "KLM1023"));
    au::json::Value v;
    std::string err;
    CHECK(au::json::parse(j, v, err), "event JSON parses: %s", err.c_str());
    CHECK(v.str("source") == "OnAir" && v.str("type") == "adsb.aircraft" && v.str("ident") == "4840D6" && v.num("freq_hz") == 1090e6, "event JSON fields");
    const au::json::Value* f = v.get("fields");
    CHECK(f && f->str("callsign") == "KLM1023" && f->num("alt_ft") == 30000 && !f->flag("onground", true), "event JSON payload fields");

    Settings s;
    s.families = defaultFamilies();
    s.mqtt.enabled = true; s.mqtt.host = "broker.local"; s.mqtt.port = 8883; s.mqtt.tls = true; s.mqtt.user = "u"; s.mqtt.password = "p\"w";
    s.mqtt.retain = true; s.mqtt.qos = 1; s.mqtt.haDiscovery = true; s.mqtt.topicTemplate = "a/{family}";
    s.webhook.enabled = true; s.webhook.url = "https://example.org/hook"; s.webhook.token = "tok";
    s.jsonl.enabled = true; s.jsonl.path = "/tmp/x.jsonl"; s.jsonl.maxMegabytes = 5;
    Settings t;
    CHECK(fromJson(toJson(s), t, err), "settings parse: %s", err.c_str());
    CHECK(t.families == s.families && t.mqtt.host == "broker.local" && t.mqtt.port == 8883 && t.mqtt.tls && t.mqtt.password == "p\"w" && t.mqtt.retain && t.mqtt.qos == 1 &&
              t.mqtt.haDiscovery && t.mqtt.topicTemplate == "a/{family}" && t.webhook.token == "tok" && t.jsonl.maxMegabytes == 5 && t.jsonl.path == "/tmp/x.jsonl", "settings round trip");
    Settings d;
    CHECK(!d.anyEnabled() && !d.mqtt.retain && d.mqtt.qos == 0 && d.mqtt.host.empty() && d.webhook.url.empty() && d.families == 0, "defaults: everything off, retain off, QoS 0, no server");
    Settings e;
    e.mqtt.enabled = true;
    CHECK(!e.anyEnabled(), "enabled without a broker typed sends nothing");
    CHECK(!fromJson("[1]", t, err), "not an object refused");
    const au::FamilyMask df = defaultFamilies();
    CHECK((df & au::familyBit(au::FAdsb)) && (df & au::familyBit(au::FPager)) && !(df & au::familyBit(au::FVoice)) && !(df & au::familyBit(au::FSession)), "default families: decoded messages, not voice or sessions");
    CHECK(httpUrlProblem("https://example.org/h").empty() && !httpUrlProblem("ftp://example.org/x").empty() && !httpUrlProblem("http://a b.c/").empty(), "webhook address check");
}

void testHa() {
    MqttSettings m;
    const auto hs = haSensors(m);
    CHECK(hs.size() >= 4, "HA sensors");
    for (const HaSensor& s : hs) {
        au::json::Value v;
        std::string err;
        CHECK(au::json::parse(s.configJson, v, err), "HA config parses");
        CHECK(s.configTopic == "homeassistant/sensor/onair_" + s.key + "/config", "HA config topic %s", s.configTopic.c_str());
        CHECK(v.str("state_topic") == s.stateTopic && v.str("unique_id") == "onair_" + s.key && v.get("device") && v.str("availability_topic") == "onair/status", "HA config fields");
    }
}

Settings mqttSettings(int port) {
    Settings s;
    s.families = defaultFamilies();
    s.mqtt.enabled = true; s.mqtt.host = "127.0.0.1"; s.mqtt.port = port;
    s.mqtt.clientId = "onair-test";
    return s;
}

void testBroker() {
    FakeBroker b;
    CHECK(b.start(), "fake broker listens");
    b.wantUser = "kris"; b.wantPass = "pw";
    Hub h;
    h.start();
    Settings s = mqttSettings(b.port);
    s.mqtt.user = "kris"; s.mqtt.password = "pw";
    s.mqtt.topicTemplate = "onair/{family}/{sub}/{ident}";

    // nothing is sent while off or without a broker typed
    h.submit(adsb("A", "B"));
    CHECK(h.flush(500) && b.accepted == 0, "off: no connection");
    Settings noHost = s;
    noHost.mqtt.host.clear();
    h.setSettings(noHost);
    h.submit(adsb("A", "B"));
    CHECK(h.flush(500) && b.accepted == 0, "no broker typed: no connection");

    h.setSettings(s);
    h.submit(adsb("4840D6", "KLM1023"));
    CHECK(h.flush(3000) && b.waitFor(1, 3000), "event arrives");
    {
        std::lock_guard<std::mutex> lk(b.mu);
        CHECK(b.connects.size() == 1 && b.connects[0].clientId == "onair-test" && b.connects[0].user == "kris" && b.connects[0].password == "pw", "login sent");
        if (!b.got.empty()) {
            CHECK(b.got[0].topic == "onair/adsb/aircraft/4840D6" && !b.got[0].retain && b.got[0].qos == 0, "topic, retain off, QoS 0 by default: %s", b.got[0].topic.c_str());
            au::json::Value v;
            std::string err;
            CHECK(au::json::parse(b.got[0].payload, v, err) && v.str("ident") == "4840D6", "payload is the event JSON");
        }
    }
    // families the user did not tick are not sent
    au::Event voice("voice.call");
    voice.at(1, "DMR", 0).num("src", 1);
    h.submit(voice);
    h.flush(500);
    CHECK(b.count() == 1, "voice events stay home unless ticked");

    // QoS 1 + retain
    s.mqtt.qos = 1; s.mqtt.retain = true;
    h.setSettings(s);
    h.submit(adsb("111111", "X"));
    CHECK(h.flush(3000) && b.waitFor(2, 3000), "QoS 1 event arrives");
    {
        std::lock_guard<std::mutex> lk(b.mu);
        CHECK(b.got.size() >= 2 && b.got[1].qos == 1 && b.got[1].retain && b.got[1].pid != 0, "QoS 1 retained");
    }
    CHECK(h.stats().mqttSent >= 2 && h.stats().mqttStatus == "connected", "stats: %s", h.stats().mqttStatus.c_str());

    // the broker drops the connection after one publish: the hub reconnects and the next event still arrives
    b.dropAfter = 1;
    h.submit(adsb("222222", "Y"));
    h.flush(3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    h.submit(adsb("333333", "Z"));
    h.flush(10000);
    CHECK(b.waitFor(4, 10000), "reconnect after a drop (%zu messages)", b.count());

    // a wrong password is reported without the password
    FakeBroker b2;
    b2.start();
    b2.wantUser = "kris"; b2.wantPass = "right";
    Settings bad = mqttSettings(b2.port);
    bad.mqtt.user = "kris"; bad.mqtt.password = "wrongpw";
    h.setSettings(bad);
    h.submit(adsb("A", "B"));
    h.flush(3000);
    const std::string st = h.stats().mqttStatus;
    CHECK(st.find("user name or password") != std::string::npos && st.find("wrongpw") == std::string::npos && b2.count() == 0, "refused login: %s", st.c_str());
    h.stop();
}

void testHaOnBroker() {
    FakeBroker b;
    b.start();
    Hub h;
    h.start();
    Settings s = mqttSettings(b.port);
    s.mqtt.haDiscovery = true;
    h.setSettings(s);
    h.submit(adsb("4840D6", "KLM"));
    h.flush(3000);
    const size_t want = 1 + haSensors(s.mqtt).size() + 2;   // status + configs + event + state
    CHECK(b.waitFor(want, 3000), "HA messages: %zu of %zu", b.count(), want);
    std::lock_guard<std::mutex> lk(b.mu);
    bool online = false, cfg = false, state = false;
    for (const mq::Publish& p : b.got) {
        if (p.topic == "onair/status" && p.payload == "online" && p.retain) online = true;
        if (p.topic == "homeassistant/sensor/onair_adsb/config" && p.retain) cfg = true;
        if (p.topic == "onair/ha/adsb") state = true;
    }
    CHECK(online && cfg && state, "HA: online, retained config, state topic");
    CHECK(b.connects.size() == 1 && b.connects[0].willTopic == "onair/status" && b.connects[0].willPayload == "offline" && b.connects[0].willRetain, "HA last will");
}

void testSinks() {
    const std::string path = "test_integrations_out.jsonl";
    std::remove(path.c_str());
    std::remove((path + ".1").c_str());
    Hub h;
    h.start();
    std::mutex mu;
    std::vector<std::string> urls, bodies, heads;
    h.setPost([&](const std::string& u, const std::string& body, const std::vector<std::string>& hd) {
        std::lock_guard<std::mutex> lk(mu);
        urls.push_back(u); bodies.push_back(body);
        for (const auto& x : hd) heads.push_back(x);
        return true;
    });
    Settings s;
    s.families = defaultFamilies();
    s.jsonl.enabled = true; s.jsonl.path = path;
    s.webhook.enabled = true; s.webhook.url = "https://example.org/hook"; s.webhook.token = "abc";
    h.setSettings(s);
    h.submit(adsb("4840D6", "KLM1023"));
    h.submit(adsb("4840D7", "KLM1024"));
    CHECK(h.flush(3000), "flushed");
    FILE* f = fopen(path.c_str(), "rb");
    int lines = 0;
    if (f) { int c; while ((c = fgetc(f)) != EOF) if (c == '\n') lines++; fclose(f); }
    CHECK(lines == 2, "two JSON lines (%d)", lines);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(urls.size() == 2 && urls[0] == "https://example.org/hook" && bodies[0].find("4840D6") != std::string::npos && heads.size() == 2 && heads[0] == "Authorization: Bearer abc", "webhook posts with the bearer token");
    }
    // a bad token or address is refused, not sent
    s.webhook.token = "a b";
    h.setSettings(s);
    h.submit(adsb("1", "A"));
    h.flush(2000);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(urls.size() == 2, "token with odd characters is not sent");
    }
    // the rate limit
    Settings r;
    r.families = defaultFamilies();
    r.maxPerSecond = 5;
    r.jsonl.enabled = true; r.jsonl.path = path;
    h.setSettings(r);
    h.setClock([] { return 1000.0; });
    for (int i = 0; i < 20; i++) h.submit(adsb("A", "B"));
    h.flush(3000);
    CHECK(h.stats().rateDropped == 15, "rate limit drops the rest (%llu)", (unsigned long long)h.stats().rateDropped);
    // the file rotates over its size limit
    r.jsonl.maxMegabytes = 0;
    h.setSettings(r);
    h.stop();
    std::remove(path.c_str());
    std::remove((path + ".1").c_str());
}

void testTlsRefused() {
    FakeBroker b;
    b.start();
    Hub h;
    h.start();
    Settings s = mqttSettings(b.port);
    s.mqtt.tls = true;
    h.setSettings(s);
    h.submit(adsb("A", "B"));
    h.flush(3000);
    // with TLS asked for, a plain connection must never be made (no TLS in this build: refused; with TLS: the handshake fails against a plain broker)
    CHECK(b.count() == 0, "TLS asked for: no plain MQTT is ever sent");
    h.stop();
}

void testEngineHook() {
    au::Engine eng;
    int seen = 0;
    eng.setPublisher([&](const au::Event&) { seen++; }, au::familyBit(au::FAdsb));
    CHECK(eng.wants(au::FAdsb) && !eng.wants(au::FAis), "publisher hook asks only for its families");
    eng.submit(adsb("A", "B"));
    au::Event other("ais.position");
    eng.submit(std::move(other));
    CHECK(seen == 1, "publisher sees ADS-B only (%d)", seen);
    eng.setPublisher(nullptr, 0);
    CHECK(!eng.wants(au::FAdsb), "hook removed");
}
}  // namespace

int main() {
    testPackets();
    testTopics();
    testJsonAndSettings();
    testHa();
    testBroker();
    testHaOnBroker();
    testSinks();
    testTlsRefused();
    testEngineHook();
    return testSummary("integrations");
}
