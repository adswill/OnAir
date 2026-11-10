// image.done recipes (round 4 #13): "Post my Meteor pass to Discord" builds the Discord message from the event, only for Meteor pictures, once
// per picture; "Picture finished" for any source; the event is in the catalog and the image family.
#include "dect2/automation/engine.h"
#include "dect2/automation/json_mini.h"
#include "dect2/automation/recipes.h"
#include "dect2/automation/rules.h"
#include "../common/test_util.h"
#include <map>
#include <string>
#include <vector>
namespace au = dect2::automation;

struct Sink : au::ActionSink {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> calls;
    bool perform(const std::string& k, const std::map<std::string, std::string>& p, std::string& d) override { calls.push_back({k, p}); d = "ok"; return true; }
};

static au::Rule recipe(const char* name) {
    for (const auto& r : au::builtinRecipes()) if (r.kind == au::Recipe::KindRule && r.rule.name == name) { au::Rule x = r.rule; x.enabled = true; return x; }
    CHECK(false, "recipe %s missing", name);
    return au::Rule();
}

static au::Event pic(const char* source, const char* sat, const char* key) {
    au::Event e("image.done");
    e.at(1'800'000'000, source, 0);
    e.str("path", std::string("/x/images/") + key + ".png").str("source", source).str("satellite", sat).str("detail", "channels 1 2 3, pass 09:41, max 62 deg").str("key", key).num("width", 1568).num("height", 2200);
    return e;
}

int main() {
    CHECK(au::findEventInfo("image.done") && au::familyOf("image.done") == au::FImage, "catalog and family");
    {
        au::Engine eng; Sink sink; double now = 1'800'000'000;
        eng.setSink(&sink); eng.setClock([&] { return now; }); eng.setMaxActionsPerMinute(0);
        au::Rule r = recipe("Post my Meteor pass to Discord");
        CHECK(au::validateRule(r).empty(), "validates: %s", au::validateRule(r).c_str());
        eng.setRules({r});
        eng.process(pic("SSTV", "", "sstv-1"));
        eng.process(pic("Meteor", "Meteor-M2 4", "m-1"));
        eng.process(pic("Meteor", "Meteor-M2 4", "m-1"));   // the same picture again: once
        eng.process(pic("HRPT", "METEOR-M2 3", "h-1"));     // satellite name, other case
        size_t hooks = 0;
        for (const auto& c : sink.calls) if (c.first == "webhook") hooks++;
        CHECK(hooks == 2, "two posts (%zu)", hooks);
        // the message the webhook would send
        const auto& sp = r.actions[0];
        std::string url, body, err;
        CHECK(au::buildWebhook(sp, pic("Meteor", "Meteor-M2 4", "m-1"), r.name, url, body, err), "builds: %s", err.c_str());
        au::json::Value v;
        CHECK(au::json::parse(body, v, err) && v.str("content") == "New Meteor-M2 4 picture: channels 1 2 3, pass 09:41, max 62 deg, 1568 x 2200" && url.find("discord.com/api/webhooks/") != std::string::npos,
              "Discord content: %s", body.c_str());
    }
    {
        au::Engine eng; Sink sink; double now = 1'800'000'000;
        eng.setSink(&sink); eng.setClock([&] { return now; }); eng.setMaxActionsPerMinute(0);
        eng.setRules({recipe("Picture finished")});
        eng.process(pic("SSTV", "", "sstv-1"));
        eng.process(pic("Fax", "", "fax-1"));
        CHECK(sink.calls.size() == 2 && sink.calls[0].first == "notify" && sink.calls[0].second["body"].find("/x/images/sstv-1.png") != std::string::npos, "notify with the path");
    }
    return testSummary("meteor_imagedone");
}
