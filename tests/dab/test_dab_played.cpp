// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Played list (round 4 #12): tags repeated every few seconds for the whole song are one entry; title before artist (and the other way) merge;
// two stations at once; the same song an hour later is a new entry; empty tags; search; the CSV round trip with commas and quotes.
#include "dect2/dab/played_log.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/recipes.h"
#include "../common/test_util.h"
#include <map>
#include <vector>
using namespace dect2;

namespace au = dect2::automation;
struct Sink : au::ActionSink {
    std::vector<std::string> kinds;
    bool perform(const std::string& k, const std::map<std::string, std::string>&, std::string& d) override { kinds.push_back(k); d = "ok"; return true; }
    int count(const char* k) const { int n = 0; for (auto& x : kinds) n += x == k; return n; }
};
static au::Rule recipe(const char* name, const char* when = nullptr) {
    for (const auto& r : au::builtinRecipes()) if (r.kind == au::Recipe::KindRule && r.rule.name == name) { au::Rule x = r.rule; x.enabled = true; if (when) x.when = when; return x; }
    CHECK(false, "recipe %s missing", name);
    return au::Rule();
}
static void testRecipes(int64_t t0) {
    auto ev = [&](const char* type, std::map<std::string, std::string> s, std::map<std::string, bool> b = {}) {
        au::Event e(type); e.at((double)t0, "FM", 93.9e6);
        for (auto& kv : s) e.str(kv.first, kv.second);
        for (auto& kv : b) e.flag(kv.first, kv.second);
        return e;
    };
    struct C { const char* name; const char* when; au::Event fire, quiet; const char* rec; };
    const C cs[] = {
        {"Favourite artist on the radio", "artist contains 'queen'", ev("broadcast.now_playing", {{"station", "R1"}, {"artist", "Queen"}, {"title", "X"}}), ev("broadcast.now_playing", {{"station", "R1"}, {"artist", "ABBA"}}), "record_audio"},
        {"Word in the radio text", nullptr, ev("broadcast.text", {{"station", "R1"}, {"text", "Traffic: A1 closed"}}), ev("broadcast.text", {{"station", "R1"}, {"text", "Weather"}}), nullptr},
        {"Traffic announcement", nullptr, ev("broadcast.text", {{"station", "R1"}}, {{"ta_start", true}, {"ta", true}}), ev("broadcast.text", {{"station", "R1"}}, {{"ta_start", false}, {"ta", true}}), "record_audio"},
    };
    for (const C& c : cs) {
        au::Engine eng; Sink sink; double now = (double)t0;
        eng.setSink(&sink); eng.setClock([&] { return now; }); eng.setMaxActionsPerMinute(0);
        const au::Rule r = recipe(c.name, c.when);
        CHECK(au::validateRule(r).empty(), "%s validates: %s", c.name, au::validateRule(r).c_str());
        eng.setRules({r});
        eng.process(c.quiet);
        CHECK(sink.count("notify") == 0, "%s quiet", c.name);
        eng.process(c.fire);
        CHECK(sink.count("notify") == 1 && (!c.rec || sink.count(c.rec) == 1), "%s fires", c.name);
    }
    CHECK(au::familyOf("broadcast.text") == au::FBroadcast && au::findEventInfo("broadcast.now_playing"), "family and catalog");
}

int main() {
    const int64_t t0 = 1'800'000'000;
    PlayedLog l;
    int news = 0;
    // song 1 on FM: the title comes 4 s before the artist, then both repeat every 10 s for 4 minutes
    news += l.note("FM", "RADIO 1 ", 93.9, "", "Bohemian Rhapsody", t0);
    news += l.note("FM", "RADIO 1", 93.9, "Queen", "Bohemian Rhapsody", t0 + 4);
    for (int s = 10; s < 240; s += 10) news += l.note("FM", "RADIO 1", 93.9, "Queen", "Bohemian Rhapsody", t0 + s);
    // meanwhile DAB, artist first
    news += l.note("DAB", "Classic", 222.064, "Mozart", "", t0 + 20);
    news += l.note("DAB", "Classic", 222.064, "Mozart", "Requiem", t0 + 25);
    // song 2 on FM, then song 1 again 40 minutes later
    news += l.note("FM", "RADIO 1", 93.9, "ABBA", "Waterloo", t0 + 250);
    news += l.note("FM", "RADIO 1", 93.9, "abba", "WATERLOO", t0 + 400);   // same song, other case
    news += l.note("FM", "RADIO 1", 93.9, "Queen", "Bohemian Rhapsody", t0 + 2650);
    news += l.note("FM", "RADIO 1", 93.9, "", "", t0 + 2700);
    CHECK(news == 4 && l.items().size() == 4, "4 entries (%d new, %zu kept)", news, l.items().size());
    if (l.items().size() == 4) {
        const auto& a = l.items()[0];
        CHECK(a.station == "RADIO 1" && a.artist == "Queen" && a.title == "Bohemian Rhapsody" && a.utc == t0 && a.lastUtc == t0 + 230, "song 1 merged, first and last heard");
        CHECK(l.items()[1].source == "DAB" && l.items()[1].artist == "Mozart" && l.items()[1].title == "Requiem", "DAB artist first, merged");
        CHECK(l.items()[3].utc == t0 + 2650, "the same song 40 minutes later is a new entry");
    }
    const auto f = l.find("queen");
    CHECK(f.size() == 2 && f[0].utc == t0 + 2650, "search newest first");
    CHECK(l.find("radio 1 waterloo").size() == 1 && l.find("mozart requiem").size() == 1 && l.find("x").empty() && l.find("").size() == 4, "search words");
    // CSV
    PlayedItem p; p.utc = t0; p.source = "FM"; p.station = "A, B"; p.freqMhz = 100.1; p.artist = "Sly \"Stone\""; p.title = "Title, Part 2";
    const auto back = playedFromCsv(playedCsv({p}) + "garbage\n");
    CHECK(back.size() == 1 && back[0].station == "A, B" && back[0].artist == "Sly \"Stone\"" && back[0].title == "Title, Part 2" && back[0].utc == t0 && back[0].freqMhz == 100.1, "CSV round trip");
    // the cap
    PlayedLog big;
    for (size_t i = 0; i < PlayedLog::kKeep + 10; i++) big.note("FM", "S", 1, "A", std::to_string(i), t0 + (int64_t)i * 100);
    CHECK(big.items().size() == PlayedLog::kKeep && big.items().back().title == std::to_string(PlayedLog::kKeep + 9), "kept at most %zu", PlayedLog::kKeep);
    testRecipes(t0);
    return testSummary("dab_played");
}
