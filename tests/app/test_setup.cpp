// The setup wizard v2 (app/ui/setup_logic.h): the place search, the mapping from a place or a position to the country, ITU region, band
// plan, FM step, DAB band III, TV plan and time zone for several countries, the interests, and who gets the wizard.
#include "../../app/ui/setup_logic.h"
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using namespace setup;
    // the tables: unique codes, valid regions, band plans that exist in the data file
    std::stringstream ss;
    { std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/data/bandplans/builtin.json"); ss << f.rdbuf(); }
    const std::string plans = ss.str();
    CHECK(!plans.empty(), "builtin.json is readable");
    int nk = 0, nc = 0;
    const Country* K = countries(&nk);
    const City* C = cities(&nc);
    std::set<std::string> codes;
    for (int i = 0; i < nk; i++) {
        CHECK(codes.insert(K[i].code).second, "duplicate country %s", K[i].code);
        CHECK(K[i].itu >= 1 && K[i].itu <= 3, "%s: ITU region", K[i].code);
        CHECK(K[i].region >= 0 && K[i].region < learn::regionCount(), "%s: learn region", K[i].code);
        CHECK(*K[i].tz && std::strchr(K[i].tz, '/'), "%s: time zone", K[i].code);
        CHECK(K[i].tvMhz == 6 || K[i].tvMhz == 7 || K[i].tvMhz == 8, "%s: TV channel width", K[i].code);
        if (*K[i].bandPlan) CHECK(plans.find(std::string("\"id\":\"") + K[i].bandPlan + "\"") != std::string::npos, "%s: band plan %s exists", K[i].code, K[i].bandPlan);
    }
    // the TV channel plan of every country (core tvplan.h): there is one, and where the plan lists the country its UHF width is the country's
    for (int i = 0; i < nk; i++) {
        const Derived d = derive(&K[i], K[i].lat, K[i].lon, true);
        const dect2::tvplan::Plan* tp = dect2::tvplan::find(d.tvPlan);
        CHECK(tp != nullptr, "%s: TV channel plan '%s' exists", K[i].code, d.tvPlan.c_str());
        if (!tp) continue;
        bool listed = false;
        for (const std::string& cc : tp->countries) if (cc == K[i].code) listed = true;
        if (listed) {
            const dect2::tvplan::Channel& top = tp->channels.back();   // the highest channel is UHF in every digital plan
            CHECK((int)std::lround(top.bwHz / 1e6) == K[i].tvMhz, "%s: plan %s UHF width %g vs %d", K[i].code, tp->id.c_str(), top.bwHz / 1e6, K[i].tvMhz);
        }
    }
    {
        CHECK(derive(countryByCode("LT"), 0, 0, true).tvPlan == "eu" && derive(countryByCode("US"), 0, 0, true).tvPlan == "us" && derive(countryByCode("JP"), 0, 0, true).tvPlan == "jp"
              && derive(countryByCode("CN"), 0, 0, true).tvPlan == "cn" && derive(countryByCode("BR"), 0, 0, true).tvPlan == "br" && derive(countryByCode("AU"), 0, 0, true).tvPlan == "au"
              && derive(countryByCode("KR"), 0, 0, true).tvPlan == "kr" && derive(countryByCode("GB"), 0, 0, true).tvPlan == "eu", "TV channel plan per country");
        CHECK(!deriveSkipped().tvPlan.empty(), "skipped setup: still a default plan");
    }
    for (int i = 0; i < nc; i++) {
        const Country* c = countryByCode(C[i].country);
        CHECK(c != nullptr, "city %s: country %s", C[i].name, C[i].country);
        // every city is inside its own country's table entry, near enough that a position search finds the same country
        const Country* at = countryAt(C[i].lat, C[i].lon);
        CHECK(at && std::string(at->code) == C[i].country, "city %s maps back to %s", C[i].name, C[i].country);
    }

    // the owner's case: Lithuania is region 1
    Derived lt = deriveFromPlace(search("vilnius").at(0));
    CHECK(lt.known && lt.country == "LT" && lt.itu == 1, "Vilnius: Lithuania, region 1");
    CHECK(lt.bandPlan == "lt" && lt.tz == "Europe/Vilnius" && lt.fmStepKhz == 100 && lt.dab3 && lt.tv == "DVB-T2" && lt.tvMhz == 8, "Lithuania presets");
    CHECK(lt.havePosition && std::fabs(lt.lat - 54.69) < 0.01, "the receiver position is the city");

    Derived us = deriveFromPlace(search("chicago").at(0));
    CHECK(us.country == "US" && us.itu == 2 && us.fmStepKhz == 200 && !us.dab3 && us.tv == "ATSC" && us.tvMhz == 6 && us.bandPlan == "us", "United States");
    CHECK(learn::regions(nullptr)[us.region].fmDeemphUs == 75, "United States: 75 us de-emphasis");

    Derived gb = deriveFromPlace(search("Manchester").at(0));
    CHECK(gb.country == "GB" && gb.itu == 1 && gb.bandPlan == "gb" && gb.dab3 && gb.tz == "Europe/London", "United Kingdom");

    Derived au = deriveFromPlace(search("perth").at(0));
    CHECK(au.country == "AU" && au.itu == 3 && au.tv == "DVB-T" && au.tvMhz == 7 && au.dab3 && au.bandPlan == "au", "Australia");

    Derived jp = deriveFromPlace(search("tokyo").at(0));
    CHECK(jp.country == "JP" && jp.itu == 3 && jp.tv == "ISDB-T" && jp.bandPlan == "jp" && jp.tz == "Asia/Tokyo" && !jp.dab3, "Japan");
    CHECK(learn::regions(nullptr)[jp.region].fmHiMhz <= 95.0, "Japan: the FM band ends at 95 MHz");

    Derived br = deriveFromPlace(search("sao paulo").at(0));
    CHECK(br.country == "BR" && br.itu == 2 && br.fmStepKhz == 200 && br.bandPlan == "br", "Brazil");

    Derived cn = deriveFromPlace(search("shanghai").at(0));
    CHECK(cn.country == "CN" && cn.itu == 3 && cn.tv == "DTMB" && cn.bandPlan == "cn", "China");

    Derived ae = deriveFromPlace(search("dubai").at(0));
    CHECK(ae.country == "AE" && ae.itu == 1 && ae.bandPlan == "ae" && ae.tz == "Asia/Dubai", "United Arab Emirates");

    Derived in = deriveFromPlace(search("mumbai").at(0));
    CHECK(in.country == "IN" && in.itu == 3 && in.bandPlan == "in" && in.tz == "Asia/Kolkata", "India");

    Derived za = deriveFromPlace(search("cape town").at(0));
    CHECK(za.country == "ZA" && za.itu == 1 && za.bandPlan == "za", "South Africa");

    // a country picked as a whole, and by its code
    CHECK(search("lithuania").at(0).country == "LT" && !search("lithuania").at(0).isCity, "a country by name");
    CHECK(search("nz").at(0).country == "NZ", "a country by its code");

    // search: prefix matches first, case and spaces ignored, nothing for rubbish
    CHECK(search("  KAUN ").at(0).name == "Kaunas", "prefix, case, spaces");
    CHECK(search("xyzzy").empty() && search("").empty(), "no match, no query");
    CHECK(search("a", 5).size() <= 5, "limit");
    { auto r = search("ber"); CHECK(!r.empty() && r[0].name == "Berlin", "prefix match comes before a match inside a name"); }

    // a position (locator or coordinates) -> the country: near a city, near a country point, in the middle of nowhere
    CHECK(deriveFromPosition(54.68, 25.28).country == "LT", "54.68, 25.28 is Lithuania");
    CHECK(deriveFromPosition(52.0, 5.0).country == "NL", "52, 5 is the Netherlands");
    CHECK(deriveFromPosition(34.0, -118.0).itu == 2, "Los Angeles area is region 2");
    CHECK(deriveFromPosition(-34.0, 151.0).itu == 3, "Sydney area is region 3");
    { Derived mid = deriveFromPosition(0.0, -40.0); CHECK(!mid.known && mid.itu == 2 && mid.fmStepKhz == 200, "mid-ocean: the region by longitude only, no country"); }
    { Derived x = deriveFromPosition(-5.0, 90.0); CHECK(!x.known && x.itu == 3, "far east, no country: region 3"); }
    CHECK(ituRegionAt(50, 10) == 1 && ituRegionAt(40, -100) == 2 && ituRegionAt(35, 135) == 3, "ITU region by longitude");
    { Derived sk = deriveSkipped(); CHECK(!sk.known && !sk.havePosition && sk.itu == 1 && sk.tz.empty() && sk.bandPlan.empty(), "skipped: nothing set"); }

    // interests
    CHECK(std::string(startModeFor(0)) == "specan", "nothing ticked: spectrum scanner");
    CHECK(std::string(startModeFor(1u << IntShips)) == "ais", "ships start in AIS");
    CHECK(std::string(startModeFor((1u << IntShips) | (1u << IntFm))) == "fm", "the first interest in table order picks the start");
    CHECK(std::string(startModeFor(1u << IntEverything)) == "specan", "everything starts in the spectrum scanner");
    for (int i = 0; i < IntCount; i++) CHECK(interests()[i].label && *interests()[i].blurb && *interests()[i].startMode && interests()[i].modes[0], "interest %d is complete", i);
    {
        const auto m = modesFirst((1u << IntAircraft) | (1u << IntShips));
        CHECK(!m.empty() && m[0] == "adsb" && std::find(m.begin(), m.end(), "ais") != m.end(), "aircraft and ships modes listed first");
        std::set<std::string> u(m.begin(), m.end());
        CHECK(u.size() == m.size(), "no repeats");
        CHECK(modesFirst(1u << IntEverything).empty() && modesFirst(0).empty(), "everything and nothing list none first");
    }
    {
        const auto lt1 = starterPresets(1u << IntFm, lt);
        CHECK(!lt1.empty() && std::fabs(lt1[0].mhz - 100.0) < 1e-9 && lt1[0].mode == "fm", "FM starter is the region's FM frequency");
        CHECK(std::fabs(lt1[1].mhz - 88.5) < 1e-9, "FM band start in Europe");
        const auto jp1 = starterPresets(1u << IntFm, jp);
        CHECK(jp1.size() >= 2 && std::fabs(jp1[1].mhz - 77.0) < 1e-9, "FM band start in Japan");
        const auto h1 = starterPresets(1u << IntHam, us), h2 = starterPresets(1u << IntHam, lt), h3 = starterPresets(1u << IntHam, au);
        auto aprs = [](const std::vector<Starter>& v) { for (const Starter& s : v) if (s.mode == "packet") return s.mhz; return 0.0; };
        CHECK(std::fabs(aprs(h1) - 144.39) < 1e-9 && std::fabs(aprs(h2) - 144.8) < 1e-9 && std::fabs(aprs(h3) - 144.64) < 1e-9, "APRS frequency per ITU region");
        CHECK(starterPresets(0, lt).empty(), "nothing ticked: no presets");
        for (const Starter& s : starterPresets((1u << IntCount) - 1, lt)) CHECK(s.mhz > 0 && !s.name.empty(), "preset %s is complete", s.name.c_str());
    }
    CHECK(goalBitsFor(1u << IntFm) == (1u << learn::GoalListen), "FM -> listen lessons");
    CHECK((goalBitsFor(1u << IntShips) & (1u << learn::GoalTrack)) != 0, "ships -> tracking lessons");
    CHECK(goalBitsFor(0) == 0, "no interests, no goals");

    // who gets the wizard
    CHECK(wizardPending(true, false, {}, false), "no settings file: the wizard");
    CHECK(wizardPending(true, false, {"setupStep", "setupRegion"}, false), "a setup closed halfway resumes");
    CHECK(wizardPending(true, false, {"setupStep", "devName", "uiLang"}, false, true), "an unfinished setup resumes even when the app saved its settings at exit");
    CHECK(wizardPending(true, false, {"AppleLanguages", "NSNavLastRootDirectory", "com.apple.x"}, false), "macOS' own keys do not count as prior settings");
    CHECK(!wizardPending(true, false, {"devName", "freqMhz"}, false), "an existing user after an update: no wizard");
    CHECK(!wizardPending(true, false, {"setupStep", "uiLang"}, false), "any other key means an existing user");
    CHECK(!wizardPending(true, true, {"setupStep"}, false, true), "finished wins");
    CHECK(!wizardPending(true, true, {}, false), "setup finished: no wizard");
    CHECK(!wizardPending(false, false, {}, false), "a start with arguments: no wizard");
    CHECK(!wizardPending(true, false, {}, true), "settings set aside after crashes: no wizard");

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("setup tests ok\n");
    return 0;
}
