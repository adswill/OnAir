// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ham.decode (round 4 #6, #14): the event from a decode and the logbook's marks (a log with QSOs: worked, new on band, new DXCC, new grid),
// the locator to lat/lon, each decode once, CW calls once per signal, and the four JTAlert-style recipes on the engine (fire / quiet / cooldown).
#include "dect2/automation/engine.h"
#include "dect2/automation/recipes.h"
#include "dect2/ham/ham_events.h"
#include "dect2/ham/ham_log.h"
#include "../common/test_util.h"
#include <cmath>
#include <map>
#include <string>
#include <vector>
namespace au = dect2::automation;
using namespace dect2::ham;

namespace {
struct Sink : au::ActionSink {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> calls;
    bool perform(const std::string& k, const std::map<std::string, std::string>& p, std::string& d) override { calls.push_back({k, p}); d = "ok"; return true; }
    int count(const char* k) const { int n = 0; for (auto& c : calls) n += c.first == k; return n; }
};
au::Rule recipe(const char* name, const char* when = nullptr) {
    for (const auto& r : au::builtinRecipes()) if (r.kind == au::Recipe::KindRule && r.rule.name == name) { au::Rule x = r.rule; x.enabled = true; if (when) x.when = when; return x; }
    CHECK(false, "recipe %s missing", name);
    return au::Rule();
}
LogQso qso(const char* call, const char* grid, const char* band, const char* mode, int dxcc) { LogQso q; q.call = call; q.grid = grid; q.band = band; q.mode = mode; q.dxcc = dxcc; return q; }
}  // namespace

int main() {
    WorkedIndex wi;
    wi.build({qso("DL1ABC", "JO62", "20m", "FT8", 230), qso("LY2XX", "KO24", "40m", "FT8", 146)});
    auto heard = [](const char* call, const char* grid, const char* msg, const char* band, bool cq, const char* country) {
        HamHeard h; h.call = call; h.grid = grid; h.msg = msg; h.mode = "FT8"; h.band = band; h.cq = cq; h.country = country; h.freqHz = 14.074e6; h.utc = 1'800'000'000; h.snrDb = -12; h.dt = 0.2f; h.audioHz = 1200;
        return h;
    };
    // the marks through the event
    const au::Event e1 = hamDecodeEvent(heard("DL1ABC", "JO62", "CQ DL1ABC JO62", "20m", true, "Germany"), wi.mark("DL1ABC", "JO62", "20m", "FT8", "230"));
    CHECK(e1.f.at("worked").b && e1.f.at("worked_band").b && !e1.f.at("new_dxcc").b && !e1.f.at("new_grid").b && e1.f.at("call").s == "DL1ABC" && e1.f.at("from").s == "DL1ABC" &&
          e1.f.at("text").s == "CQ DL1ABC JO62" && std::fabs(e1.f.at("dial_mhz").n - 14.074) < 1e-9 && e1.mode == "FT8", "worked station");
    CHECK(std::fabs(e1.f.at("lat").n - 52.5) < 1e-9 && std::fabs(e1.f.at("lon").n - 13) < 1e-9, "JO62 middle %.2f %.2f", e1.f.at("lat").n, e1.f.at("lon").n);
    const au::Event e2 = hamDecodeEvent(heard("LY3ZZ", "KO25", "CQ LY3ZZ KO25", "20m", true, "Lithuania"), wi.mark("LY3ZZ", "KO25", "20m", "FT8", "146"));
    CHECK(!e2.f.at("worked").b && !e2.f.at("new_dxcc").b && e2.f.at("new_dxcc_band").b && e2.f.at("new_grid").b, "Lithuania worked on 40 m only: new on 20 m, new grid");
    const au::Event e3 = hamDecodeEvent(heard("VK2AB", "QF56", "CQ VK2AB QF56", "20m", true, "Australia"), wi.mark("VK2AB", "QF56", "20m", "FT8", "150"));
    CHECK(e3.f.at("new_dxcc").b && e3.f.at("new_dxcc_band").b, "new entity");
    const au::Event e4 = hamDecodeEvent(heard("K1ABC", "RR73", "LY2XX K1ABC RR73", "20m", false, ""), DecodeMarks());
    CHECK(!e4.f.count("lat"), "RR73 is not a locator");
    const au::Event e5 = hamDecodeEvent(heard("G4XYZ", "IO91wm", "x", "20m", false, ""), DecodeMarks());
    CHECK(e5.f.count("lat") && std::fabs(e5.f.at("lat").n - 51.5208) < 0.001 && std::fabs(e5.f.at("lon").n - (-0.125)) < 0.001, "6-character locator %.4f %.4f", e5.f.count("lat") ? e5.f.at("lat").n : 0, e5.f.count("lon") ? e5.f.at("lon").n : 0);
    // once
    HamDecodeOnce once;
    CHECK(once.first("A", "FT8", "CQ A", 15.0) && !once.first("A", "FT8", "CQ A", 15.4) && once.first("A", "FT8", "CQ A", 30.0) && once.first("A", "FT4", "CQ A", 15.0), "each decode once");
    HamCallOnce cw;
    CHECK(cw.first("3|DL1ABC", 100) && !cw.first("3|DL1ABC", 400) && cw.first("4|DL1ABC", 400) && cw.first("3|DL1ABC", 701), "CW: once per signal and call in 10 minutes");
    // the recipes
    struct C { const char* name; const char* when; const au::Event* fire; const au::Event* quiet; };
    au::Event wanted = hamDecodeEvent(heard("3Y0J", "", "CQ 3Y0J", "20m", true, "Bouvet"), DecodeMarks());
    const C cs[] = {{"New DXCC heard", nullptr, &e3, &e2}, {"New on this band", nullptr, &e2, &e3}, {"New grid", nullptr, &e2, &e1}, {"Wanted callsign heard", "", &wanted, &e1}};
    for (const C& c : cs) {
        au::Engine eng; Sink sink; double now = 1'800'000'000;
        eng.setSink(&sink); eng.setClock([&] { return now; }); eng.setMaxActionsPerMinute(0);
        au::Rule r = recipe(c.name, c.when);
        if (std::string(c.name) == "Wanted callsign heard") r.when = "call in ['3Y0J', 'P5DX']";
        CHECK(au::validateRule(r).empty(), "%s validates: %s", c.name, au::validateRule(r).c_str());
        eng.setRules({r});
        eng.process(*c.quiet);
        CHECK(sink.count("notify") == 0, "%s: quiet", c.name);
        eng.process(*c.fire);
        CHECK(sink.count("notify") == 1, "%s: fires", c.name);
        now += 60;
        eng.process(*c.fire);
        CHECK(sink.count("notify") == 1, "%s: cooldown", c.name);
    }
    CHECK(au::familyOf("ham.decode") == au::FHam && au::findEventInfo("ham.decode"), "family and catalog");
    return testSummary("ham_events");
}
