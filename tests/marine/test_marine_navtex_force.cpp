// NAVTEX warnings in force (marine_navtex_force.h): message texts in the forms the coast stations use (UK "WZ" numbers, NAVAREA and
// coastal series "123/26", NAVTEX headers), cancellations, self-expiry, an in-force list, repeats with damaged characters, a damaged
// header, other stations, assumed lifetimes, and the save / load round trip. The texts are written for the test (no real station log).
#include "dect2/marine/marine_navtex_force.h"
#include <string>
#include "../common/test_util.h"
using namespace dect2::marine;

static int kinds(const std::vector<NavtexChange>& v, const char* k) { int n = 0; for (const auto& c : v) if (c.kind == k) n++; return n; }
static bool has(const NavtexInForce& f, char st, const char* hdr) { for (const auto& w : f.inForce(st)) if (w.header == hdr) return true; return false; }

int main() {
    const int64_t t0 = 1768132800;   // 2026-01-11 12:00 UTC
    // the parsers
    CHECK(NavtexInForce::selfExpiry("WRECK\nCANCEL THIS MSG 121800 UTC JAN 26\n", t0) == 1768240800, "dtg %lld", (long long)NavtexInForce::selfExpiry("CANCEL THIS MSG 121800 UTC JAN 26", t0));
    CHECK(NavtexInForce::selfExpiry("CANCEL THIS MSG 151200Z JAN 26.", t0) == 1768478400, "Z form");
    CHECK(NavtexInForce::selfExpiry("CANCEL THIS MESSAGE 020600 UTC FEB 2026", t0) == 1770012000, "4-digit year");
    CHECK(NavtexInForce::selfExpiry("CANCEL WZ 1093", t0) == 0, "not a self-cancel");
    {
        auto c = NavtexInForce::cancelRefs("NAV WARN 130/26\nCANCELS 117/26 AND 118/26. CANCEL EA42 AND WZ 1093.\nCANCEL THIS MSG 121800 UTC JAN 26");
        CHECK(c.count("117/26") && c.count("118/26") && c.count("EA42") && c.count("WZ1093") && !c.count("130/26") && c.size() == 4, "cancel refs %zu", c.size());
        auto o = NavtexInForce::ownRefs("NAVAREA I 081/2026\n111200 UTC JAN 26\nNORTH SEA.");
        CHECK(o.size() == 1 && o[0] == "81/26", "own refs %zu %s", o.size(), o.empty() ? "" : o[0].c_str());
    }

    NavtexInForce f;
    std::vector<NavtexChange> ch;
    f.add('E', 'A', "EA42", "WZ 1093\nENGLISH CHANNEL. DOVER STRAIT.\nSANDETTIE BANK LIGHT BUOY UNLIT.\n", t0, ch);
    f.add('E', 'A', "EA43", "NAV WARN 123/26\n111200 UTC JAN 26\nGULF OF FINLAND. WRECK IN 60-01N 025-30E.\nCANCEL THIS MSG 121800 UTC JAN 26\n", t0 + 60, ch);
    f.add('E', 'B', "EB10", "GALE WARNING\nTHAMES DOVER: SOUTHWEST GALE FORCE 8 IMMINENT\n", t0 + 120, ch);
    f.add('E', 'E', "EE11", "WEATHER FORECAST FOR THE NEXT 24 HOURS\n", t0 + 130, ch);   // a forecast is no warning
    f.add('X', 'A', "XA01", "NAV WARN 123/26\nANOTHER STATION'S WARNING WITH THE SAME NUMBER\n", t0 + 140, ch);
    CHECK(kinds(ch, "new") == 4 && f.total() == 4, "new %d total %zu", kinds(ch, "new"), f.total());
    CHECK(f.inForce('E').size() == 3 && f.inForce('E')[0].title == "WZ 1093", "E has %zu; title %s", f.inForce('E').size(), f.inForce('E').empty() ? "" : f.inForce('E')[0].title.c_str());
    ch.clear();
    // a cancellation by UK number, then one by series number (before the self-expiry)
    f.add('E', 'A', "EA44", "NAV WARN 124/26\nDOVER STRAIT. BUOY RESTORED.\nCANCEL WZ 1093.\n", t0 + 3600, ch);
    CHECK(kinds(ch, "cancelled") == 1 && !has(f, 'E', "EA42") && has(f, 'E', "EA44"), "WZ cancel: %d", kinds(ch, "cancelled"));
    ch.clear();
    f.add('E', 'A', "EA45", "NAV WARN 125/26\nGULF OF FINLAND. WRECK REMOVED.\nCANCEL NAV WARN 123/26\n", t0 + 7200, ch);
    CHECK(kinds(ch, "cancelled") == 1 && !has(f, 'E', "EA43") && has(f, 'X', "XA01"), "series cancel: %d, other station kept %d", kinds(ch, "cancelled"), (int)has(f, 'X', "XA01"));
    ch.clear();
    // a repeat with two damaged characters, and a damaged header
    f.add('E', 'A', "EA44", "NAV WARN 124/26\nDOVER STRAIT. B*OY RESTORED.\nCANCEL WZ 1*93.\n", t0 + 7300, ch);
    f.add('E', 'A', "E*48", "NAV WARN 127/26\nSOMETHING\n", t0 + 7400, ch);
    CHECK(ch.empty() && f.inForce('E').size() == 3, "repeat / damaged header changed %zu, E has %zu", ch.size(), f.inForce('E').size());
    // an in-force list that does not name 126/26
    f.add('E', 'A', "EA47", "NAV WARN 126/26\nNORTH SEA. RIG MOVED.\n", t0 + 8000, ch);
    ch.clear();
    f.add('E', 'A', "EA49", "NAV WARNINGS IN FORCE 120600 UTC JAN 26\n2026 SERIES: 124, 125\nNNNN\n", t0 + 9000, ch);
    CHECK(kinds(ch, "dropped") == 1 && !has(f, 'E', "EA47") && has(f, 'E', "EA45") && !has(f, 'E', "EA49"), "in-force list: dropped %d", kinds(ch, "dropped"));
    ch.clear();
    // the gale warning ages out after 24 h; a self-expiring warning expires
    f.add('E', 'D', "ED50", "SAR\nPERSON MISSING NEAR 51-10N 001-30E\nCANCEL THIS MSG 111900 UTC JAN 26\n", t0 + 9500, ch);
    ch.clear();
    f.tick(t0 + 8 * 3600, ch);
    CHECK(kinds(ch, "expired") == 1 && !has(f, 'E', "ED50") && has(f, 'E', "EB10"), "expiry: %d", kinds(ch, "expired"));
    ch.clear();
    f.tick(t0 + 25 * 3600, ch);
    CHECK(kinds(ch, "aged") == 1 && !has(f, 'E', "EB10"), "aged: %d", kinds(ch, "aged"));
    // save / load
    NavtexInForce g;
    CHECK(g.load(f.save()), "load");
    CHECK(g.total() == f.total() && has(g, 'E', "EA45") && has(g, 'X', "XA01"), "round trip %zu / %zu", g.total(), f.total());
    ch.clear();
    g.add('E', 'A', "EA51", "NAV WARN 128/26\nCANCEL 125/26\n", t0 + 30 * 3600, ch);
    CHECK(kinds(ch, "cancelled") == 1, "cancel after load %d", kinds(ch, "cancelled"));
    return testSummary("marine_navtex_force");
}
