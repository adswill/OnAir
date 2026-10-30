// Watchlists: matching of every list kind, push request building (and that tokens stay out of the history), rate limit, repeat cooldown,
// quiet hours, the JSON file, the engine hook.
#include "dect2/automation/engine.h"
#include "dect2/watchlist/watchlist.h"
#include "../common/test_util.h"
#include <chrono>
#include <thread>

using namespace dect2::watchlist;
namespace au = dect2::automation;

namespace {
constexpr double kT0 = 1792232100.0;   // 2026-10-17 09:55 UTC (roughly)

struct Fake : Sink {
    std::vector<std::string> notes, sounds;
    std::vector<Request> posts;
    void notify(const std::string& t, const std::string& b) override { notes.push_back(t + "|" + b); }
    void sound(const std::string& f) override { sounds.push_back(f); }
    void post(const Request& r) override { posts.push_back(r); }
};

au::Event adsb(const char* hex, const char* cs) {
    au::Event e("adsb.aircraft");
    e.at(kT0, "ADS-B", 1090e6);
    e.str("hex", hex).str("callsign", cs).num("alt_ft", 30000);
    return e;
}
List mk(Kind k, std::vector<std::string> v, const char* name = "L") {
    List l;
    l.id = name; l.name = name; l.kind = k; l.values = std::move(v);
    return l;
}
bool m(const List& l, const au::Event& e) { std::string a, b; return matchList(l, e, a, b); }

void testMatch() {
    CHECK(m(mk(Kind::Aircraft, {"4840d6"}), adsb("4840D6", "KLM1023")), "hex, case");
    CHECK(m(mk(Kind::Aircraft, {"klm 1023"}), adsb("4840D6", "KLM1023")), "callsign with a space");
    CHECK(m(mk(Kind::Aircraft, {"KLM*"}), adsb("4840D6", "KLM1023")), "prefix");
    CHECK(!m(mk(Kind::Aircraft, {"KLM"}), adsb("4840D6", "KLM1023")), "no partial match without *");
    au::Event ac("acars.message");
    ac.str("tail", "A6-EDA").str("text", "HELLO");
    CHECK(m(mk(Kind::Aircraft, {"a6eda"}), ac), "registration without dash");
    CHECK(m(mk(Kind::Aircraft, {"A6-EDA"}), ac), "registration");
    CHECK(!m(mk(Kind::Ship, {"A6-EDA"}), ac), "ship list ignores ACARS");
    au::Event v("vdl2.message");
    v.str("src", "4840D6").str("dst", "100A12");
    CHECK(m(mk(Kind::Aircraft, {"100A12"}), v), "vdl2 dst");

    au::Event ais("ais.vessel");
    ais.num("mmsi", 219012345).str("name", "NORDIC STAR");
    CHECK(m(mk(Kind::Ship, {"219012345"}), ais), "mmsi number");
    CHECK(m(mk(Kind::Ship, {"nordic star"}), ais), "ship name");
    CHECK(!m(mk(Kind::Ship, {"21901234"}), ais), "mmsi prefix needs *");
    CHECK(m(mk(Kind::Ship, {"2190*"}), ais), "mmsi prefix");
    au::Event dsc("dsc.call");
    dsc.str("from_mmsi", "219000123").str("to_mmsi", "all");
    CHECK(m(mk(Kind::Ship, {"219000123"}), dsc), "dsc caller");
    {   // DSC distress: the special entry matches every distress call, nothing else; a watched ship in an acknowledgement matches too
        au::Event d("dsc.call");
        d.str("from_mmsi", "002320064").str("to_mmsi", "all ships").str("category", "distress").str("distress_mmsi", "232123456");
        CHECK(m(mk(Kind::Ship, {"DSC distress"}), d), "dsc distress entry");
        CHECK(m(mk(Kind::Ship, {"dsc-distress"}), d), "dsc distress entry, spelling");
        CHECK(!m(mk(Kind::Ship, {"DSC distress"}), dsc), "routine call is no distress");
        CHECK(m(mk(Kind::Ship, {"232123456"}), d), "ship named in the acknowledgement");
        CHECK(!m(mk(Kind::Ship, {"DSC distress"}), ais), "ais is no dsc");
    }

    au::Event ap("aprs.packet");
    ap.str("from", "LY1ABC-9").str("to", "APRS").str("info", "test");
    CHECK(m(mk(Kind::Aprs, {"LY1ABC"}), ap), "aprs any ssid");
    CHECK(m(mk(Kind::Aprs, {"ly1abc-9"}), ap), "aprs exact ssid");
    CHECK(!m(mk(Kind::Aprs, {"LY1ABC-7"}), ap), "aprs other ssid");
    CHECK(!m(mk(Kind::Aprs, {"LY1AB"}), ap), "aprs partial");

    au::Event f("ais.vessel");
    f.at(kT0, "AIS", 162.025e6);
    CHECK(m(mk(Kind::Frequency, {"162.025"}), f), "freq MHz");
    CHECK(m(mk(Kind::Frequency, {"162025 kHz"}), f), "freq kHz");
    CHECK(m(mk(Kind::Frequency, {"160-163"}), f), "freq range");
    CHECK(!m(mk(Kind::Frequency, {"162.1"}), f), "freq off");
    List wide = mk(Kind::Frequency, {"162.1"});
    wide.tolHz = 100e3;
    CHECK(m(wide, f), "freq tolerance");
    CHECK(!m(mk(Kind::Frequency, {"abc", ""}), f), "freq garbage");

    au::Event pg("pager.message");
    pg.at(kT0, "POCSAG", 152e6);
    pg.num("address", 1234567).str("text", "FIRE ALARM MAIN ST 12");
    CHECK(m(mk(Kind::Words, {"fire"}), pg), "word");
    CHECK(m(mk(Kind::Words, {"fire alarm"}), pg), "phrase");
    CHECK(!m(mk(Kind::Words, {"fir"}), pg), "no part of a word");
    CHECK(m(mk(Kind::Words, {"fir*"}), pg), "word prefix");
    CHECK(!m(mk(Kind::Words, {"fire"}), au::Event("signal.new")), "signal events carry no words");
    au::Event enc = pg;
    enc.flag("encrypted", true);
    CHECK(!m(mk(Kind::Words, {"fire"}), enc) && !m(mk(Kind::Frequency, {"152"}), enc), "encrypted never matches");
    CHECK(!m(mk(Kind::Words, {}), pg), "empty list");
}

void testQuiet() {
    Settings s;
    s.quietOn = true; s.quietStartMin = 22 * 60; s.quietEndMin = 7 * 60;
    const double midnight = 86400.0 * 20000;
    CHECK(inQuietHours(s, midnight + 23 * 3600), "23:00 quiet");
    CHECK(inQuietHours(s, midnight + 3 * 3600), "03:00 quiet");
    CHECK(!inQuietHours(s, midnight + 12 * 3600), "noon not quiet");
    CHECK(!inQuietHours(s, midnight + 7 * 3600), "07:00 ends");
    s.tzOffsetSec = 3 * 3600;
    CHECK(inQuietHours(s, midnight + 19 * 3600 + 60), "local 22:01");
    s.quietStartMin = 9 * 60; s.quietEndMin = 10 * 60; s.tzOffsetSec = 0;
    CHECK(inQuietHours(s, midnight + 9 * 3600 + 30 * 60) && !inQuietHours(s, midnight + 10 * 3600), "same-day window");
    s.quietOn = false;
    CHECK(!inQuietHours(s, midnight + 9 * 3600 + 30 * 60), "off");
}

void testBuild() {
    Channels c;
    Request r;
    std::string err;
    c.ntfyServer = "https://ntfy.sh/"; c.ntfyTopic = "my-topic_1"; c.ntfyToken = "tk_secret";
    CHECK(buildNtfy(c, "Watchlist: Planes \xc3\xa9", "body", r, err), "ntfy ok: %s", err.c_str());
    CHECK(r.url == "https://ntfy.sh/my-topic_1" && r.body == "body", "ntfy url");
    CHECK(r.headers.size() == 2 && r.headers[0] == "Title: Watchlist: Planes ?" && r.headers[1] == "Authorization: Bearer tk_secret", "ntfy headers");
    c.ntfyTopic = "bad topic/../x";
    CHECK(!buildNtfy(c, "t", "b", r, err), "ntfy bad topic");
    c.ntfyTopic = "ok"; c.ntfyServer = "file:///etc/passwd";
    CHECK(!buildNtfy(c, "t", "b", r, err), "ntfy not a web address");
    c.ntfyServer = "https://ntfy.sh"; c.ntfyToken = "a b\r\nX: y";
    CHECK(!buildNtfy(c, "t", "b", r, err), "ntfy token with a newline");

    c.telegramToken = "123456:ABC-def_GHI"; c.telegramChat = "-100123";
    CHECK(buildTelegram(c, "T", "line \"q\"", r, err), "telegram ok");
    CHECK(r.url == "https://api.telegram.org/bot123456:ABC-def_GHI/sendMessage", "telegram url");
    CHECK(r.body == "{\"chat_id\":\"-100123\",\"text\":\"T\\nline \\\"q\\\"\"}", "telegram body: %s", r.body.c_str());
    c.telegramToken = "nocolon";
    CHECK(!buildTelegram(c, "T", "b", r, err), "telegram bad token");
    c.telegramToken = "1:2"; c.telegramChat = "a b";
    CHECK(!buildTelegram(c, "T", "b", r, err), "telegram bad chat");

    c.webhookUrl = "https://example.org/hook"; c.webhookToken = "wh";
    Hit h; h.list = "L"; h.kind = "aircraft"; h.matched = "4840D6"; h.ident = "4840D6"; h.summary = "x \"y\"";
    au::Event ev("adsb.aircraft"); ev.at(kT0, "ADS-B", 1090e6);
    CHECK(buildWebhook(c, h, ev, r, err), "webhook ok");
    CHECK(r.body.find("\"source\":\"OnAir\"") != std::string::npos && r.body.find("\\\"y\\\"") != std::string::npos, "webhook json: %s", r.body.c_str());
    CHECK(r.headers.size() == 1 && r.headers[0] == "Authorization: Bearer wh", "webhook auth");
    c.webhookUrl = "ftp://x";
    CHECK(!buildWebhook(c, h, ev, r, err), "webhook not http");
}

void testWatcher() {
    Fake sink;
    Watcher w;
    double now = kT0;
    w.setClock([&] { return now; });
    w.setSink(&sink);
    Settings s;
    s.ch.notify = true; s.ch.sound = true;
    s.ch.ntfy = true; s.ch.ntfyTopic = "t"; s.ch.ntfyToken = "SECRETTOKEN";
    s.ch.telegram = true; s.ch.telegramToken = "9:TOKENTOKENTOKEN"; s.ch.telegramChat = "42";
    s.ch.webhook = true; s.ch.webhookUrl = "https://example.org/h?k=URLSECRET"; s.ch.webhookToken = "WHSECRET";
    s.cooldownSec = 300; s.maxPerHour = 3;
    w.setSettings(s);
    w.setLists({mk(Kind::Aircraft, {"4840D6", "A6-EDA"}, "Planes")});

    CHECK(w.process(adsb("100A12", "XYZ1")) == 0 && sink.notes.empty(), "no match, no alert");
    CHECK(w.process(adsb("4840D6", "KLM1023")) == 1, "match delivers");
    CHECK(sink.notes.size() == 1 && sink.sounds.size() == 1 && sink.posts.size() == 3, "all channels: %zu %zu %zu", sink.notes.size(), sink.sounds.size(), sink.posts.size());
    CHECK(sink.notes[0].find("Watchlist: Planes") == 0 && sink.notes[0].find("4840D6") != std::string::npos, "notification text: %s", sink.notes[0].c_str());
    now += 10;
    CHECK(w.process(adsb("4840D6", "KLM1023")) == 0, "repeat inside the cooldown is silent");
    CHECK(w.history().size() == 1, "repeat not in the history");
    now += 400;
    CHECK(w.process(adsb("4840D6", "KLM1023")) == 1, "after the cooldown again");
    au::Event b = adsb("4840D6", "KLM1023"); b.str("hex", "A6EDA0");
    au::Event ac("acars.message"); ac.str("tail", "A6-EDA").str("text", "T");
    CHECK(w.process(ac) == 1, "third alert");
    au::Event ac2("acars.message"); ac2.str("tail", "A6-EDA").str("text", "T");
    now += 400;
    CHECK(w.process(ac2) == 0, "rate limit: fourth in the hour is held back");
    const std::vector<Hit> hist = w.history();
    CHECK(hist.size() == 4 && !hist[3].delivered && hist[3].why == "rate limit", "rate limit in the history");
    now += 3700;
    CHECK(w.process(ac2) == 1, "an hour later alerts again");
    // tokens must never be in the history
    bool leak = false;
    for (const Hit& h : w.history()) {
        std::string all = h.list + h.summary + h.result + h.why + h.matched + h.ident + h.type;
        for (const char* t : {"SECRETTOKEN", "TOKENTOKEN", "URLSECRET", "WHSECRET"}) if (all.find(t) != std::string::npos) leak = true;
    }
    CHECK(!leak, "no token in the history");
    CHECK(sink.posts.size() >= 3 && sink.posts[0].label == "ntfy" && sink.posts[1].label == "telegram" && sink.posts[2].label == "webhook", "post order");

    // quiet hours: logged, not delivered
    Fake q;
    Watcher w2;
    w2.setClock([&] { return 86400.0 * 20000 + 23 * 3600; });
    w2.setSink(&q);
    Settings s2;
    s2.quietOn = true;
    w2.setSettings(s2);
    w2.setLists({mk(Kind::Words, {"fire"}, "Fire")});
    au::Event pg("pager.message"); pg.num("address", 1).str("text", "FIRE");
    CHECK(w2.process(pg) == 0 && q.notes.empty(), "quiet: nothing shown");
    CHECK(w2.history().size() == 1 && w2.history()[0].why == "quiet hours", "quiet: in the history");

    // disabled list / switched off
    w.clearHistory();
    w.setLists({[] { List l = mk(Kind::Aircraft, {"4840D6"}, "Off"); l.enabled = false; return l; }()});
    now += 1000;
    CHECK(w.process(adsb("4840D6", "K")) == 0, "disabled list");
    // a bad push channel is reported without the token
    Fake e;
    Watcher w3;
    w3.setSink(&e);
    Settings s3;
    s3.ch.notify = false; s3.ch.ntfy = true; s3.ch.ntfyTopic = "bad topic"; s3.ch.ntfyToken = "SECRETTOKEN";
    w3.setSettings(s3);
    w3.setLists({mk(Kind::Words, {"fire"}, "Fire")});
    CHECK(w3.process(pg) == 1 && e.posts.empty(), "bad channel sends nothing");
    CHECK(w3.history()[0].result.find("ntfy:") == 0 && w3.history()[0].result.find("SECRETTOKEN") == std::string::npos, "bad channel reported: %s", w3.history()[0].result.c_str());
    w3.sendTest();
    CHECK(w3.history().size() == 2, "test alert logged");
}

void testJson() {
    std::vector<List> l = {mk(Kind::Words, {"fire", "flood \"x\""}, "W"), mk(Kind::Frequency, {"162.025"}, "F")};
    l[1].tolHz = 12500;
    Settings s;
    s.maxPerHour = 7; s.cooldownSec = 60; s.quietOn = true; s.quietStartMin = 1380; s.quietEndMin = 360;
    s.ch.ntfy = true; s.ch.ntfyTopic = "abc"; s.ch.webhookToken = "tk";
    std::vector<List> l2;
    Settings s2;
    std::string err;
    CHECK(fromJson(toJson(l, s), l2, s2, err), "parse own output: %s", err.c_str());
    CHECK(l2.size() == 2 && l2[0].values.size() == 2 && l2[0].values[1] == "flood \"x\"" && l2[1].tolHz == 12500 && l2[1].kind == Kind::Frequency, "lists round trip");
    CHECK(s2.maxPerHour == 7 && s2.cooldownSec == 60 && s2.quietOn && s2.quietStartMin == 1380 && s2.ch.ntfy && s2.ch.ntfyTopic == "abc" && s2.ch.webhookToken == "tk", "settings round trip");
    CHECK(!fromJson("{not json", l2, s2, err), "bad json refused");
    CHECK(fromJson("{\"lists\":[{\"kind\":\"nonsense\",\"values\":[\"x\"]}]}", l2, s2, err) && l2.empty(), "unknown kind dropped");
    const auto v = splitValues("a, B;\n a ,,c\r\n  d e  ");
    CHECK(v.size() == 4 && v[0] == "a" && v[1] == "B" && v[2] == "c" && v[3] == "d e", "splitValues");
    CHECK(joinValues(v) == "a\nB\nc\nd e", "joinValues");
}

void testHook() {
    au::Engine eng;
    Fake sink;
    Watcher w;
    w.setSink(&sink);
    Settings s;
    s.ch.sound = false;
    w.setSettings(s);
    w.setLists({mk(Kind::Aircraft, {"4840D6"}, "P")});
    w.start();
    eng.setWatcher([&](const au::Event& e) { w.submit(e); }, au::kAllFamilies);
    CHECK(eng.wants(au::FAdsb), "the watcher makes the family wanted");
    eng.submit(adsb("4840D6", "KLM"));
    for (int i = 0; i < 100 && w.history().empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(w.history().size() == 1 && sink.notes.size() == 1, "event reaches the watcher through the engine");
    eng.setWatcher(nullptr, 0);
    CHECK(!eng.wants(au::FAdsb), "hook removed");
    w.stop();
}
}  // namespace

int main() {
    testMatch();
    testQuiet();
    testBuild();
    testWatcher();
    testJson();
    testHook();
    return testSummary("watchlist");
}
