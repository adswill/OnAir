// Map badges, app side (core/src/badges/badges_client.cpp): the shared marketplace session file and every way it can be wrong, the Bearer
// header, the home locator, the /ingest answer (awards, refused tokens, odd or hostile fields), the toast queue (once per badge and tier,
// across restarts, from two threads), the Badges panel model from /api/me (order, progress, tiers) and the panel's next step.
#include "dect2/badges/badges_client.h"
#include "../common/test_util.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;
namespace bd = dect2::badges;

namespace {
const int64_t kNow = 1800000000;
const char* kTok = "eyJ1aWQiOjQyLCJsb2dpbiI6ImFsaWNlIn0.abcdefghijklmnopqrstuvwxyz0123456789_-AB";
fs::path tmpDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / ("onair-badges-test-" + std::string(name) + "-" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(d);
    return d;
}
void write(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }
bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }
}  // namespace

int main() {
    // ---- the marketplace's session.json is the one sign-in
    {
        const fs::path d = tmpDir("sess");
        CHECK(!bd::readSession(d.string(), kNow).valid(kNow), "no file: signed out");
        write(d / "session.json", std::string("{\"token\":\"") + kTok + "\",\"login\":\"alice\",\"expires\":" + std::to_string(kNow + 3600) + "}");
        bd::Session s = bd::readSession(d.string(), kNow);
        CHECK(s.valid(kNow) && s.login == "alice" && s.token == kTok, "valid session read");
        CHECK(bd::authHeader(s, kNow) == std::string("Authorization: Bearer ") + kTok, "bearer header");
        CHECK(bd::authHeader(s, kNow + 3601).empty(), "expired: no header");
        CHECK(!bd::readSession(d.string(), kNow + 3600).valid(kNow + 3600), "expiry is exclusive");
        write(d / "session.json", std::string("{\"token\":\"abc\\r\\nX-Evil: 1") + kTok + "\",\"login\":\"alice\",\"expires\":" + std::to_string(kNow + 3600) + "}");
        CHECK(!bd::readSession(d.string(), kNow).valid(kNow), "a token with CR/LF never reaches a header");
        write(d / "session.json", std::string("{\"token\":\"") + kTok + "\",\"login\":\"al ice\",\"expires\":" + std::to_string(kNow + 3600) + "}");
        CHECK(!bd::readSession(d.string(), kNow).valid(kNow), "odd login refused");
        write(d / "session.json", "{not json");
        CHECK(!bd::readSession(d.string(), kNow).valid(kNow), "broken file: signed out");
        write(d / "session.json", std::string(9000, ' '));
        CHECK(!bd::readSession(d.string(), kNow).valid(kNow), "oversized file ignored");
        CHECK(!bd::sendToken(true, bd::Session{}, kNow), "no session: no token");
        bd::Session ok; ok.token = kTok; ok.login = "alice"; ok.expires = kNow + 10;
        CHECK(bd::sendToken(true, ok, kNow) && !bd::sendToken(false, ok, kNow), "token only with Earn badges on");
        fs::remove_all(d);
    }
    // ---- home locator (sent only by choice, for range)
    {
        CHECK(bd::locator6(54.6872, 25.2797) == "KO24pq", "Vilnius: %s", bd::locator6(54.6872, 25.2797).c_str());
        CHECK(bd::locator6(-33.8688, 151.2093) == "QF56od", "Sydney: %s", bd::locator6(-33.8688, 151.2093).c_str());
        CHECK(bd::locator6(40.7128, -74.0060) == "FN20xr", "New York: %s", bd::locator6(40.7128, -74.0060).c_str());
        CHECK(bd::locator6(90, 180) == "RR99xx", "the corner stays inside the grid: %s", bd::locator6(90, 180).c_str());
        CHECK(bd::locator6(0, 0).empty() && bd::locator6(NAN, 1).empty() && bd::locator6(91, 0).empty(), "no locator for unset or bad positions");
    }
    // ---- /ingest answers
    {
        const std::string r = R"({"ok":true,"n":3,"account":{"login":"alice","badges":true,"public":false},"awards":[
            {"id":"plane_spotter","tier":2,"name":"Plane spotter (Silver)","icon":"plane"},
            {"id":"BAD ID","tier":1,"name":"x"},{"id":"first_light","tier":9,"name":"y"},{"id":"long_range","tier":1,"name":""}]})";
        const bd::IngestAccount a = bd::parseIngestReply(r);
        CHECK(a.present && a.login == "alice" && a.badgesOn && !a.isPublic, "account read");
        CHECK(a.awards.size() == 1 && a.awards[0].id == "plane_spotter" && a.awards[0].tier == 2, "only well-formed awards: %zu", a.awards.size());
        CHECK(bd::toastText(a.awards[0]) == "Badge earned: Plane spotter (Silver)", "toast text");
        const bd::IngestAccount anon = bd::parseIngestReply(R"({"ok":true,"n":3,"load":{"pct":2}})");
        CHECK(!anon.present && anon.awards.empty() && anon.error.empty(), "signed-out answer: nothing");
        const bd::IngestAccount bad = bd::parseIngestReply(R"({"ok":true,"n":3,"account_error":"Your OnAir sign-in has expired"})");
        CHECK(!bad.present && has(bad.error, "expired"), "refused token reported");
        CHECK(!bd::parseIngestReply("<html>").present && !bd::parseIngestReply("").present, "garbage");
        const bd::IngestAccount n = bd::parseIngestReply(R"({"account":{"login":"a","badges":true,"note":"duplicate"}})");
        CHECK(n.note == "duplicate", "why a post earned nothing");
    }
    // ---- toast queue: once per badge and tier, across restarts, two threads
    {
        const fs::path d = tmpDir("toast");
        const std::string f = (d / "badges-shown.txt").string();
        bd::Award a{"plane_spotter", "Plane spotter (Silver)", "plane", 2}, b{"first_light", "First light", "spark", 1};
        {
            bd::ToastQueue q(f);
            q.push({a, b, a});
            CHECK(q.pending() == 2, "duplicates dropped: %zu", q.pending());
            bd::Award x;
            CHECK(q.pop(x) && x.id == "plane_spotter" && q.pop(x) && x.id == "first_light" && !q.pop(x), "order kept");
            q.push({a});
            CHECK(q.pending() == 0, "never twice");
        }
        {
            bd::ToastQueue q2(f);
            q2.push({a, bd::Award{"plane_spotter", "Plane spotter (Gold)", "plane", 3}});
            CHECK(q2.pending() == 1, "after a restart only the new tier: %zu", q2.pending());
        }
        bd::ToastQueue q3;
        std::thread t1([&] { for (int i = 0; i < 200; i++) q3.push({bd::Award{"text_reader", "T", "acars", 1 + i % 4}}); });
        std::thread t2([&] { bd::Award x; for (int i = 0; i < 200; i++) q3.pop(x); });
        t1.join(); t2.join();
        bd::Award x; int left = 0; while (q3.pop(x)) left++;
        CHECK(left <= 4, "at most the four tiers once: %d", left);
        fs::remove_all(d);
    }
    // ---- Badges panel model from /api/me
    {
        const std::string me = R"({"signedIn":true,"login":"alice","badges":true,"public":true,"by":"AliceRx","apps":1,"profile":{
          "login":"alice","stats":{"hours":12.5,"days":9,"streak":3,"bestStreak":5,"objects":420,"rangeKm":287.4,"countries":["DE","LT","PL"],
            "counts":{"aircraft":300,"vessel":80,"sonde":2,"aprs":30,"sat":4,"ham":3,"fix":1,"acars":0}},
          "badges":[
            {"id":"plane_spotter","name":"Plane spotter","desc":"Different aircraft","icon":"plane","unit":"","tiers":[100,1000,10000,100000],"tier":1,"earned":[{"tier":1,"at":1000}],"value":300},
            {"id":"airtime","name":"On the air","desc":"Hours","icon":"clock","unit":"h","tiers":[10,100,1000,5000],"tier":1,"earned":[{"tier":1,"at":2000}],"value":12.5},
            {"id":"first_light","name":"First light","desc":"First share","icon":"spark","unit":"","tiers":[1],"tier":1,"earned":[{"tier":1,"at":500}],"value":1},
            {"id":"sonde_hunter","name":"Sonde hunter","desc":"Sondes","icon":"sonde","unit":"","tiers":[1,10,50,250],"tier":1,"earned":[{"tier":1,"at":3000}],"value":2},
            {"id":"long_range","name":"Long range","desc":"Range","icon":"range","unit":"km","tiers":[100,200,300,400],"tier":2,"earned":[{"tier":1,"at":100},{"tier":2,"at":4000}],"value":287.4},
            {"id":"night_owl","name":"Night owl","desc":"Night","icon":"moon","unit":"h","tiers":[5,25,100,500],"tier":0,"earned":[],"value":4},
            {"id":"rare_catch","name":"Rare catch","desc":"Rare","icon":"star","unit":"","tiers":[1,3,5,8],"tier":0,"earned":[],"value":0},
            {"id":"Bad-Id","name":"x","tiers":[1],"tier":1}]}})";
        const bd::Profile p = bd::parseMe(me);
        CHECK(p.ok && p.signedIn && p.login == "alice" && p.badgesOn && p.isPublic && p.by == "AliceRx" && p.apps == 1, "account fields");
        CHECK(p.aircraft == 300 && p.ships == 80 && p.countries == 3 && p.objects == 420 && std::fabs(p.hours - 12.5) < 1e-9, "stats");
        CHECK(p.badges.size() == 7, "bad id dropped: %zu", p.badges.size());
        CHECK(p.earnedCount() == 5, "earned: %d", p.earnedCount());
        CHECK(p.badges[0].id == "long_range" && p.badges[0].name == "Long range (Silver)", "highest tier first: %s", p.badges[0].name.c_str());
        CHECK(p.badges[1].id == "sonde_hunter", "then newest: %s", p.badges[1].id.c_str());
        CHECK(p.badges[4].id == "first_light" && p.badges[4].name == "First light" && p.badges[4].progress == "Top tier", "a one-tier badge has no tier word");
        CHECK(p.badges[5].id == "night_owl" && std::fabs(p.badges[5].frac - 0.8) < 1e-9 && p.badges[5].progress == "4 h of 5 h", "locked, by progress: %s", p.badges[5].progress.c_str());
        const bd::BadgeRow& ps = p.badges[3].id == "plane_spotter" ? p.badges[3] : p.badges[2];
        CHECK(ps.id == "plane_spotter" && ps.progress == "300 of 1,000" && std::fabs(ps.frac - 200.0 / 900.0) < 1e-9, "progress to the next tier: %s", ps.progress.c_str());
        CHECK(p.badges[0].progress == "287.4 km of 300 km", "range progress: %s", p.badges[0].progress.c_str());
        const bd::Profile out = bd::parseMe(R"({"signedIn":false,"configured":true,"web":false})");
        CHECK(out.ok && !out.signedIn && out.configured, "signed out");
        CHECK(!bd::parseMe("garbage").ok, "unreadable");
        // the next step
        CHECK(bd::panelStep(false, nullptr, false, false) == bd::PanelStep::SignIn, "sign in first");
        const bd::Profile nc = bd::parseMe(R"({"signedIn":false,"configured":false})");
        CHECK(bd::panelStep(true, &nc, true, true) == bd::PanelStep::NotConfigured, "server not set up");
        CHECK(bd::panelStep(true, &out, true, true) == bd::PanelStep::SignIn, "server forgot the sign-in");
        bd::Profile off = p; off.badgesOn = false;
        CHECK(bd::panelStep(true, &off, true, true) == bd::PanelStep::TurnOnBadges, "badges off on the server");
        CHECK(bd::panelStep(true, &p, false, true) == bd::PanelStep::TurnOnBadges, "badges off in the app");
        CHECK(bd::panelStep(true, &p, true, false) == bd::PanelStep::TurnOnSharing, "sharing off");
        CHECK(bd::panelStep(true, &p, true, true) == bd::PanelStep::Earning, "earning");
        CHECK(std::strlen(bd::panelStepText(bd::PanelStep::Earning)) > 10, "text");
    }
    // ---- request bodies and links
    {
        CHECK(bd::settingsBody(1, -1, nullptr) == "{\"badges\":true}", "%s", bd::settingsBody(1, -1, nullptr).c_str());
        const std::string nm = "Kris \"RX\"";
        CHECK(bd::settingsBody(-1, 0, &nm) == "{\"by\":\"Kris \\\"RX\\\"\",\"public\":false}", "%s", bd::settingsBody(-1, 0, &nm).c_str());
        CHECK(bd::profileUrl("https://map.on-air.dev/", "alice") == "https://map.on-air.dev/u/alice", "profile link");
        CHECK(bd::profileUrl("https://map.on-air.dev", "../x") == "https://map.on-air.dev/leaders", "odd login: the leaderboard");
        CHECK(bd::fmtValue(1234567, "") == "1,234,567", "thousands: %s", bd::fmtValue(1234567, "").c_str());
        CHECK(bd::fmtValue(3.75, "h") == "3.8 h", "one decimal: %s", bd::fmtValue(3.75, "h").c_str());
        CHECK(bd::fmtValue(-1500, "") == "-1,500", "negative");
    }
    return testSummary("badges_client");
}
