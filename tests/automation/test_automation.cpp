// Automations: the expression language (also malformed and hostile input), rules, cooldowns and limits, geofences, templates and JSON escaping,
// recipe files, the Lua sandbox (escape attempts, infinite loops, memory bombs) and the engine with a fake action sink.
#include "dect2/automation/catalog.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/expr.h"
#include "dect2/automation/json_mini.h"
#include "dect2/automation/recipes.h"
#include "dect2/automation/rules.h"
#include "dect2/automation/script.h"
#include "../common/test_util.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace dect2::automation;
namespace fs = std::filesystem;

namespace {
constexpr double kT0 = 1792232100.0;   // 2026-10-17 10:15:00 UTC, a Saturday

Event aircraft(const char* hex, int squawk, double lat = 54.69, double lon = 25.28, double alt = 12000) {
    Event e("adsb.aircraft");
    e.at(kT0, "ADS-B", 1090e6);
    e.str("hex", hex).str("callsign", "TST1").num("squawk", squawk).num("lat", lat).num("lon", lon).num("alt_ft", alt);
    return e;
}

struct Fx {
    Event ev;
    Env env;
    Fx(Event e = Event("x")) : ev(std::move(e)) { env.ev = &ev; env.now = kT0; env.tzOffsetSec = 0; env.stationSet = true; env.stationLat = 54.6872; env.stationLon = 25.2797; }
};

// evaluates to a bool; "ERR" when it does not parse or fails
std::string ev(const std::string& expr, Fx& fx, std::string* detail = nullptr) {
    ExprError pe;
    auto e = Expr::parse(expr, pe);
    if (!e) { if (detail) *detail = pe.text(); return "PARSE"; }
    std::string err;
    const Value v = e->eval(fx.env, &err);
    if (!err.empty()) { if (detail) *detail = err; return "ERR"; }
    return v.null() ? "null" : v.text();
}

void testExpr() {
    Fx fx(aircraft("4840D6", 7700));
    fx.ev.str("text", "Fire at MAIN street").num("n", 5).flag("ground", false);
    CHECK(ev("1 + 2 * 3", fx) == "7", "precedence");
    CHECK(ev("(1 + 2) * 3", fx) == "9", "parens");
    CHECK(ev("10 % 4 == 2", fx) == "true", "modulo");
    CHECK(ev("-n + 8", fx) == "3", "unary minus on a field");
    CHECK(ev("118.1MHz == 118100000", fx) == "true", "MHz unit");
    CHECK(ev("5min == 300", fx) == "true", "minutes unit");
    CHECK(ev("1km < 2000", fx) == "true", "km unit");
    CHECK(ev("squawk == 7700", fx) == "true", "field compare");
    CHECK(ev("squawk in [7500, 7600, 7700]", fx) == "true", "in list");
    CHECK(ev("squawk in [1000]", fx) == "false", "not in list");
    CHECK(ev("squawk == 7700 and not ground", fx) == "true", "and not");
    CHECK(ev("squawk == 1 or hex == '4840D6'", fx) == "true", "or");
    CHECK(ev("not (squawk == 1 or n == 4)", fx) == "true", "not parens");
    CHECK(ev("text contains 'fire'", fx) == "true", "contains is case-insensitive");
    CHECK(ev("text startswith 'FIRE'", fx) == "true", "startswith");
    CHECK(ev("text endswith 'street'", fx) == "true", "endswith");
    CHECK(ev("text matches 'fire*street'", fx) == "true", "glob");
    CHECK(ev("text matches 'fire?'", fx) == "false", "glob whole string");
    CHECK(ev("missing > 3", fx) == "false", "missing field compares false");
    CHECK(ev("missing == null", fx) == "true", "null compare");
    CHECK(ev("has('squawk') and not has('nothing')", fx) == "true", "has()");
    CHECK(ev("lower(hex) == '4840d6'", fx) == "true", "lower");
    CHECK(ev("len(text)", fx) == "19", "len");
    CHECK(ev("'a' + 'b' == 'ab'", fx) == "true", "concat");
    CHECK(ev("max(2, 9) - min(2, 9) + abs(-1)", fx) == "8", "min max abs");
    CHECK(ev("squawk == '7700'", fx) == "true", "text that looks like a number");
    CHECK(ev("type == 'adsb.aircraft' and mode == 'ADS-B' and freq_mhz > 1089", fx) == "true", "built-in fields");
    CHECK(ev("", fx) == "true", "empty is true");
    CHECK(ev("distance_km(51.5074, -0.1278, 48.8566, 2.3522) > 340 and distance_km(51.5074, -0.1278, 48.8566, 2.3522) < 348", fx) == "true", "London-Paris");
    CHECK(ev("distance_km(54.6872, 25.2797) < 0.01", fx) == "true", "distance from the station");
    fx.env.stationLat = 0; fx.env.stationLon = 0;
    CHECK(ev("abs(bearing_deg(0, 10) - 90) < 0.5", fx) == "true", "bearing");
    fx.env.stationLat = 54.6872; fx.env.stationLon = 25.2797;
    CHECK(ev("within(54.69, 25.28, 54.6872, 25.2797, 1)", fx) == "true", "within");
    CHECK(ev("within(54.69, 25.28, 55.6872, 25.2797, 1)", fx) == "false", "not within");
    CHECK(ev("inside(54.5, 25.5, '54,25; 54,26; 55,26; 55,25')", fx) == "true", "inside polygon");
    CHECK(ev("inside(56, 25.5, '54,25; 54,26; 55,26; 55,25')", fx) == "false", "outside polygon");
    CHECK(ev("hour() == 10 and minute() == 15 and weekday() == 6", fx) == "true", "clock functions (Saturday = 6)");
    fx.env.tzOffsetSec = 3 * 3600;
    CHECK(ev("hour() == 13", fx) == "true", "local time zone");
    fx.env.stationSet = false;
    CHECK(ev("distance_km(1, 2) < 5", fx) == "false", "no station: distance is null, condition false");
    fx.env.stationSet = true;
    // numbers of a text field
    fx.ev.str("alt", "36000");
    CHECK(ev("alt > 30000", fx) == "true", "numeric text compares as number");
}

void testExprHostile() {
    Fx fx(aircraft("A", 1));
    std::string d;
    CHECK(ev("1 / 0", fx, &d) == "ERR" && d.find("division") != std::string::npos, "division by zero is a fault: %s", d.c_str());
    CHECK(ev("5 % 0", fx, &d) == "ERR", "modulo by zero");
    CHECK(ev("1e308 * 1e308", fx, &d) == "ERR", "overflow is a fault");
    {
        ExprError pe;
        CHECK(!Expr::parse("1 +", pe) && pe.pos >= 2, "incomplete: pos %d %s", pe.pos, pe.msg.c_str());
        CHECK(!Expr::parse("(1 + 2", pe) && pe.msg.find("')'") != std::string::npos, "missing paren: %s", pe.msg.c_str());
        CHECK(!Expr::parse("foo(1)", pe) && pe.msg.find("unknown function") != std::string::npos, "unknown function");
        CHECK(!Expr::parse("5furlongs", pe) && pe.msg.find("unit") != std::string::npos && pe.pos == 1, "unknown unit at the unit: pos %d", pe.pos);
        CHECK(!Expr::parse("1 2", pe) && pe.pos == 2, "two values: pos %d", pe.pos);
        CHECK(!Expr::parse("hex == 'abc", pe) && pe.msg.find("unterminated") != std::string::npos, "unterminated text");
        CHECK(!Expr::parse("hex in 5", pe), "in needs a list");
        CHECK(!Expr::parse("min(1)", pe), "arity");
        CHECK(!Expr::parse("a ? b", pe), "unknown character");
        CHECK(!Expr::parse("and", pe), "keyword alone");
        CHECK(!Expr::parse("1 == == 2", pe), "double operator");
    }
    {   // depth and size
        ExprError pe;
        std::string deep(10000, '(');
        deep += "1" + std::string(10000, ')');
        CHECK(!Expr::parse(deep, pe), "10000 parens refused (no stack overflow)");
        std::string nots;
        for (int i = 0; i < 5000; i++) nots += "not ";
        CHECK(!Expr::parse(nots + "true", pe), "5000 nots refused");
        std::string chain = "1";
        for (int i = 0; i < 1000; i++) chain += "+1";
        CHECK(!Expr::parse(chain, pe), "1000 terms refused (node limit)");
        std::string list = "squawk in [";
        for (int i = 0; i < 600; i++) list += (i ? "," : "") + std::to_string(i);
        CHECK(!Expr::parse(list + "]", pe), "600 list items refused");
        CHECK(!Expr::parse(std::string(3000, 'a'), pe), "3000 characters refused");
        std::string nest = "";
        for (int i = 0; i < 100; i++) nest += "abs(";
        CHECK(!Expr::parse(nest + "1" + std::string(100, ')'), pe), "call nesting refused");
    }
    {   // a huge text field
        Fx big(Event("x"));
        Value v; v.t = Value::Str; v.s = std::string(2'000'000, 'a'); v.s += "needle";
        big.ev.f["text"] = v;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(ev("text contains 'needle' and text endswith 'needle' and len(text) > 1000000", big) == "true", "huge text");
        CHECK(ev("text matches '*a*a*a*a*a*a*a*a*b'", big) == "false", "pathological glob");
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(s < 3, "huge text took %.2f s", s);
    }
    {   // event field text is cut
        Event e("x"); e.str("t", std::string(100000, 'z'));
        CHECK(e.f["t"].s.size() == kMaxFieldText, "Event::str cuts at %zu", kMaxFieldText);
    }
    CHECK(ev("text contains null", fx) == "false", "null operand");
    CHECK(ev("'\\u0000' == ''", fx) != "ERR", "odd text does not fault");
    CHECK(geo::distanceKm(0, 0, 0, 180) > 20000 && geo::distanceKm(0, 0, 0, 180) < 20100, "antipodal distance");
    CHECK(geo::distanceKm(90, 0, -90, 0) < 20100, "pole to pole");
    CHECK(!globMatch("a", ""), "glob empty text");
    CHECK(globMatch("*", ""), "glob star matches empty");
}

void testGeometry() {
    std::vector<std::pair<double, double>> p;
    CHECK(geo::parsePolygon("54,25; 54,26; 55,26", p) && p.size() == 3, "triangle");
    CHECK(!geo::parsePolygon("54,25; 54,26", p), "two points");
    CHECK(!geo::parsePolygon("54,25; 54,26; x,y", p), "bad number");
    CHECK(!geo::parsePolygon("54,25; 54,26; 95,26", p), "latitude out of range");
    std::string many;
    for (int i = 0; i < 70; i++) many += "10,10;";
    CHECK(!geo::parsePolygon(many, p), "more than 64 points");
    geo::parsePolygon("54,25; 54,26; 55,26; 55,25", p);
    CHECK(geo::pointInPolygon(54.5, 25.5, p) && !geo::pointInPolygon(53.9, 25.5, p) && !geo::pointInPolygon(54.5, 26.1, p), "square");
    CHECK(std::fabs(geo::bearingDeg(0, 0, 10, 0)) < 0.01 && std::fabs(geo::bearingDeg(0, 0, 0, -10) - 270) < 0.5, "north and west");
}

void testTemplates() {
    Event e("pager.message");
    e.at(kT0, "POCSAG", 152.0e6);
    e.str("text", "He said \"fire\"\nline2 \\ \x01 tab\t").num("address", 1234567).flag("damaged", true);
    CHECK(renderTemplate("{{ address }}: {{text}} {{nothing}}|", e, "R") == "1234567: He said \"fire\"\nline2 \\ \x01 tab\t |", "plain");
    const std::string j = renderTemplate("{\"t\":\"{{text}}\"}", e, "R", true);
    json::Value v; std::string err;
    CHECK(json::parse(j, v, err), "escaped JSON parses: %s | %s", err.c_str(), j.c_str());
    CHECK(v.str("t") == e.f["text"].s, "JSON escaping round trip");
    CHECK(renderTemplate("{{type}} {{mode}} {{freq_mhz}} {{time_iso}} {{rule}} {{damaged}}", e, "My rule") == "pager.message POCSAG 152.0000 2026-10-17T10:15:00Z My rule true", "built-ins: %s", renderTemplate("{{type}} {{mode}} {{freq_mhz}} {{time_iso}} {{rule}} {{damaged}}", e, "My rule").c_str());
    Event inj("x"); inj.str("a", "{{b}}").str("b", "SECRET");
    CHECK(renderTemplate("{{a}}", inj, "R") == "{{b}}", "values are not expanded again");
    CHECK(renderTemplate("a {{ unterminated", inj, "R") == "a {{ unterminated", "unterminated");
    CHECK(renderTemplate(std::string(100, '{') + "{a}}", inj, "R").size() > 0, "braces");
    Event bad("x"); bad.str("t", std::string("ok \xC3\xA9 bad \xFF\xFE tail \xE2\x82"));
    const std::string jb = renderTemplate("\"{{t}}\"", bad, "R", true);
    CHECK(json::parse(jb, v, err), "invalid UTF-8 still gives valid JSON: %s", err.c_str());
    CHECK(v.s.find("\xC3\xA9") != std::string::npos && v.s.find("\xEF\xBF\xBD") != std::string::npos, "valid UTF-8 kept, bad bytes replaced");
    Event huge("x"); huge.str("t", std::string(1999, 'q'));
    std::string tpl;
    for (int i = 0; i < 100; i++) tpl += "{{t}}";
    CHECK(renderTemplate(tpl, huge, "R").size() <= 8000, "rendered text is capped");
    CHECK(csvField("a,b") == "\"a,b\"" && csvField("say \"x\"") == "\"say \"\"x\"\"\"" && csvField("plain") == "plain", "csv quoting");
    CHECK(csvField("=1+1") == "'=1+1" && csvField("@SUM(A1)") == "'@SUM(A1)" && csvField("-2+3") == "'-2+3", "csv formula guard");
}

void testWebhook() {
    Event e("adsb.aircraft");
    e.at(kT0, "ADS-B", 1090e6);
    e.str("callsign", "A\"B\\C\n").num("squawk", 7700).flag("ground", false);
    std::string url, body, err;
    json::Value v;
    ActionSpec g; g.type = "webhook"; g.p["url"] = "https://example.org/hook";
    CHECK(buildWebhook(g, e, "Rule \"1\"", url, body, err) && url == "https://example.org/hook", "generic: %s", err.c_str());
    CHECK(json::parse(body, v, err) && v.str("rule") == "Rule \"1\"" && v.str("type") == "adsb.aircraft", "default body is JSON of the event");
    CHECK(v.get("fields") && v.get("fields")->str("callsign") == "A\"B\\C\n" && v.get("fields")->num("squawk") == 7700, "default body fields");
    g.p["body"] = "{\"c\":\"{{callsign}}\",\"s\":{{squawk}}}";
    CHECK(buildWebhook(g, e, "R", url, body, err) && json::parse(body, v, err) && v.str("c") == "A\"B\\C\n" && v.num("s") == 7700, "templated body: %s", body.c_str());
    ActionSpec d; d.type = "webhook"; d.p["preset"] = "discord"; d.p["url"] = "https://discord.com/api/webhooks/1/x"; d.p["text"] = "{{callsign}} squawks {{squawk}}";
    CHECK(buildWebhook(d, e, "R", url, body, err) && json::parse(body, v, err) && v.str("content") == "A\"B\\C\n squawks 7700", "discord");
    d.p["preset"] = "slack";
    CHECK(buildWebhook(d, e, "R", url, body, err) && json::parse(body, v, err) && v.str("text") == "A\"B\\C\n squawks 7700", "slack");
    ActionSpec n; n.type = "webhook"; n.p["preset"] = "ntfy"; n.p["topic"] = "my\"topic"; n.p["title"] = "T {{squawk}}"; n.p["text"] = "{{callsign}}";
    CHECK(buildWebhook(n, e, "R", url, body, err) && url == "https://ntfy.sh" && json::parse(body, v, err) && v.str("topic") == "my\"topic" && v.str("title") == "T 7700", "ntfy");
    n.p.erase("topic");
    CHECK(!buildWebhook(n, e, "R", url, body, err), "ntfy needs a topic");
    ActionSpec t; t.type = "webhook"; t.p["preset"] = "telegram"; t.p["token"] = "123:ABC-def_"; t.p["chat_id"] = "-100123"; t.p["text"] = "{{callsign}}";
    CHECK(buildWebhook(t, e, "R", url, body, err) && url == "https://api.telegram.org/bot123:ABC-def_/sendMessage" && json::parse(body, v, err) && v.str("chat_id") == "-100123", "telegram");
    t.p["token"] = "123/../x";
    CHECK(!buildWebhook(t, e, "R", url, body, err), "telegram token with a slash refused");
    g.p["url"] = "file:///etc/passwd";
    CHECK(!buildWebhook(g, e, "R", url, body, err), "file:// refused");
    g.p["url"] = "https://x.org/a b";
    CHECK(!buildWebhook(g, e, "R", url, body, err), "space in URL refused");
    g.p["url"] = "javascript:alert(1)";
    CHECK(!buildWebhook(g, e, "R", url, body, err), "javascript: refused");
}

Rule simple(const char* name, const char* trigger, const char* when, ActionSpec a) {
    Rule r;
    r.name = name; r.id = makeId(name); r.trigger = trigger; r.when = when; r.cooldownSec = 0; r.maxPerHour = 0;
    r.actions.push_back(std::move(a));
    return r;
}
ActionSpec notify(const char* body) { ActionSpec a; a.type = "notify"; a.p["title"] = "T"; a.p["body"] = body; return a; }

void testRecipes() {
    const auto all = builtinRecipes();
    CHECK(all.size() >= 12, "built-in recipes: %zu", all.size());
    std::set<std::string> ids;
    for (const Recipe& r : all) {
        CHECK(ids.insert(r.kind == Recipe::KindRule ? r.rule.id : r.script.id).second, "duplicate recipe id %s", r.title().c_str());
        if (r.kind == Recipe::KindRule) {
            const std::string e = validateRule(r.rule);
            CHECK(e.empty(), "built-in rule '%s': %s", r.rule.name.c_str(), e.c_str());
            CHECK(!r.rule.enabled, "built-in rules start switched off");
        } else {
            const std::string e = checkScriptSyntax(r.script.code);
            CHECK(e.empty(), "built-in script '%s': %s", r.script.name.c_str(), e.c_str());
        }
        const std::string text = recipeToText(r);
        Recipe back; std::string err;
        CHECK(recipeFromText(text, back, err, true), "round trip of '%s': %s", r.title().c_str(), err.c_str());
        CHECK(recipeToText(back) == text, "round trip is stable for '%s'", r.title().c_str());
        Recipe imp;
        CHECK(recipeFromText(text.find("\"enabled\": true") != std::string::npos ? text : text, imp, err) && !(imp.kind == Recipe::KindRule ? imp.rule.enabled : imp.script.enabled), "imported recipes come in switched off");
    }
    // an enabled recipe from outside still arrives switched off
    Recipe r = all[0]; r.rule.enabled = true;
    Recipe imp; std::string err;
    CHECK(recipeFromText(recipeToText(r), imp, err) && !imp.rule.enabled, "enabled flag of a file is ignored on import");
    CHECK(recipeFromText(recipeToText(r), imp, err, true) && imp.rule.enabled, "kept from the program's own store");
    CHECK(!recipeFromText("not json", imp, err) && !err.empty(), "not JSON: %s", err.c_str());
    CHECK(!recipeFromText("{\"format\":\"other\"}", imp, err), "wrong format");
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":99,\"kind\":\"rule\"}", imp, err) && err.find("newer") != std::string::npos, "newer version");
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"rule\",\"name\":\"x\",\"trigger\":\"adsb.aircraft\",\"actions\":[{\"type\":\"format_disk\"}]}", imp, err) && err.find("unknown action") != std::string::npos, "unknown action: %s", err.c_str());
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"rule\",\"name\":\"x\",\"trigger\":\"adsb.aircraft\",\"when\":\"squawk ==\",\"actions\":[{\"type\":\"sound\"}]}", imp, err) && err.find("character") != std::string::npos, "bad condition reported with position: %s", err.c_str());
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"rule\",\"name\":\"x\",\"trigger\":\"nope.event\",\"actions\":[{\"type\":\"sound\"}]}", imp, err), "unknown trigger");
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"rule\",\"name\":\"x\",\"trigger\":\"*\",\"actions\":[]}", imp, err), "no actions");
    std::string many = "{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"rule\",\"name\":\"x\",\"trigger\":\"*\",\"actions\":[";
    for (int i = 0; i < 9; i++) many += std::string(i ? "," : "") + "{\"type\":\"sound\"}";
    CHECK(!recipeFromText(many + "]}", imp, err), "9 actions");
    CHECK(!recipeFromText(std::string(40, '[') + std::string(40, ']'), imp, err), "deep JSON refused");
    std::string deep = "{\"a\":";
    for (int i = 0; i < 100000; i++) deep += "[";
    CHECK(!recipeFromText(deep, imp, err), "100000 nested arrays refused without a crash");
    CHECK(!recipeFromText(std::string(2'000'000, ' '), imp, err), "2 MB refused");
    CHECK(!recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"script\",\"name\":\"s\",\"events\":[\"zzz.x\"],\"code\":\"function on_event(e) end\"}", imp, err), "script with unknown event");
    CHECK(recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"script\",\"name\":\"s\",\"code\":\"function on_event(e) end\"}", imp, err) && imp.script.id == "s", "script, id made from the name");
    CHECK(makeId("Squawk 7700!") == "squawk-7700" && makeId("!!!") == "rule" && makeId("Ünï") == "n" , "makeId: %s", makeId("Ünï").c_str());
    CHECK(validId("abc-1_2") && !validId("../x") && !validId("") && !validId(std::string(49, 'a')), "validId");
    // path-like ids from a file are replaced
    CHECK(recipeFromText("{\"format\":\"onair-automation\",\"version\":1,\"kind\":\"script\",\"id\":\"../../evil\",\"name\":\"Nice\",\"code\":\"function on_event(e) end\"}", imp, err) && imp.script.id == "nice", "bad id replaced");
}

struct Sink : ActionSink {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> calls;
    bool fail = false;
    std::mutex mu;
    bool perform(const std::string& k, const std::map<std::string, std::string>& p, std::string& d) override {
        std::lock_guard<std::mutex> lk(mu);
        calls.push_back({k, p});
        if (fail) { d = "boom"; return false; }
        d = "ok";
        return true;
    }
    size_t count() { std::lock_guard<std::mutex> lk(mu); return calls.size(); }
    size_t count(const char* kind) { std::lock_guard<std::mutex> lk(mu); size_t n = 0; for (auto& c : calls) n += c.first == kind; return n; }
};

struct Rig {
    Engine eng;
    Sink sink;
    double now = kT0;
    Rig() {
        eng.setSink(&sink);
        eng.setClock([this] { return now; });
        eng.setStation(true, 54.6872, 25.2797);
        eng.setMaxActionsPerMinute(0);
    }
};

void testConditionRows() {
    Rule r = simple("Rows", "adsb.aircraft", "", notify("x"));
    r.conds = {{"squawk", "in", "7500, 7600,7700"}, {"callsign", "startswith", "o'brien"}, {"alt_ft", "<", "5000"}, {"hex", "==", "4840D6"}, {"lat", "has", ""}, {"freq", ">=", "1.09GHz"}, {"callsign", "==", "12ab"}};
    CHECK(conditionExpr(r) == "squawk in [7500, 7600, 7700] and callsign startswith 'o\\'brien' and alt_ft < 5000 and hex == '4840D6' and has('lat') and freq >= 1.09GHz and callsign == '12ab'", "built expression: %s", conditionExpr(r).c_str());
    CHECK(validateRule(r).empty(), "rows validate: %s", validateRule(r).c_str());
    r.condAny = true; r.when = "alt_ft > 1";
    CHECK(conditionExpr(r).find(" or ") != std::string::npos && conditionExpr(r).rfind(") and (alt_ft > 1)") != std::string::npos, "any-match and the advanced part: %s", conditionExpr(r).c_str());
    Rig g;
    Rule t = simple("RowsRun", "adsb.aircraft", "", notify("fired"));
    t.conds = {{"squawk", "in", "7700,7600"}, {"alt_ft", ">", "10000"}};
    g.eng.setRules({t});
    g.eng.process(aircraft("A", 7700, 54.7, 25.3, 12000));
    g.eng.process(aircraft("A", 7700, 54.7, 25.3, 9000));
    g.eng.process(aircraft("A", 1200, 54.7, 25.3, 12000));
    CHECK(g.sink.count() == 1, "rows decide: %zu", g.sink.count());
    Rule bad = t; bad.conds[0].field = "a b";
    CHECK(!validateRule(bad).empty(), "bad field refused");
    bad = t; bad.conds[0].op = "~=";
    CHECK(!validateRule(bad).empty(), "bad operator refused");
    bad = t; bad.conds[0].field = "x'); drop"; 
    CHECK(!validateRule(bad).empty(), "injection through the field refused");
    bad = t; bad.conds[1].value = "1' or 'a'=='a";
    Event e = aircraft("A", 1, 0, 0, 5);
    Rig g2; g2.eng.setRules({bad}); g2.eng.process(e);
    CHECK(g2.sink.count() == 0, "a value cannot break out of its quotes");
    Recipe a, b; std::string err;
    a.kind = Recipe::KindRule; a.rule = t;
    CHECK(recipeFromText(recipeToText(a), b, err, true) && b.rule.conds.size() == 2 && b.rule.conds[0].value == "7700,7600", "rows survive the file: %s", err.c_str());
}

void testEngine() {
    {   // firing, filter, templates
        Rig r;
        r.eng.setRules({simple("Squawk", "adsb.aircraft", "squawk in [7500, 7600, 7700]", notify("{{callsign}} {{squawk}}"))});
        r.eng.process(aircraft("A", 1000));
        CHECK(r.sink.count() == 0, "no match, no action");
        r.eng.process(aircraft("A", 7700));
        CHECK(r.sink.count() == 1 && r.sink.calls[0].second.at("body") == "TST1 7700", "fires with the template filled");
        CHECK(r.eng.stats("squawk").fired == 1 && r.eng.stats("squawk").matched == 1, "stats");
        CHECK(r.eng.firings().size() == 1 && r.eng.firings()[0].rule == "Squawk" && r.eng.firings()[0].actions.size() == 1, "firing log");
        Event other("pager.message"); other.str("text", "x");
        r.eng.process(other);
        CHECK(r.sink.count() == 1, "other event type ignored");
    }
    {   // family trigger and wildcard
        Rig r;
        Rule a = simple("All adsb", "adsb.*", "", notify("a"));
        Rule b = simple("All", "*", "", notify("b"));
        r.eng.setRules({a, b});
        r.eng.process(aircraft("A", 1));
        Event n = aircraft("A", 1); n.type = "adsb.new";
        r.eng.process(n);
        Event p("pager.message");
        r.eng.process(p);
        CHECK(r.sink.count() == 5, "adsb.* matches two types, * all three: %zu", r.sink.count());
        CHECK(r.sink.calls[0].second.at("body") == "a" && r.sink.calls[1].second.at("body") == "b", "rules run in list order");
    }
    {   // cooldown per key, restart after it
        Rig r;
        Rule x = simple("Cool", "adsb.aircraft", "", notify("x"));
        x.cooldownSec = 600; x.cooldownKey = "hex";
        r.eng.setRules({x});
        r.eng.process(aircraft("A", 1)); r.eng.process(aircraft("A", 1)); r.eng.process(aircraft("B", 1));
        CHECK(r.sink.count() == 2, "one cooldown per aircraft: %zu", r.sink.count());
        r.now += 601;
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 3, "fires again after the cooldown");
        CHECK(r.eng.stats("cool").suppressed == 1, "suppressed counted");
        Rule y = x; y.cooldownKey = ""; y.id = "cool2"; y.name = "Cool2";
        Rig r2;
        r2.eng.setRules({y});
        r2.eng.process(aircraft("A", 1)); r2.eng.process(aircraft("B", 1));
        CHECK(r2.sink.count() == 1, "one cooldown for the whole rule");
    }
    {   // per hour limit
        Rig r;
        Rule x = simple("Hourly", "adsb.aircraft", "", notify("x"));
        x.maxPerHour = 3;
        r.eng.setRules({x});
        for (int i = 0; i < 10; i++) { r.now += 1; r.eng.process(aircraft("A", 1)); }
        CHECK(r.sink.count() == 3, "3 per hour: %zu", r.sink.count());
        r.now += 3700;
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 4, "the hour passes");
    }
    {   // global throttle
        Rig r;
        r.eng.setMaxActionsPerMinute(5);
        r.eng.setRules({simple("A", "adsb.aircraft", "", notify("x")), simple("B", "adsb.aircraft", "", notify("y"))});
        for (int i = 0; i < 10; i++) r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 5, "5 firings per minute for all rules: %zu", r.sink.count());
        r.now += 61;
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 7, "minute passes: %zu", r.sink.count());
    }
    {   // time window, with a time zone and across midnight
        Rig r;
        Rule x = simple("Night", "adsb.aircraft", "", notify("x"));
        x.windowFromHour = 22; x.windowToHour = 6;
        r.eng.setRules({x});
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 0, "10:15 UTC is outside 22 to 6");
        r.eng.setTzOffset(13 * 3600);   // 23:15 local
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 1, "23:15 local is inside");
        r.eng.setTzOffset(-5 * 3600);   // 05:15 local
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 2, "05:15 local is inside");
        Rule day = simple("Day", "adsb.aircraft", "", notify("d"));
        day.windowFromHour = 8; day.windowToHour = 12;
        Rig r2;
        r2.eng.setRules({day});
        r2.eng.process(aircraft("A", 1));
        CHECK(r2.sink.count() == 1, "10:15 is inside 8 to 12");
    }
    {   // geofences
        Rig r;
        Rule home = simple("Home", "adsb.aircraft", "", notify("h"));
        home.geo = Rule::GeoCircle; home.geoHome = true; home.geoRadiusKm = 20; home.geoEnterOnly = true; home.cooldownKey = "hex";
        r.eng.setRules({home});
        r.eng.process(aircraft("A", 1, 55.0, 25.28));          // 35 km
        CHECK(r.sink.count() == 0, "outside");
        r.eng.process(aircraft("A", 1, 54.75, 25.28));         // 7 km: enters
        r.eng.process(aircraft("A", 1, 54.76, 25.28));         // still inside: no second firing
        CHECK(r.sink.count() == 1, "fires on entering only: %zu", r.sink.count());
        r.eng.process(aircraft("A", 1, 56.0, 25.28));          // leaves
        r.eng.process(aircraft("A", 1, 54.75, 25.28));         // enters again
        CHECK(r.sink.count() == 2, "fires again after leaving and re-entering: %zu", r.sink.count());
        r.eng.process(aircraft("B", 1, 54.75, 25.28));
        CHECK(r.sink.count() == 3, "each aircraft has its own state");
        Event nopos("adsb.aircraft"); nopos.str("hex", "C");
        r.eng.process(nopos);
        CHECK(r.sink.count() == 3, "an event without a position never matches a geofence");
        {   // #403: the notification shows the distance although the event carried none
            Rig rd;
            Rule d = simple("D", "adsb.aircraft", "", notify("{{hex}} {{distance_km}} km"));
            rd.eng.setRules({d});
            rd.eng.process(aircraft("A", 1, 54.75, 25.28));
            CHECK(rd.sink.count() == 1 && rd.sink.calls[0].second.at("body").find("km") != std::string::npos &&
                  rd.sink.calls[0].second.at("body").find("{{") == std::string::npos && rd.sink.calls[0].second.at("body").find("A  km") == std::string::npos && rd.sink.calls[0].second.at("body").find("A 6.9") == 0,
                  "distance_km filled from the station: %s", rd.sink.count() ? rd.sink.calls[0].second.at("body").c_str() : "");
            Rule c = simple("C", "adsb.aircraft", "distance_km < 20", notify("c"));
            Rig rc; rc.eng.setRules({c});
            rc.eng.process(aircraft("A", 1, 54.75, 25.28));
            rc.eng.process(aircraft("B", 1, 56.0, 25.28));
            CHECK(rc.sink.count() == 1, "distance_km condition works without a distance in the event: %zu", rc.sink.count());
        }
        Rig r2;
        r2.eng.setStation(false, 0, 0);
        r2.eng.setRules({home});
        r2.eng.process(aircraft("A", 1, 54.75, 25.28));
        CHECK(r2.sink.count() == 0, "home geofence without a receiver position never matches");
        Rig r3;
        Rule poly = simple("Poly", "adsb.aircraft", "", notify("p"));
        poly.geo = Rule::GeoPolygon; poly.geoPolygon = "54,25; 54,26; 55,26; 55,25"; poly.geoInside = false;
        r3.eng.setRules({poly});
        r3.eng.process(aircraft("A", 1, 54.5, 25.5));
        CHECK(r3.sink.count() == 0, "inside, rule wants outside");
        r3.eng.process(aircraft("A", 1, 53.0, 25.5));
        CHECK(r3.sink.count() == 1, "outside");
        Rig r4;
        Rule circle = simple("Circle", "adsb.aircraft", "", notify("c"));
        circle.geo = Rule::GeoCircle; circle.geoLat = 0; circle.geoLon = 0; circle.geoRadiusKm = 100;
        r4.eng.setRules({circle});
        r4.eng.process(aircraft("A", 1, 0.5, 0.5));
        r4.eng.process(aircraft("A", 1, 2.0, 0.5));
        CHECK(r4.sink.count() == 1, "fixed circle");
    }
    {   // errors in conditions never fire, never crash
        Rig r;
        r.eng.setRules({simple("Div", "adsb.aircraft", "1 / (squawk - 1000) > 0", notify("x")), simple("Syntax", "adsb.aircraft", "squawk ==", notify("y"))});
        r.eng.process(aircraft("A", 1000));
        r.eng.process(aircraft("A", 1001));
        CHECK(r.sink.count() == 1 && r.eng.stats("div").errors == 1, "division by zero counted as error, other event fires");
        CHECK(r.eng.stats("syntax").errors >= 1 && !r.eng.stats("syntax").lastError.empty(), "syntax error recorded");
    }
    {   // disabled, wants mask
        Rig r;
        Rule x = simple("Off", "adsb.aircraft", "", notify("x"));
        x.enabled = false;
        r.eng.setRules({x});
        CHECK(!r.eng.wants(FAdsb), "disabled rule: not wanted");
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count() == 0, "disabled rule does not fire");
        x.enabled = true;
        r.eng.setRules({x});
        CHECK(r.eng.wants(FAdsb) && !r.eng.wants(FPager), "mask follows the rules");
        r.eng.setRules({simple("Star", "*", "", notify("x"))});
        CHECK(r.eng.wants(FPager) && r.eng.wants(FTime), "* wants every family");
        CHECK(familyOf("adsb.new") == FAdsb && familyOf("nope") == -1, "familyOf");
        for (const EventInfo& e : eventCatalog()) CHECK(familyOf(e.type) >= 0, "family for %s", e.type);
    }
    {   // actions: several, in order, failure reported, log file, run gate
        Rig r;
        const fs::path dir = fs::temp_directory_path() / "onair_automation_test_logs";
        fs::remove_all(dir);
        r.eng.setLogDir(dir.string());
        Rule x = simple("Multi", "pager.message", "", notify("first"));
        ActionSpec log; log.type = "log"; log.p["file"] = "pager-alerts";
        ActionSpec snd; snd.type = "sound";
        ActionSpec rec; rec.type = "record_iq"; rec.p["seconds"] = "30";
        x.actions = {notify("first"), log, snd, rec};
        r.eng.setRules({x});
        Event p("pager.message"); p.at(kT0, "POCSAG", 152e6); p.str("text", "FIRE, \"big\"\nnow").num("address", 42);
        r.eng.process(p);
        CHECK(r.sink.calls.size() == 3 && r.sink.calls[0].first == "notify" && r.sink.calls[1].first == "sound" && r.sink.calls[2].first == "record_iq", "actions in order; log is done by the engine");
        std::string csv;
        { std::ifstream f(dir / "pager-alerts.csv"); std::stringstream ss; ss << f.rdbuf(); csv = ss.str(); }   // closed before the folder is removed (Windows)
        CHECK(csv.find("time,rule,type,mode,freq_mhz,fields\n") == 0 && csv.find("2026-10-17T10:15:00Z,Multi,pager.message,POCSAG,152.0000,") != std::string::npos, "csv header and row: %s", csv.c_str());
        CHECK(csv.find("\"address=42; text=FIRE, \"\"big\"\"\nnow\"") != std::string::npos, "csv quoting of the field list: %s", csv.c_str());
        r.sink.fail = true;
        r.eng.process(p);
        CHECK(!r.eng.firings().back().ok && r.eng.stats("multi").errors == 1, "failed action reported");
        r.sink.fail = false;
        // bad log names
        for (const char* bad : {"../x", "a/b", ".hidden", "", "a b"}) {
            ActionSpec l; l.type = "log"; l.p["file"] = bad;
            Rule lr = simple("LogBad", "pager.message", "", l);
            Rig r2; r2.eng.setLogDir(dir.string()); r2.eng.setRules({lr}); r2.eng.process(p);
            CHECK(!r2.eng.firings().empty() && !r2.eng.firings()[0].ok, "log file name '%s' refused", bad);
        }
        CHECK(!fs::exists(dir.parent_path() / "x.csv"), "no file written outside the folder");
        fs::remove_all(dir);
    }
    {   // run: off by default, needs the switch and the confirmation, arguments stay arguments
        Rig r;
        ActionSpec run; run.type = "run"; run.p["program"] = "/usr/bin/say"; run.p["args"] = "-v\nAlert {{text}}";
        Rule x = simple("Run", "pager.message", "", run);
        r.eng.setRules({x});
        Event p("pager.message"); p.str("text", "line1\n-rf /etc/passwd");
        r.eng.process(p);
        CHECK(r.sink.count("run") == 0, "run does nothing by default");
        r.eng.setAllowRun(true);
        r.eng.process(p);
        CHECK(r.sink.count("run") == 0, "allowed but not confirmed: nothing");
        r.eng.setConfirmedRuns({runFingerprint(x)});
        r.eng.process(p);
        CHECK(r.sink.count("run") == 1, "confirmed and allowed: runs");
        if (r.sink.count("run") == 1) {
            const auto& c = r.sink.calls[0].second;
            CHECK(c.at("program") == "/usr/bin/say" && c.at("args") == "-v\nAlert line1 -rf /etc/passwd", "a newline in a value does not make a new argument: %s", c.at("args").c_str());
        }
        Rule changed = x; changed.actions[0].p["program"] = "/bin/rm";
        CHECK(runFingerprint(changed) != runFingerprint(x), "changing the program voids the confirmation");
        r.eng.setRules({changed});
        r.eng.process(p);
        CHECK(r.sink.count("run") == 1, "changed program not run");
        r.eng.setAllowRun(false);
        r.eng.setRules({x});
        r.eng.process(p);
        CHECK(r.sink.count("run") == 1, "global switch off stops it again");
        Recipe imp; std::string err;
        Recipe orig; orig.rule = x; orig.rule.enabled = true;
        CHECK(recipeFromText(recipeToText(orig), imp, err), "run recipe imports: %s", err.c_str());
        CHECK(!imp.rule.enabled, "an imported run recipe is switched off");
    }
    {   // dry run and fire-with-sample
        Rig r;
        Rule x = simple("Test", "adsb.aircraft", "squawk == 7700", notify("{{callsign}}"));
        const Event s = sampleEvent("adsb.aircraft", kT0);
        FireReport rep = r.eng.test(x, s, false);
        CHECK(!rep.matched && rep.why.find("false") != std::string::npos, "sample squawk 1000: no match (%s)", rep.why.c_str());
        Event hit = s; hit.num("squawk", 7700);
        rep = r.eng.test(x, hit, false);
        CHECK(rep.matched && rep.actions.size() == 1 && rep.actions[0].find("body=KLM1023") != std::string::npos, "dry run shows the rendered action: %s", rep.actions.empty() ? "" : rep.actions[0].c_str());
        CHECK(r.sink.count() == 0 && r.eng.firings().empty(), "dry run touches nothing");
        rep = r.eng.test(x, hit, true);
        CHECK(r.sink.count() == 1 && r.eng.firings().size() == 1 && r.eng.firings()[0].test, "execute does the action and logs it as a test");
        CHECK(!r.eng.test(x, sampleEvent("pager.message", kT0), false).matched, "wrong type");
        Rule bad = x; bad.when = "((";
        CHECK(r.eng.test(bad, hit, false).why.find("character") != std::string::npos, "syntax error shown");
    }
    {   // {{distance_km}} works for an ADS-B aircraft in a notification (issue #403)
        Rig r;
        r.eng.setStation(true, 54.6872, 25.2797);
        Rule x = simple("Near", "adsb.aircraft", "has('lat')", notify("{{callsign}} {{distance_km}} km"));
        x.geo = Rule::GeoCircle; x.geoHome = true; x.geoRadiusKm = 20; x.geoEnterOnly = true;
        r.eng.setRules({x});
        Event a("adsb.aircraft");
        a.str("hex", "ABC123").str("callsign", "TST1").num("lat", 54.7872).num("lon", 25.2797);
        r.eng.process(a);
        CHECK(r.sink.count("notify") == 1, "aircraft within 20 km notified");
        const std::string body = r.sink.calls.back().second.at("body");
        CHECK(body.find("TST1 11.") == 0 && body.find("km") != std::string::npos, "distance in the notification: %s", body.c_str());
    }
    {   // time.minute and sonde landing
        Rig r;
        r.eng.setRules({simple("Tick", "time.minute", "hour() == 10 and minute() == 15 and weekday() == 6", notify("tick")), simple("Landed", "sonde.landed", "distance_km <= 50", notify("{{serial}} {{distance_km}}"))});
        r.eng.tick(kT0);
        CHECK(r.sink.count("notify") == 1, "time.minute event made by the clock");
        r.eng.tick(kT0 + 20);
        CHECK(r.sink.count("notify") == 1, "once per minute");
        Event s("sonde.update");
        s.str("serial", "V1").str("type", "RS41").num("lat", 54.9).num("lon", 25.3).num("alt_m", 20000).num("vspeed_ms", -30);
        r.eng.process(s);
        s.num("alt_m", 2500);
        r.now += 600; r.eng.process(s);
        r.eng.tick(r.now + 30);
        CHECK(r.sink.count("notify") == 1, "not landed while frames arrive");
        r.eng.tick(r.now + 100);
        CHECK(r.sink.count("notify") == 2, "landed after 90 s of silence while falling");
        r.eng.tick(r.now + 200);
        CHECK(r.sink.count("notify") == 2, "landed once");
        CHECK(r.sink.calls.back().second.at("body").find("V1 ") == 0, "landing event fields: %s", r.sink.calls.back().second.at("body").c_str());
    }
    {   // queue, thread
        Rig r;
        r.eng.setRules({simple("Q", "pager.message", "", notify("x"))});
        Event ignored("adsb.aircraft");
        r.eng.submit(std::move(ignored));
        r.eng.drain();
        CHECK(r.sink.count() == 0, "an event nobody wants is dropped at the door");
        for (int i = 0; i < 5000; i++) r.eng.submit(Event("pager.message"));
        CHECK(r.eng.dropped() == 5000 - 2048, "bounded queue drops the rest: %llu", (unsigned long long)r.eng.dropped());
        r.eng.drain();
        CHECK(r.sink.count() == 2048, "queued events are processed: %zu", r.sink.count());
        Rig t;
        t.eng.setRules({simple("T", "pager.message", "", notify("x"))});
        t.eng.start();
        Event p("pager.message");
        t.eng.submit(std::move(p));
        for (int i = 0; i < 100 && t.sink.count() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        t.eng.stop();
        CHECK(t.sink.count() == 1, "the thread processes events");
    }
    {   // catalog
        std::set<std::string> types;
        for (const EventInfo& e : eventCatalog()) {
            CHECK(types.insert(e.type).second, "duplicate event type %s", e.type);
            std::set<std::string> names;
            for (const FieldInfo& f : e.fields) CHECK(names.insert(f.name).second && (f.kind == 'n' || f.kind == 's' || f.kind == 'b'), "field %s.%s", e.type, f.name);
            const Event s = sampleEvent(e.type, kT0);
            CHECK(s.f.size() == e.fields.size() && s.type == e.type, "sample event of %s", e.type);
        }
        CHECK(triggerMatches("adsb.*", "adsb.new") && !triggerMatches("adsb.*", "adsbx.new") && triggerMatches("*", "x.y") && !triggerMatches("", "x"), "triggerMatches");
    }
}

struct ScriptRig : ScriptApi {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> acts;
    std::vector<std::string> notes;
    bool st = true;
    bool act(const std::string& k, const std::map<std::string, std::string>& p, std::string& d) override { acts.push_back({k, p}); d = "ok"; return true; }
    void note(const std::string& t) override { notes.push_back(t); }
    bool station(double& la, double& lo) override { la = 54.6872; lo = 25.2797; return st; }
    double now() override { return kT0; }
    int tzOffsetSec() override { return 3600; }
};
Script scr(const std::string& code) { Script s; s.id = "t"; s.name = "t"; s.code = code; return s; }

void testScripts() {
    {   // the normal way
        ScriptRig api;
        ScriptHost h(scr(
            "function on_event(e)\n"
            "  if e.squawk == 7700 and e.callsign:starts_with('tst') then\n"
            "    notify('Mayday ' .. e.callsign, e.hex)\n"
            "    webhook('https://example.org/x', {callsign = e.callsign, list = {1, 2, 3}, n = e.squawk})\n"
            "    log_row('rows', e.hex, e.alt_ft, 'a,b')\n"
            "    record_iq(30)\n"
            "    tune(118.1e6, 'AM')\n"
            "    log('seen ' .. string.format('%.1f', distance_km(e.lat, e.lon)) .. ' hour ' .. hour())\n"
            "  end\n"
            "end\n"), &api);
        CHECK(h.loaded(), "script loads: %s", h.error().c_str());
        CHECK(h.onEvent(aircraft("4840D6", 7700)), "on_event runs: %s", h.error().c_str());
        CHECK(api.acts.size() == 5 && api.acts[0].first == "notify" && api.acts[0].second.at("title") == "Mayday TST1", "notify from Lua");
        json::Value v; std::string err;
        CHECK(api.acts[1].first == "webhook" && json::parse(api.acts[1].second.at("body"), v, err) && v.str("callsign") == "TST1" && v.get("list") && v.get("list")->a.size() == 3, "webhook table becomes JSON: %s", api.acts[1].second.at("body").c_str());
        CHECK(api.acts[2].first == "log" && api.acts[2].second.at("cells") == "4840D6\n12000\na,b", "log_row cells");
        CHECK(api.acts[3].second.at("seconds") == "30" && api.acts[4].second.at("freq_mhz") == "118.100000" && api.acts[4].second.at("mode") == "AM", "record_iq and tune");
        CHECK(api.notes.size() == 1 && api.notes[0].find("seen 0.") == 0 && api.notes[0].find("hour 11") != std::string::npos, "log, distance_km, hour: %s", api.notes.empty() ? "" : api.notes[0].c_str());
        api.st = false;
        api.acts.clear(); api.notes.clear();
        ScriptHost h2(scr("function on_event(e) log(tostring(distance_km(1, 2))) end"), &api);
        h2.onEvent(aircraft("A", 1));
        CHECK(api.notes.size() == 1 && api.notes[0] == "nil", "distance_km is nil without a station");
    }
    {   // load problems
        ScriptRig api;
        ScriptHost a(scr("function on_event(e) "), &api);
        CHECK(!a.loaded() && a.error().find("script") != std::string::npos, "syntax error shown: %s", a.error().c_str());
        ScriptHost b(scr("x = 1"), &api);
        CHECK(!b.loaded() && b.error().find("on_event") != std::string::npos, "no on_event: %s", b.error().c_str());
        ScriptHost c(scr(std::string("\x1b") + "Lua\x54\x00\x19\x93\r\n\x1a\n"), &api);
        CHECK(!c.loaded() && c.error().find("compiled") != std::string::npos, "bytecode refused");
        ScriptHost d(scr("error('boom at load')"), &api);
        CHECK(!d.loaded(), "error while loading");
        CHECK(!checkScriptSyntax("function (").empty() && checkScriptSyntax("function on_event(e) end").empty(), "checkScriptSyntax");
    }
    {   // the sandbox: nothing dangerous exists
        ScriptRig api;
        const char* names[] = {"os", "io", "package", "require", "debug", "load", "loadstring", "dofile", "loadfile", "collectgarbage", "pcall", "xpcall", "getmetatable", "setmetatable", "coroutine", "rawequal"};
        std::string code = "function on_event(e)\n  local leaked = {}\n";
        for (const char* n : names) code += std::string("  if ") + n + " ~= nil then leaked[#leaked+1] = '" + n + "' end\n";
        code += "  if string.dump or string.match or string.gmatch or string.gsub or string.pack then leaked[#leaked+1] = 'string' end\n"
                "  if _G.os or _ENV.io or _G['require'] then leaked[#leaked+1] = 'G' end\n"
                "  if #leaked > 0 then error('leaked: ' .. table.concat(leaked, ',')) end\n  log('clean')\nend\n";
        ScriptHost h(scr(code), &api);
        CHECK(h.loaded() && h.onEvent(aircraft("A", 1)), "sandbox has no dangerous globals: %s", h.error().c_str());
        auto blocked = [&](const char* body, const char* expect = nullptr) {
            ScriptRig a2;
            ScriptHost s(scr(std::string("function on_event(e)\n") + body + "\nend"), &a2);
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = s.loaded() && s.onEvent(aircraft("A", 1));
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (sec > 3) { CHECK(false, "'%s' took %.1f s", body, sec); }
            if (ok) { CHECK(false, "'%s' was allowed", body); return; }
            if (expect) CHECK(s.error().find(expect) != std::string::npos, "'%s' gave '%s', expected '%s'", body, s.error().c_str(), expect);
            // the host stays usable
            CHECK(true, "");
        };
        blocked("os.execute('echo x')");
        blocked("io.open('/etc/passwd')");
        blocked("require('os')");
        blocked("local f = load('return 1') f()");
        blocked("dofile('/etc/passwd')");
        blocked("loadstring('x=1')()");
        blocked("debug.sethook()");
        blocked("string.dump(on_event)");
        blocked("('x'):match('x')");
        blocked("return ('x'):gsub('x', 'y')");
        blocked("setmetatable(_G, {})");
        blocked("getmetatable('').__index.dump(on_event)");
        blocked("while true do end", "instructions");
        blocked("local n = 0 while true do n = n + 1 end", "instructions");
        blocked("repeat until false", "instructions");
        blocked("local t = {} for i = 1, 1e9 do t[#t + 1] = ('x'):rep(100) .. i end");
        blocked("local s = ('x'):rep(1e9)", "memory");
        blocked("local s = ('x'):rep(1e7) local t = {} for i = 1, 100 do t[i] = s .. i end", "memory");
        blocked("local function f() return f() + 1 end f()");
        blocked("local function f(n) return f(n + 1) end return f(1)", "");
        blocked("string.rep('x', 1e8, ',')", "memory");
        blocked("table.concat(setmetatable({}, {__index = function() return 'x' end}), ',', 1, 1e9)");
        blocked("local t = {} t[1e300] = 1 error('x')");
        blocked("local a = {} for i = 1, 1e6 do a[i] = i end table.sort(a, function(x, y) return true end)");
    }
    {   // actions limit, run does not exist, errors do not kill the script
        ScriptRig api;
        ScriptHost h(scr("function on_event(e)\n  if e.bad then error('bad event') end\n  for i = 1, 30 do notify('n', tostring(i)) end\n  run('x')\nend"), &api);
        CHECK(!h.onEvent(aircraft("A", 1)), "run() is not defined");
        Event ok = aircraft("A", 1);
        // 'run' error happens after 30 notify calls: all go to the API which the engine limits to 20 per call; the fake does not limit, so only check the error text
        CHECK(h.error().find("run") != std::string::npos, "error names the missing function: %s", h.error().c_str());
        ScriptHost h2(scr("function on_event(e) if e.bad then error('bad event') end log('fine') end"), &api);
        Event bad = aircraft("A", 1); bad.flag("bad", true);
        api.notes.clear();
        CHECK(!h2.onEvent(bad) && h2.error().find("bad event") != std::string::npos, "script error reported: %s", h2.error().c_str());
        CHECK(h2.onEvent(aircraft("A", 1)) && api.notes.size() == 1, "script keeps running after an error");
        // state survives between events
        ScriptHost h3(scr("local n = 0 function on_event(e) n = n + 1 log(tostring(n)) end"), &api);
        api.notes.clear();
        h3.onEvent(aircraft("A", 1)); h3.onEvent(aircraft("A", 1));
        CHECK(api.notes.size() == 2 && api.notes[1] == "2", "script variables persist");
        // an event field cannot reach outside
        ScriptHost h4(scr("function on_event(e) log(e.text) log(tostring(e.freq)) end"), &api);
        Event t = aircraft("A", 1); t.str("text", "x\0y");
        api.notes.clear();
        CHECK(h4.onEvent(t), "event text with odd bytes");
    }
    {   // through the engine: rate limit of 20 actions per call, script firing in the log
        Rig r;
        Script s = scr("function on_event(e)\n  for i = 1, 30 do notify('n', tostring(i)) end\nend");
        s.id = "spam"; s.name = "Spam"; s.events = {"adsb.*"};
        r.eng.setScripts({s});
        CHECK(r.eng.wants(FAdsb) && !r.eng.wants(FPager), "script events set the mask");
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count("notify") == 20, "20 actions per call at most: %zu", r.sink.count("notify"));
        CHECK(!r.eng.firings().empty() && r.eng.firings().back().rule == "Spam", "script firing logged");
        r.eng.setMaxActionsPerMinute(5);
        r.now += 61;
        r.eng.process(aircraft("A", 1));
        CHECK(r.sink.count("notify") == 25, "the global cap applies to scripts: %zu", r.sink.count("notify"));
        Script w = scr("function on_event(e) webhook('ftp://x/y', 'z') webhook('https://ok.example/', 'z') log_row('../evil', 1) end");
        w.id = "w"; w.name = "W";
        Rig r2;
        r2.eng.setScripts({w});
        r2.eng.process(aircraft("A", 1));
        CHECK(r2.sink.count("webhook") == 1, "script webhooks need http(s): %zu", r2.sink.count("webhook"));
        Script bad = scr("function on_event(e) end x = ");
        bad.id = "bad"; bad.name = "Bad";
        r2.eng.setScripts({bad});
        CHECK(!r2.eng.scriptError("bad").empty(), "engine keeps the load error");
        Script loop = scr("function on_event(e) while true do end end");
        loop.id = "loop"; loop.name = "Loop";
        r2.eng.setScripts({loop});
        const auto t0 = std::chrono::steady_clock::now();
        r2.eng.process(aircraft("A", 1));
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2, "an endless script does not hang the engine");
        CHECK(r2.eng.stats("loop").errors == 1 && !r2.eng.firings().empty() && !r2.eng.firings().back().ok, "its error is logged");
    }
}
}  // namespace

int main() {
    testExpr();
    testExprHostile();
    testGeometry();
    testTemplates();
    testWebhook();
    testRecipes();
    testConditionRows();
    testEngine();
    testScripts();
    return testSummary("automation");
}
