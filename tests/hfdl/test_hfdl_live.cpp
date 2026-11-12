// HFDL nearby ground stations (hfdl_live.h): parsing a saved hfdl.observer list (tests/data/hfdl/hfdl_observer_active.json, 2026-11-12 08:11 UTC;
// no network in the tests), relative and bad times, malformed text, the merge with the squitters (newer list wins, agreeing frequencies carry
// both sources, stale data dropped), the distance and bearing sort, the hints and the tune point.
#include "dect2/hfdl/hfdl_live.h"
#include "dect2/hfdl/hfdl_hop.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::ostringstream s; s << f.rdbuf(); return s.str(); }
static const HfdlLiveStation* findL(const std::vector<HfdlLiveStation>& v, int id) { for (const auto& s : v) if (s.id == id) return &s; return nullptr; }
static const HfdlNearby* findN(const std::vector<HfdlNearby>& v, int id) { for (const auto& s : v) if (s.id == id) return &s; return nullptr; }

int main() {
    const double kNow = 1794471107.0;   // the list's "when"
    // 1: the saved list
    std::vector<HfdlLiveStation> live;
    {
        const std::string j = readFile(std::string(DECT2_SOURCE_DIR) + "/tests/data/hfdl/hfdl_observer_active.json");
        CHECK(j.size() > 1000);
        std::string err;
        CHECK(hfdlParseLiveList(j, kNow, live, &err));
        CHECK(live.size() == 16);
        const auto* b = findL(live, 15);
        CHECK(b && b->name == "Al Muharraq, Bahrain" && b->activeKhz == std::vector<int>({17967, 21982}) && std::fabs(b->updatedSec - 1794470998.0) < 0.5);
        CHECK(b && b->stratum == 4 && b->updateSource == 15);
        const auto* m = findL(live, 2);
        CHECK(m && m->activeKhz == std::vector<int>({10027, 11312, 13324}));
        CHECK(!findL(live, 12));
    }
    // 2: relative time, junk entries, duplicates, out of range frequencies; malformed and wrong-shape text
    {
        std::vector<HfdlLiveStation> v;
        const std::string j = R"({"ground_stations":[{"id":7,"name":"Shannon é","last_updated":-120,"frequencies":{"active":[8942,8942,5547.4,1000,40000,"x"]}},
                                  {"id":0,"name":"none"},{"name":"no id"},{"id":3,"last_updated":0,"frequencies":{}},[1,2],{"id":200}],"when":"x"})";
        CHECK(hfdlParseLiveList(j, 1000.0, v));
        CHECK(v.size() == 2);
        const auto* s = findL(v, 7);
        CHECK(s && std::fabs(s->updatedSec - 880.0) < 1e-6 && s->activeKhz == std::vector<int>({5547, 8942}) && s->name == "Shannon \xc3\xa9");
        const auto* r = findL(v, 3);
        CHECK(r && r->updatedSec == 0 && r->activeKhz.empty());
        std::string err;
        CHECK(!hfdlParseLiveList("{\"ground_stations\": [ {\"id\": 1, ", 0, v, &err) && !err.empty());
        CHECK(!hfdlParseLiveList("<html>502 Bad Gateway</html>", 0, v, &err));
        CHECK(!hfdlParseLiveList("{\"stations\":[]}", 0, v, &err));
        CHECK(!hfdlParseLiveList("", 0, v, &err));
        std::string deep(100, '['); deep += std::string(100, ']');
        CHECK(!hfdlParseLiveList(deep, 0, v, &err));   // bounded depth, no crash
        CHECK(hfdlParseLiveList("{\"ground_stations\":[]}", 0, v) && v.empty());
    }
    // 3: the sort from Dubai and from Vilnius, distances and bearings
    {
        const auto n = hfdlNearby(live, {}, true, 25.2, 55.3, kNow);
        CHECK(n.size() == 16);
        CHECK(n.size() >= 5 && n[0].id == 15 && n[1].id == 14 && n[2].id == 6 && n[3].id == 7 && n[4].id == 8);
        CHECK(std::fabs(n[0].km - 486) < 10 && std::fabs(n[0].bearingDeg - 286) < 3);
        CHECK(std::fabs(n[1].km - 4559) < 40 && std::fabs(n[1].bearingDeg - 31) < 3);
        for (size_t i = 1; i < n.size(); i++) CHECK(n[i].km >= n[i - 1].km);
        CHECK(n[0].source == kHfdlSrcLive && n[0].freqs.size() == 2 && n[0].freqs[0].sources == kHfdlSrcLive);
        const auto v = hfdlNearby(live, {}, true, 54.7, 25.3, kNow);
        CHECK(v.size() >= 3 && v[0].id == 7 && v[1].id == 3 && v[2].id == 15);
        CHECK(std::fabs(v[0].bearingDeg - 278) < 3);
        const auto np = hfdlNearby(live, {}, false, 0, 0, kNow);   // no position: by id, no distance
        CHECK(np.size() == 16 && np[0].id == 1 && np[15].id == 17 && np[0].km < 0);
    }
    // 4: the merge with the squitters
    {
        // a squitter newer than the list: its list is shown, the frequency both name carries both sources
        std::vector<HfdlHeardStation> heard = {{15, 1794471090.0, {13354, 17967}}};
        auto n = hfdlNearby(live, heard, true, 25.2, 55.3, kNow);
        const auto* b = findN(n, 15);
        CHECK(b && b->freqs.size() == 2 && b->freqs[0].khz == 13354 && b->freqs[0].sources == kHfdlSrcSquitter);
        CHECK(b && b->freqs[1].khz == 17967 && b->freqs[1].sources == (kHfdlSrcSquitter | kHfdlSrcLive) && b->source == (kHfdlSrcSquitter | kHfdlSrcLive));
        CHECK(b && std::fabs(b->updatedSec - 1794471090.0) < 0.5);
        // an older squitter: the live list stays, with no frequency in common the source is the live list only
        heard = {{15, 1794470000.0, {8885}}};
        n = hfdlNearby(live, heard, true, 25.2, 55.3, kNow);
        b = findN(n, 15);
        CHECK(b && b->freqs.size() == 2 && b->freqs[0].khz == 17967 && b->source == kHfdlSrcLive);
        // the list rounds a kHz differently: still the same frequency
        heard = {{15, 1794471100.0, {17968}}};
        n = hfdlNearby(live, heard, true, 25.2, 55.3, kNow);
        b = findN(n, 15);
        CHECK(b && b->freqs.size() == 1 && b->freqs[0].sources == (kHfdlSrcSquitter | kHfdlSrcLive));
        // two hours later the live list is stale: only the stations heard now are listed
        const double later = kNow + 7200;
        heard = {{15, later - 40, {11348, 17967}}, {99, later - 10, {8900}}};
        n = hfdlNearby(live, heard, true, 25.2, 55.3, later);
        CHECK(n.size() == 2 && n[0].id == 15 && n[0].source == kHfdlSrcSquitter && n[0].freqs.size() == 2);
        CHECK(n.size() == 2 && n[1].id == 99 && n[1].km < 0 && n[1].name == "station 99");   // unknown station: listed last, no distance
        HfdlNearbyConfig keep; keep.keepStale = true;
        n = hfdlNearby(live, heard, true, 25.2, 55.3, later, keep);
        CHECK(n.size() == 17);
        const auto* st = findN(n, 7);
        CHECK(st && st->freqs.empty() && st->source == 0);
        // a list from a clock far ahead is not "now"
        std::vector<HfdlLiveStation> fut = {live[0]};
        fut[0].updatedSec = kNow + 3600;
        CHECK(hfdlNearby(fut, {}, true, 25.2, 55.3, kNow).empty());
        // nothing at all
        CHECK(hfdlNearby({}, {}, true, 25.2, 55.3, kNow).empty());
    }
    // 5: hints, nearest first; without a position every frequency
    {
        const auto n = hfdlNearby(live, {}, true, 25.2, 55.3, kNow);
        const auto h = hfdlHintKhz(n, 6000, 3);
        CHECK(h == std::vector<int>({17967, 21982, 21990, 5655, 13270}));
        const auto h2 = hfdlHintKhz(n, 100, 2);   // nobody within 100 km: the nearest two
        CHECK(h2 == std::vector<int>({17967, 21982, 21990}));
        const auto np = hfdlNearby(live, {}, false, 0, 0, kNow);
        CHECK(hfdlHintKhz(np).size() >= 25);
    }
    // 6: tune points
    {
        CHECK(std::fabs(hfdlTunePointMhz(2, {8942}, 2e6) - 8.90) < 1e-9);       // the whole band fits
        CHECK(std::fabs(hfdlTunePointMhz(2, {}, 120e3) - 8.90) < 1e-9);         // nothing wanted
        CHECK(std::fabs(hfdlTunePointMhz(2, {13276}, 120e3) - 8.90) < 1e-9);    // wanted elsewhere
        CHECK(std::fabs(hfdlTunePointMhz(2, {8942}, 0) - 8.90) < 1e-9);
        CHECK(hfdlTunePointMhz(-1, {8942}, 120e3) == 0 && hfdlTunePointMhz(99, {8942}, 120e3) == 0);
        const double half = 0.4 * 120.0 - 2.0;
        const double c3 = hfdlTunePointMhz(2, {8942, 8948, 8885}, 120e3) * 1e3;   // all three fit in the span
        CHECK(std::fabs(c3 - 8917.94) < 0.01);
        for (int k : {8942, 8948, 8885}) CHECK(std::fabs(k + 1.44 - c3) <= half);
        const double c1 = hfdlTunePointMhz(2, {8942, 8834}, 120e3) * 1e3;        // too far apart: the first one, kept off the centre
        CHECK(std::fabs(c1 - (8943.44 - 10)) < 0.01 && std::fabs(8943.44 - c1) <= half);
        const double cn = hfdlTunePointMhz(2, {8942}, 20e3) * 1e3;                // a narrow span: centred on the signal
        CHECK(std::fabs(cn - 8943.44) < 0.01);
    }
    printf(fails ? "hfdl_live: %d FAILED\n" : "hfdl_live: all ok\n", fails);
    return fails ? 1 : 0;
}
