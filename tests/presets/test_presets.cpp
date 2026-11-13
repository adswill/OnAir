// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Presets: serialisation round trip, forward-compatible unknown fields, secrets and location left out, validation, radio-compatible
// apply, the store (names, rename, delete, favourites, recents, default), the file and the built-ins.
#include "dect2/presets/preset.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace dect2::presets;
namespace J = dect2::automation::json;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Preset sample() {
    Preset p; p.name = "My FM"; p.mode = "fm";
    p.values["freqMhz"] = Value::n(93.9);
    p.values["autoGain"] = Value::f(true);
    p.values["antenna"] = Value::t("RX");
    p.values["opt.deemph"] = Value::n(50);
    p.values["loc.lat"] = Value::n(54.7);
    p.values["loc.lon"] = Value::n(25.3);
    return p;
}

int main() {
    // round trip, location only when ticked
    {
        const Preset p = sample();
        Preset q; std::string err;
        CHECK(importText(exportText(p), q, err));
        CHECK(q.name == "My FM" && q.mode == "fm");
        CHECK(q.values.count("freqMhz") && q.values["freqMhz"].num == 93.9);
        CHECK(q.values["autoGain"].flag && q.values["antenna"].text == "RX" && q.values["opt.deemph"].num == 50);
        CHECK(!q.values.count("loc.lat") && !q.values.count("loc.lon"));
        Preset r;
        CHECK(importText(exportText(p, ExportOptions{true}), r, err));
        CHECK(r.values.count("loc.lat") && r.values["loc.lat"].num == 54.7);
    }
    // forward compatibility: a newer file with unknown fields and keys survives import -> export
    {
        const std::string text = R"({"format":"onair-preset","version":7,"name":"X","mode":"airband","future":{"a":1},
            "values":{"freqMhz":121.5,"newThing":3,"audio.volume":0.5}})";
        Preset p; std::string err;
        CHECK(importText(text, p, err));
        CHECK(p.version == 7);
        CHECK(p.values.size() == 2);
        CHECK(p.extra.get("future") && p.extraValues.get("newThing"));
        J::Value j = J::Value::object(); std::string e2;
        CHECK(J::parse(exportText(p), j, e2));
        CHECK(j.get("future") && j.get("values") && j.get("values")->get("newThing"));
        CHECK(j.num("version") == 7);
        Preset edited = sample(); mergeUnknown(edited, p);
        CHECK(edited.extra.get("future") && edited.extraValues.get("newThing") && edited.version == 7);
        CHECK(applyPreset(p, RadioCaps{}).apply.count("newThing") == 0);
    }
    // secrets never go in or out
    {
        Preset p = sample();
        p.values["opt.apiKey"] = Value::t("abc");
        p.extra = J::Value::object(); p.extra.set("api_token", J::Value::string("zzz")); p.extra.set("note", J::Value::string("hi"));
        const std::string t = exportText(p, ExportOptions{true});
        CHECK(t.find("abc") == std::string::npos && t.find("zzz") == std::string::npos && t.find("hi") != std::string::npos);
        Preset q; std::string err;
        CHECK(importText(R"({"format":"onair-preset","version":1,"name":"S","mode":"fm","password":"p","values":{"opt.token":"t","freqMhz":100}})", q, err));
        CHECK(!q.values.count("opt.token") && !q.extra.get("password") && q.values.count("freqMhz"));
    }
    // damaged input
    {
        Preset q; std::string err;
        CHECK(!importText("not json", q, err));
        CHECK(!importText(R"({"format":"other"})", q, err));
        CHECK(!importText(R"({"format":"onair-preset","version":1,"name":"","mode":"fm"})", q, err));
        CHECK(importText(R"({"format":"onair-preset","version":1,"name":"A","mode":"fm","values":{"freqMhz":-5,"gainDb":"loud","squelchDb":-70}})", q, err));
        CHECK(q.values.size() == 1 && q.values.count("squelchDb") && !err.empty());
    }
    // validation and apply
    {
        CHECK(validate("freqMhz", Value::n(100)).empty());
        CHECK(!validate("freqMhz", Value::n(-1)).empty());
        CHECK(!validate("freqMhz", Value::n(1e300)).empty());
        CHECK(!validate("freqMhz", Value::t("x")).empty());
        CHECK(!validate("nope", Value::n(1)).empty());
        CHECK(!validate("opt.x", Value::n(NAN)).empty());
        Preset p; p.name = "a"; p.mode = "fm";
        p.values["freqMhz"] = Value::n(2000);
        p.values["gainDb"] = Value::n(30);
        p.values["autoGain"] = Value::f(true);
        p.values["biasTee"] = Value::f(true);
        p.values["antenna"] = Value::t("B");
        p.values["sampleRate"] = Value::n(20e6);
        p.values["squelchDb"] = Value::n(-400);   // out of range
        p.values["radio"] = Value::t("hackrf:123");
        RadioCaps c; c.maxMhz = 1766; c.maxRate = 10e6; c.hasAutoGain = false; c.hasBiasTee = false; c.antennas = {"A"};
        const ApplyResult r = applyPreset(p, c);
        CHECK(r.apply.size() == 1 && r.apply.count("gainDb"));
        CHECK(r.rejected == 1);
        CHECK(r.skipped == 5);
        CHECK(r.notes.size() == 6);
        c.maxMhz = 6000; c.maxRate = 30e6; c.hasAutoGain = true; c.hasBiasTee = true; c.antennas = {"A", "B"};
        CHECK(applyPreset(p, c).apply.size() == 6);   // everything but the bad squelch and the radio hint
    }
    // store
    {
        Store s;
        const std::string a = s.add(sample());
        const std::string b = s.add(sample());
        CHECK(a != b && s.find(a)->name == "My FM" && s.find(b)->name == "My FM 2");
        CHECK(s.rename(b, "Night FM") && !s.rename(a, "Night FM") && !s.rename(a, "") && !s.rename("builtin:fm-broadcast", "x"));
        CHECK(s.setFavourite(b, true) && s.isFavourite(b));
        CHECK(s.forMode("fm").front()->id == b);
        CHECK(s.setDefault("fm", a) && !s.setDefault("airband", a) && s.setDefault("fm", "builtin:fm-broadcast"));
        s.setDefault("fm", a);
        for (int i = 0; i < 12; ++i) s.touch(i % 2 ? a : b);
        CHECK(s.recent.size() == 2 && s.recent[0] == a);
        for (const auto& p : builtinPresets()) s.touch(p.id);
        CHECK(s.recent.size() == Store::kMaxRecent);
        CHECK(!s.remove("builtin:fm-broadcast"));
        // file round trip
        const std::string path = (std::filesystem::temp_directory_path() / "test_presets_store.json").string();
        std::string err;
        CHECK(saveStoreFile(path, s, err));
        Store t;
        CHECK(loadStoreFile(path, t, err));
        CHECK(t.user.size() == 2 && t.defaultFor["fm"] == a && t.isFavourite(b) && t.recent == s.recent);
        CHECK(t.find(a)->values.count("loc.lat"));   // the user's own file keeps their location
        CHECK(s.remove(a) && !s.defaultFor.count("fm") && s.user.size() == 1);
        for (const auto& id : s.recent) CHECK(id != a);
        std::remove(path.c_str());
        Store none; CHECK(loadStoreFile(path + ".missing", none, err) && none.user.empty());
        Store bad; CHECK(!loadStoreText("{", bad, err) && !loadStoreText(R"({"format":"x"})", bad, err));
        // unknown top-level fields kept
        Store u; CHECK(loadStoreText(R"({"format":"onair-presets","version":3,"later":{"x":1},"presets":[]})", u, err));
        CHECK(storeText(u).find("later") != std::string::npos);
    }
    // built-ins: every one is valid and the common modes are covered
    {
        const char* need[] = {"fm", "airband", "marine", "adsb", "general", "meteor", "dab", "dvbt"};
        for (const char* m : need) { bool f = false; for (const auto& p : builtinPresets()) f = f || p.mode == m; CHECK(f); }
        CHECK(builtinPresets().size() >= 9);
        for (const auto& p : builtinPresets()) {
            CHECK(p.builtin && p.id.rfind("builtin:", 0) == 0);
            for (const auto& [k, v] : p.values) { CHECK(validate(k, v).empty()); if (!validate(k, v).empty()) std::printf("  %s %s\n", p.id.c_str(), k.c_str()); }
        }
        CHECK(suggestedFileName(sample()) == "My FM.onairpreset");
        Preset w; w.name = "a/b:c"; CHECK(suggestedFileName(w) == "a_b_c.onairpreset");
    }
    std::printf(fails ? "%d FAILED\n" : "presets: all passed\n", fails);
    return fails ? 1 : 0;
}
