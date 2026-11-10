// Mesh node watch (round 4 #27): a watched node falls silent (one event, again only after it came back), silence across a restart, the battery
// limit with its 5 % hysteresis, new nodes near you (not those of the first look, not far ones), and the recipes on the engine.
#include "dect2/mesh/mesh_watch.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/recipes.h"
#include "../common/test_util.h"
#include <map>
using namespace dect2;
namespace au = dect2::automation;

static MeshNode node(const char* id, double heard, double bat = -1, bool pos = false, double lat = 0, double lon = 0) {
    MeshNode n; n.id = id; n.longName = std::string("Node ") + id; n.lastHeard = heard; n.batteryPct = bat; n.hasPosition = pos; n.lat = lat; n.lon = lon; return n;
}
static int count(const std::vector<au::Event>& v, const char* t) { int n = 0; for (auto& e : v) n += e.type == t; return n; }
struct Sink : au::ActionSink { int n = 0; bool perform(const std::string& k, const std::map<std::string, std::string>&, std::string& d) override { n += k == "notify"; d = "ok"; return true; } };

int main() {
    MeshWatchConfig c;
    c.watched = {"!hill"};
    c.silentHours = 6; c.batteryPct = 20; c.newWithinKm = 10; c.hasHome = true; c.homeLat = 54.9; c.homeLon = 23.9;
    MeshWatch w;
    const double t0 = 1'800'000'000;
    auto e = w.update({node("!hill", 10, 80), node("!near", 5, -1, true, 54.91, 23.91)}, c, t0);
    CHECK(e.empty(), "first look: nothing (%zu)", e.size());
    e = w.update({node("!hill", 10, 80)}, c, t0 + 5 * 3600);
    CHECK(e.empty(), "5 h: not yet");
    e = w.update({node("!hill", 10, 80)}, c, t0 + 6 * 3600 + 1);
    CHECK(count(e, "lora.node_silent") == 1 && e[0].f["hours"].n >= 6 && !e[0].f["back"].b && e[0].f["name"].s == "Node !hill", "silent after 6 h");
    e = w.update({node("!hill", 10, 80)}, c, t0 + 9 * 3600);
    CHECK(e.empty(), "once");
    e = w.update({node("!hill", 99, 80)}, c, t0 + 9 * 3600 + 5);
    CHECK(count(e, "lora.node_silent") == 1 && e[0].f["back"].b, "back again");
    // battery with hysteresis
    e = w.update({node("!hill", 100, 19)}, c, t0 + 10 * 3600);
    CHECK(count(e, "lora.node_battery") == 1 && e[0].f["battery_pct"].n == 19, "battery low");
    e = w.update({node("!hill", 101, 18)}, c, t0 + 10 * 3600 + 60);
    e = w.update({node("!hill", 102, 22)}, c, t0 + 10 * 3600 + 120);
    e = w.update({node("!hill", 103, 19)}, c, t0 + 10 * 3600 + 180);
    CHECK(e.empty(), "not again until 5 %% above the limit");
    w.update({node("!hill", 104, 26)}, c, t0 + 10 * 3600 + 240);
    e = w.update({node("!hill", 105, 15)}, c, t0 + 10 * 3600 + 300);
    CHECK(count(e, "lora.node_battery") == 1, "again after recovering");
    // new nodes
    e = w.update({node("!hill", 105, 15), node("!new1", 1, -1, true, 54.95, 23.95), node("!far", 1, -1, true, 56.0, 25.0), node("!nopos", 1)}, c, t0 + 11 * 3600);
    CHECK(count(e, "lora.node_new") == 1 && e[0].f["id"].s == "!new1" && e[0].f["distance_km"].n < 10, "one new node near");
    // restart: silence across it
    MeshWatch r;
    r.parse(w.serialize());
    e = r.update({}, c, t0 + 10 * 3600 + 300 + 6 * 3600 + 10);
    CHECK(count(e, "lora.node_silent") == 1, "silent across a restart, the node not in the table");
    // recipes
    for (const char* name : {"Mesh node silent", "Mesh node battery low", "New mesh node near me"}) {
        au::Rule rule;
        bool found = false;
        for (const auto& rc : au::builtinRecipes()) if (rc.kind == au::Recipe::KindRule && rc.rule.name == name) { rule = rc.rule; rule.enabled = true; found = true; }
        CHECK(found && au::validateRule(rule).empty(), "%s", name);
        au::Engine eng; Sink s; double now = t0; eng.setSink(&s); eng.setClock([&] { return now; }); eng.setMaxActionsPerMinute(0); eng.setRules({rule});
        au::Event ev(rule.trigger); ev.at(t0, "Mesh", 0); ev.str("id", "!x").str("name", "X").flag("back", false).num("hours", 7).num("battery_pct", 10).num("distance_km", 3);
        au::Event back = ev; back.flag("back", true);
        if (rule.trigger == "lora.node_silent") eng.process(back);
        eng.process(ev);
        CHECK(s.n == 1, "%s fires once (%d)", name, s.n);
    }
    return testSummary("mesh_watch");
}
